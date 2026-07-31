# M3 — User mode & multitasking

NO_OS M3 takes the NOC shell out of the single ring-0 world and gives it
real processes: user-mode (ring 3) NOC programs running under a preemptive,
PIT-driven round-robin scheduler, with a syscall gate as the only way user
code touches the kernel. The CLI stays first-class: the REPL becomes a
schedulable kernel task, so the shell keeps working while user processes
run alongside it.

This is also the safety foundation for M5 (ML/LLM self-evolution): a
faulting or malicious NOC process kills only itself, never the kernel.

## 1. Privilege model

- Kernel: ring 0, identity-mapped, unchanged.
- User processes: ring 3, running NOC bytecode through a user-space copy
  of the NOC compiler + VM.
- Ring transitions:
  - user -> kernel: `int 0x80` (software interrupt gate, DPL 3).
  - kernel -> user: `iretq` (or syscall return via `iretq`).
  - user <-> kernel on IRQs: hardware stack switch through the TSS.
- User code runs with `IF=1` (preemptible), `IOPL=0`, and no IO bitmap, so
  `in/out/cli/sti` fault (a #GP kills the process, see section 8).

### GDT and TSS

The GDT (currently 5 entries) gains a 64-bit available TSS descriptor:

| Index | Selector | Description |
|---|---|---|
| 0 | 0x00 | null |
| 1 | 0x08 | kernel code, DPL 0 |
| 2 | 0x10 | kernel data, DPL 0 |
| 3 | 0x18 | user code, DPL 3 (exists today) |
| 4 | 0x20 | user data, DPL 3 (exists today) |
| 5 | 0x28 | TSS (16-byte descriptor), loaded with `ltr` |

The TSS is a 104-byte structure. `rsp0` is set to the *running* task's
kernel-stack top and updated on every switch; `iomap_base` is set to the TSS
size (0x68) so no IO permission map exists.

## 2. Memory model

- Kernel: the boot identity map (0..1 GiB, 2 MiB pages) stays supervisor
  (`U/S` clear) and is the kernel address space.
- Each user process has its **own PML4**:
  - `PML4[0]` reuses the kernel's PDPT pointer, so every process sees the
    kernel identity map as supervisor pages (kernel code can reach user
    memory directly; no SMAP/SMEP).
  - `PML4[2]` points at the process's own PDPT/PD/PTs for its user region,
    mapped with 4 KiB pages and `U/S` set.
- Context switch reloads CR3 (single CPU, no shootdowns).

### Fixed per-process user address map

| Virtual | Size | Contents |
|---|---|---|
| `0x80000000` | image size | user binary blob: `.text` + `.rodata` + `.data` (R/O pages then R/W) |
| `0x80C00000` | 32 KiB | script scratch — kernel writes the NOC source here before start |
| `0x80D00000` | 2 MiB | user heap (grows up via the `alloc`/`sbrk` syscall) |
| `0x80F00000` | 64 KiB | user stack (grows down; RSP starts at `0x80F00000`) |

The user region is capped at `0x81000000` (16 MiB).

### New VMM layer (`mm/vmm.c`, `kernel/include/vmm.h`)

```c
u64  vmm_kernel_cr3(void);                          /* boot PML4 */
u64  vmm_new_address_space(void);                   /* PML4, kernel map copied */
void vmm_map(u64 cr3, u64 vaddr, u64 paddr, u8 flags);   /* P|W|U bits */
void vmm_unmap(u64 cr3, u64 vaddr);
u64  vmm_alloc_user_pages(u64 cr3, usize pages, u64 vaddr);  /* pmm-backed */
void vmm_free_address_space(u64 cr3);
```

## 3. Task model & scheduler (`kern/sched.c`, `kernel/include/sched.h`)

```c
#define TASK_MAX 16

typedef enum { TASK_RUNNABLE, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE } task_state_t;

typedef struct {
    u32         pid;
    task_state_t state;
    const char *name;
    u8         *kstack;        /* per-task kernel stack base */
    usize       kstack_size;   /* 16 KiB */
    struct regs ctx;           /* saved interrupt frame */
    u64         cr3;           /* user PML4, or 0 for kernel tasks */
    u64         wake_ticks;    /* block-until-ticks for sleep */
    u8          user;          /* 1 = ring 3 task */
} task_t;
```

- **Context switch is a frame swap.** A PIT tick pushes a full interrupt
  frame (all GPRs + rip/cs/rflags/rsp/ss) onto the running task's kernel
  stack. The scheduler copies that frame into `current->ctx`, picks the
  next runnable task round-robin, sets `TSS.rsp0 = next->kstack_top`
  (user tasks) and loads `next->cr3`, then overwrites the in-place frame
  with `next->ctx`. The ISR epilogue `iretq` then resumes the next task —
  into ring 3 if its CS is `0x18`, into its own kernel stack otherwise.
- **Blocking:** `sleep(ms)` sets `wake_ticks` and BLOCKED; the keyboard
  read path blocks until the keyboard IRQ flags a key. BLOCKED tasks are
  woken when their condition clears; ZOMBIEs are reaped.
- **Preemption:** every PIT tick (100 Hz = 10 ms timeslice).
- Task 0 is the REPL: a kernel task with its own 16 KiB kernel stack. It
  blocks on keyboard reads; user processes run whenever the shell is idle.

## 4. Syscall ABI (`arch/x86_64/syscall.c`, `kernel/include/syscall.h`)

- Vector `0x80`, IDT interrupt gate with DPL 3, kernel selector `0x08`.
- Convention: `rax` = number, `rdi`/`rsi`/`rdx`/`r10` = args 1..4,
  return in `rax`. String args are user pointers; the kernel copies them
  out (they may be page-boundary-crossing) before use.

| # | Signature | Effect |
|---|---|---|
| 0 | `exit(i64 code)` | terminate the process (ZOMBIE) |
| 1 | `putc(i64 c)` | write one char to serial + VGA |
| 2 | `puts(ptr s)` | write a NUL-terminated string |
| 3 | `sleep(i64 ms)` | block until `wake_ticks` |
| 4 | `ticks()` | return `pit_ticks()` |
| 5 | `kbd_poll()` | next key or -1 (non-blocking) |
| 6 | `kbd_wait()` | block until a key, then return it |
| 7 | `alloc(usize n)` | user heap alloc (frame-backed), 0 on failure |
| 8 | `free(ptr p)` | return memory to the user heap |
| 9 | `yield()` | give up the timeslice |

Frame-swap switching also serves `sleep`/`kbd_wait`: the syscall handler
may block the current task and swap in the next frame before the stub's
`iretq`, exactly like the scheduler tick.

## 5. User-mode NOC runtime (`user/`)

The NOC compiler + VM become buildable as a user-space flat binary, so a
process interprets its own program in ring 3.

- Sources: shared `kernel/noc/{lexer,parser,compiler,vm}.c` +
  `user/ucrt0.s` + `user/nocproc.c`.
- **Platform abstraction.** `kernel/noc/*.c` currently call kernel services
  (`printk`, `kmalloc`, `pit_ticks`, `kbd_*`) directly. M3 introduces
  `kernel/include/noc_os.h`:

  ```c
  void *noc_os_alloc(usize n);  void noc_os_free(void *p);
  void  noc_os_putc(char c);    void noc_os_puts(const char *s);
  u64   noc_os_ticks(void);     void noc_os_sleep(u64 ms);
  int   noc_os_kbd_poll(void);  int  noc_os_kbd_wait(void);
  void  noc_os_exit(int code);
  ```

  - Kernel build: `kern/noc_os.c` maps these to `kmalloc`/`printk`/
    `pit_ticks`/`kbd_*` (busy-wait sleep, as today).
  - User build: `user/noc_os.c` maps them to `int 0x80` syscalls (real
    blocking sleep).
- Builtin registry is shared but tagged: kernel-only builtins
  (`Help`, `Version`, `MemInfo`, `FaultTest`, `Reboot`) are not registered
  in the user build; user processes get `Print`, `PrintLn`, `Sleep`,
  `Time`, `KeyGet`, `KeyPressed`, `Alloc`, `Free`, `Len`.
- **Fixed script contract:** `crt0` zeroes `.bss` then `nocproc_main()`
  reads the script from `0x80C00000` (mapped by the kernel), lexes,
  parses, compiles, runs. One binary serves every script.

### Build & embedding

1. Compile the user runtime with the same freestanding flags, link with
   `user/user.ld` at `0x80000000`, `objcopy -O binary` -> `build/nocproc.bin`.
2. `objcopy -I binary -O elf64-x86-64 -B i386` -> `nocproc_blob.o`,
   linked into the kernel; symbols
   `_binary_nocproc_bin_start/_end`.
3. Kernel `proc_spawn(text)` maps the blob at `0x80000000`, writes `text`
   at `0x80C00000`, maps the stack, sets the entry to `0x80000000`,
   creates the task, and starts it with an `iretq` to ring 3.

## 6. NOC builtins for processes

Kernel-side registry additions (REPL):

| Signature | Effect |
|---|---|
| `I64 Spawn(Str code)` | create one user process running `code`; returns PID |
| `U0 Ps()` | list tasks (pid, state, kind) |
| `U0 Demo()` | spawn two looping scripts ("A" / "B") for the acceptance test |

## 7. Faults from user mode

In `isr_dispatch`, faults (exceptions 0..31, #GP, #PF, #UD, ...) with
`r->cs == 0x18` do **not** panic. Instead: log
`process %u (pid) killed: <exception>`, mark the task ZOMBIE, and swap in
the next runnable frame before the ISR returns (same frame-swap mechanism
as the scheduler). Kernel-mode faults keep the existing panic path.

## 8. Build & harness changes (`scripts/build.ps1`)

- Build the user runtime + blob object as part of `Invoke-Build`.
- New kernel sources in `$csrcs`: `kern/sched.c`, `kern/noc_os.c`,
  `mm/vmm.c`, `arch/x86_64/tss.c`, `arch/x86_64/syscall.c`;
  `kern/repl.c` unchanged.
- `noc/repl.c` runs as task 0.
- Harness (`Invoke-Test`) keeps the entire M2.5 sequence, then appends the
  M3 acceptance checks.

## 9. Acceptance

1. Full M2.5 suite still passes (REPL regression, CLI-first).
2. Harness sends `Demo;` (or two `Spawn(...)` lines) and asserts the serial
   log shows **both** `A` and `B` output multiple times and interleaved
   (e.g. `^A` and `^B` each count >= 3, and an `A.*B` and `B.*A` pair
   exist) — proving real preemptive interleaving.
3. After the demo, the REPL is still responsive: `9*9;` -> `81`.
4. `Ps;` lists 3+ tasks (REPL + spawned processes).

## 10. Non-goals (M3)

- No `syscall`/`sysret` (int 0x80 is enough until the JIT milestone).
- No fork/exec, no ELF loader, no loader beyond the embedded flat blob.
- No IPC/pipes/signals beyond exit status.
- No user-mode FPU/SSE (still disabled).
- No SMP, no demand paging or swap (that is M4/M5 territory).
- No ML yet — M5 builds on the sandbox this milestone creates.
