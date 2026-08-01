
# NO_OS

---
```
 _   _     ___     ____     ___      ____ 
| \ | |   / _ \            / _ \    / ___|
|  \| |  | | | |          | | | |   \___ \
| |\  |  | |_| |          | |_| |    ___) |
|_| \_|   \___/ __________ \___/    |____/ 
- an OS that grows a brain, not a manual -
```
---

A from-scratch operating system for x86-64 that learns its users and writes
its own programs. **NO_OS** is TempleOS-inspired in soul — a single language
(**NOC**) is the shell, the app framework, and the training corpus — but built
on a modern foundation: paging, interrupts, real ring-3 processes, and a
journal-lite filesystem, so crashes are debuggable instead of fatal.

The endgame is literal: the OS logs what you type, retrains a model while the
machine idles, drafts NOC code, sandboxes it in a user process, and files the
winners into a versioned corpus. It earns its intelligence one megabyte at a
time — deterministic at the floor, opportunistic with every spare byte of RAM.

> **Current state (M5):** the self-evolution loop is real and boot-tested —
> the model predicts the next command, drafts code that runs in ring 3, and
> the corpus survives reboot. The byte-level transformer is wired in behind
> the same demand-paged model window — integer fixed-point forward pass and
> generation, its size autotuned to whatever RAM the kernel booted with.

## What's in the box

| | |
|---|---|
| **Kernel** | x86-64, long mode, multiboot. Ring 0 C substrate, ~1.2 MB static. |
| **Language** | NOC — a HolyC-flavored, C-like bytecode language. The REPL *is* the shell. |
| **Processes** | Preemptive ring-3 NOC processes, `int 0x80` syscall gate, TSS, per-process address spaces. |
| **Filesystem** | `NO_OSFS`, a RedSea-homage FAT-like FS with dual superblocks, bitmap allocator, IDE PIO driver. |
| **Self-evolution** | Interaction log → idle bigram retrain → `DraftRun` sandboxed drafts → versioned corpus. |
| **Self-tuning** | One binary, any machine — model size, context, layers, and activation budget are derived from total RAM at boot, then autotuned up / degraded down to fit. |
| **Platform** | Runs in QEMU today, GRUB-bootable later. Serial console + VGA. |
| **Ethos** | No networking. No POSIX. No bloat. One engineer (plus an AI) can hold the whole architecture. |

### Milestones

| Milestone | Status |
|---|---|
| **M0** — Toolchain & first boot | done |
| **M1** — Core services (GDT/IDT, PIC/PIT, PS/2, PMM + heap, printk) | done |
| **M2** — NOC bytecode language (lexer, parser, compiler, VM, REPL) | done |
| **M2.5** — Shell & stability (line editor, interruptible VM, harness) | done |
| **M3** — User mode & multitasking (syscall gate, ring-3 NOC processes) | done |
| **M4** — Filesystem (IDE driver, NO_OSFS, persistence across reboot) | done |
| **M5** — ML & self-evolution (predictors, model budget, DraftRun, corpus) | done* |
| **M6** — Stretch: JIT & self-hosting | next |
| **M7** — Stretch: PC speaker & games | |
| **M8** — Graphics (VGA mode 0x12, sprites from NOC) | |

\* M5 ships the full loop with a byte-bigram model, plus the byte-level
transformer's forward pass, generation, and demand-paged weight window —
all boot-tested. Its SGD training (`trans_train`) is the declared next
layer of the same milestone ([`docs/M5-AI.md`](docs/M5-AI.md)).

## The OS that tunes itself

NO_OS is not one OS — it's a spectrum that picks the right capabilities for
the hardware it boots on, with zero configuration. The kernel reads total RAM
once at boot (`pmm_total_bytes()`) and everything else follows:

| Free RAM | What the system becomes |
|---|---|
| **< 16 MB** | Deterministic RTOS — no ML footprint, real-time guarantees |
| **16-32 MB** | Infant — character/bigram predictor learns from your typing |
| **32-64 MB** | Child — byte-level transformer drafts NOC code (the M5 loop) |
| **64-256 MB** | Teen — bigger transformer, JIT, feedback from corrections |
| **> 256 MB** | Adult — continuous learning, retrieval, anticipates your needs |

The model config is derived from RAM, not hardcoded: `trans_init()` sets the
weight budget (`total_ram / 8`) and activation cap (`total_ram / 4`), then
`autotune()` grows context, width, and layers while the budgets allow, and
`degrade()` shrinks them back (ctx → d_ff → layers → d_model) until the
working set fits. On a 64 MiB test image that lands exactly on the M5 spec
model; on a multi-GB bare-metal machine the same kernel silently builds a
much larger transformer. `TransMem(<kb>)` shrinks the activation cap live and
the model gracefully degrades in place; `TransConfig(<layers>, <ctx>)`
reconfigures; `TransInfo` reports the live shape.

> **Bare metal and VM are the same kernel.** NO_OS boots as a multiboot
> kernel with a serial console — the exact same binary runs in QEMU (the
> test harness's world, driven headless via `sendkey`) and on real hardware
> via GRUB. The VMM, demand-paged model window, and per-process address
> spaces are the same structures in both. The learning loop doesn't care
> whether the "machine" is virtual or physical: it watches the interaction
> log, not the hardware.

This is the *reserved-minimum + opportunistic-maximum* memory model from
[`docs/MEMORY-DYNAMIC.md`](docs/MEMORY-DYNAMIC.md): Z0 kernel core and Z1
per-process minimums are guaranteed at boot; Z2 (AI/ML) and Z3 (cache,
elastic growth) consume whatever spare RAM exists and are reclaimable under
pressure. The same discipline is staged as a biological growth metaphor in
[`docs/GROWTH-ROADMAP.md`](docs/GROWTH-ROADMAP.md) — womb → infant → child →
teen → adult — each stage earning the next by demonstrated value, never by
checklist.

## NOC — the language that runs the OS

C-like, immediate, all values 64-bit, functions with default arguments,
and the last expression result prints itself — the calculator-soul of HolyC.

---
```c
Print("Hello"); 40+2;                 // prints: Hello 42
I64 Mul2(I64 x, I64 y = 2) {          // functions with default args
    return x * y;
}
Mul2(40);        // 80
Mul2(40, 5);     // 200
for (I64 i = 0; i < 5; i++) { Print("%d ", i); }   // 0 1 2 3 4
```
---

Bare identifiers auto-call: `Version` runs `Version()`. Every command line is
lexed → parsed → compiled to bytecode → run on a stack VM. Functions defined
at the prompt persist for the session — later lines can call them.

### A quick tour of the shell

---
```
no/os> Print("Hello"); 40+2;
Hello 42
no/os> I64 Mul2(I64 x, I64 y = 2) { return x * y; }
no/os> Mul2(40);
80
no/os> FormatDisk;                    // format the raw IDE disk (NO_OSFS)
no/os> SaveFile("demo.noc", "PrintLn(\"saved from disk!\");");
no/os> ListDir;                       // demo.noc
no/os> Run("demo.noc");               // saved from disk!  (script survives reboot)
no/os> Predict;                       // next-command guess from your history
no/os> Train;                         // force an idle-retrain pass on the log
no/os> DraftRun("PrintLn(\"DRAFT");   // model completes + runs a sandboxed draft
no/os> Ps;                            // pid  state   kind  name
no/os>  0  ready  kern  repl
no/os>  1  ready  user  corp0001.noc
```
---

### Builtin surface

The whole OS is exposed as NOC builtins — no hidden admin prompt:

- **Console & misc** — `Print`, `PrintLn`, `Time`, `Sleep`, `KeyGet`,
  `KeyPressed`, `Alloc`, `Free`, `MemSet`, `MemCpy`, `Len`, `Echo`,
  `Version`, `MemInfo`, `Help`, `FaultTest` (deliberate #UD, trapped cleanly),
  `Reboot`
- **Processes** — `Spawn`, `Ps`, `Demo` (two interleaving ring-3 procs),
  `PageFault` (deliberately fault a user process)
- **Filesystem** — `FormatDisk`, `SaveFile`, `ReadFile`, `DeleteFile`,
  `ListDir`, `StatFile`, `Run`
- **Prediction** — `Predict`, `Hist`, `ClearHist` (command history bigram),
  `PgPred` (page-fault-stream bigram)
- **Model budget** — `ModelBudget`, `ModelCommit`, `ModelTouch`,
  `ModelEvict`, `ModelStats`, `ModelInfo` (demand-paged read-only weight
  pages with a hard per-process cap)
- **Self-evolution** — `LogInfo`, `LogDump`, `LogSave`, `LogClear`
  (interaction log), `Train`, `TrainIdle`, `TrainReset`, `PredictBigram`,
  `DraftRun`, `CorpusInfo`, `CorpusRollback`
- **Transformer** — `TransInfo` (live model shape + budgets), `TransConfig`
  (`layers`, `ctx`; degrades to fit RAM), `TransMem` (activation-cap override,
  graceful degradation live), `TransPredict` (greedy next-byte generation)

The full language spec is in [`docs/NOC.md`](docs/NOC.md).

## Architecture

Everything before M3 ran in ring 0; now the shell sits in a real kernel with
preemptible ring-3 processes. The `noc_os` abstraction layer lets the same
compiler + VM run both in-kernel (REPL) and in user space (`int 0x80`
syscalls) — that duality is what makes sandboxed model output possible.

---
```
kernel/
  arch/x86_64/   boot.s, gdt, idt, isr, tss, syscall gate, coro.s
  drivers/       vga, serial, keyboard, pic, pit, ide (PIO)
  mm/            pmm, heap, vmm, model (budget), pgreg (page predictor)
  fs/            noosfs (format, mount, bitmap alloc, dirs)
  kern/          kernel.c, sched.c, noc_os.c, printk/format, line editor
  noc/           lexer, parser, compiler, vm, repl, exec,
                 predict, train, interact, corpus, trans
  include/       shared headers
user/            ring-3 NOC runtime (noc_os.c, nocproc.c, ucrt0.s, user.ld)
scripts/         build.ps1, run-qemu.ps1
docs/            SPEC, ROADMAP, NOC, M3/M4/M5/M8, ADR/, memory docs
```
---

Key decisions are recorded as ADRs in
[`docs/ADR/`](docs/ADR/) — stack VM over register VM (ADR-0001), no network
stack (ADR-0002), and the `objcopy` ELF-reframe trick that lets QEMU's
`-kernel` boot 64-bit code (ADR-0003). The full architecture is specified in
[`docs/SPEC.md`](docs/SPEC.md).

### Boot

1. BIOS loads the multiboot kernel; the CPU starts in 32-bit protected mode.
2. `boot.s` identity-maps the first 1 GiB with 2 MiB pages, enables
   PAE + long mode, and far-jumps to C.
3. `kmain()` inits serial + VGA, memory, scheduler, filesystem, then drops
   into the NOC REPL.

QEMU's multiboot loader rejects `ELFCLASS64` kernels, so the build links a
true 64-bit ELF and reframes it with `objcopy -O elf32-i386` into a 32-bit
ELF container holding byte-identical 64-bit code. GRUB can use either one.

## Build, run, test

Requirements: **Zig** (`zig cc`), **NASM**, **binutils** (`objcopy`),
**QEMU** (`qemu-system-x86_64`), **PowerShell 7+**. The `Makefile` wraps
`scripts/build.ps1`:

---
```sh
make build     # kernel + ring-3 user runtime + 32 MiB IDE disk image
make run       # boot in QEMU with the disk attached (-serial stdio)
make test      # headless boot; harness drives the keyboard and asserts output
make clean     # wipe build/
```
---

Or call the driver directly:
`pwsh -NoProfile -File scripts/build.ps1 -Action test`.

The test harness (`make test`) is the project's backbone — it boots headless
QEMU, drives the REPL through the QEMU monitor's `sendkey`, and asserts:

- boot self-test, NOC compile/run, bare-command builtins, line editor,
  Ctrl+C, Esc-interrupt, clean fault trapping
- **M3**: two ring-3 processes preemptively interleave `A`/`B` on serial,
  REPL stays responsive, `Ps` lists the tasks
- **M4**: IDE LBA0 read-back, format/save/stat/list/read/delete, then a
  `system_reset` and a re-mount that reads the file back — persistence proven
- **M5**: `Predict` from history, page-predictor, `model_budget` enforcement,
  demand-paged weight evict/refault, and `DraftRun` producing a real
  ring-3 process from a model-completed seed; the transformer weight window
  is read back through a spawned process (header magic, LN gamma/bias),
  write access is refused, and budget pressure denies/evicts/refaults pages

## Docs

- [`docs/ROADMAP.md`](docs/ROADMAP.md) — the milestone plan (spec → implement
  → boot-test → commit)
- [`docs/SPEC.md`](docs/SPEC.md) — architecture specification & decisions
- [`docs/NOC.md`](docs/NOC.md) — the NOC language reference
- [`docs/M3-USERMODE.md`](docs/M3-USERMODE.md) — processes, syscalls, scheduler
- [`docs/M4-FILESYSTEM.md`](docs/M4-FILESYSTEM.md) — NO_OSFS design
- [`docs/M5-AI.md`](docs/M5-AI.md) — the self-evolution architecture
- [`docs/M8-GRAPHICS.md`](docs/M8-GRAPHICS.md) — the GUI-last graphics plan
- [`docs/ADR/`](docs/ADR/) — architecture decision records
- [`docs/MEMORY-BUDGET.md`](docs/MEMORY-BUDGET.md) & [`docs/MEMORY-DYNAMIC.md`](docs/MEMORY-DYNAMIC.md) — the <64 MB discipline
- [`docs/GROWTH-ROADMAP.md`](docs/GROWTH-ROADMAP.md) — womb → adult capability staging

## Why

Most OS/ML stacks need gigabytes before they're useful. NO_OS starts at the
other end: a deterministic microkernel that *becomes* intelligent as RAM
allows — character predictor at 16 MB, byte model at 32 MB, transformer and
retrieval beyond that, all without configuration, all air-gapped by design.

It's an OS that doesn't just run your programs. It watches you, learns, and
eventually writes the programs for you — the way an organism grows into its
environment. That's the whole point.
