# NO_OS

A from-scratch, AI-assisted, TempleOS-inspired operating system for x86-64.
The shell and application language is **NOC** — a custom HolyC-like language
that compiles to bytecode and runs on a stack VM inside the kernel. The end
goal is a system that learns from its users and writes its own programs.

Unlike TempleOS, NO_OS is built on a modern foundation: paging, interrupts,
and clean fault handling — so crashes are debuggable, not fatal.

## Current status

Milestone-driven development: spec -> implement -> boot-test -> commit.
Every milestone ends with a bootable, QEMU-testable acceptance case.

| Milestone | Status |
|---|---|
| **M0** — Toolchain & first boot | done |
| **M1** — Core services (GDT/IDT, PIC/PIT, PS/2, PMM + heap, printk) | done |
| **M2** — NOC bytecode language (lexer, parser, compiler, VM, REPL) | done |
| **M2.5** — Shell & stability (bare-call builtins, line editor, interruptible VM) | done |
| **M3** — User mode & multitasking | done |
| **M4** — Filesystem | next |
| **M5** — ML/LLM & self-evolution | |
| **M6** — Stretch: JIT & self-hosting | |
| **M7** — Stretch: PC speaker & games | |
| **M8** — Graphics (VGA 640x480x16, sprites from NOC) | |

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full milestone plan.

## Features

- **NOC language** — C-like syntax, 64-bit values, functions with default
  arguments, `if`/`while`/`for`, and immediate execution. The REPL *is* the
  shell: `Print("Hello"); 40+2;` compiles, runs, and prints `42`.
- **Interruptible VM** — Esc / Ctrl+C aborts a running NOC program cleanly.
- **Line editor** — history (up/down), cursor movement, Ctrl+C / Esc clears.
- **Safe trap handling** — deliberate faults log `RIP`/`CR2` and halt cleanly
  instead of triple-faulting (`FaultTest`).
- **User mode & multitasking** — ring-3 NOC processes (`dema`/`demb`) with
  per-process PML4 isolation, preemptive round-robin scheduling, and a
  `Ps` task listing (M3).
- **Bare-identifier builtins** — admin commands are pure NOC builtins:
  `Help`, `Version`, `MemInfo`, `Echo`, `FaultTest`, `Reboot`.
- **Serial console** (COM1) for headless testing and harness automation.

## NOC in 30 seconds

```c
I64 Mul2(I64 x, I64 y = 2) { return x * y; }
Mul2(40);        // 80  (default argument fills the tail)
Mul2(40, 5);     // 200

for (I64 i = 0; i < 10; i++) { Print("%d ", i); }
// 0 1 2 3 4 5 6 7 8 9
```

The full language spec lives in [`docs/NOC.md`](docs/NOC.md).

## Architecture

Everything runs in ring 0 until the user-mode milestone (M3); since M3, NOC
processes execute in ring 3 under a per-process PML4. The kernel is written in
C (`zig cc` / clang, freestanding, pure scalar GPR code — no SSE, since boot
code leaves `CR4.OSFXSR` clear). NOC is the shell and application language.

```
kernel/
  arch/x86_64/   boot.s, gdt, idt, isr, syscall, tss
  drivers/       vga, serial, keyboard, pit, pic
  mm/            pmm (frame allocator), heap
  kern/          kernel.c, sched.c (tasks, blob spawn), printk, string, line editor
  noc/           lexer, parser, compiler, vm, repl
  include/       shared headers
scripts/         build.ps1, run-qemu.ps1
docs/            SPEC.md, ROADMAP.md, NOC.md, M3-USERMODE.md, M8-GRAPHICS.md
```

The canonical architecture spec is [`docs/SPEC.md`](docs/SPEC.md).

### Boot

1. BIOS loads the multiboot kernel; CPU starts in 32-bit protected mode.
2. `boot.s` identity-maps the first 1 GiB with 2 MiB pages, enables
   PAE + long mode, and far-jumps to C code.
3. `kmain()` inits serial + VGA, prints the banner, and drops into the NOC
   REPL.

QEMU's multiboot loader rejects `ELFCLASS64` kernels, so the build links a
true 64-bit ELF and reframes it with `objcopy -O elf32-i386` into a 32-bit
ELF container holding byte-identical 64-bit code. GRUB can use either one.

## Build & run

Requirements:

- **Zig** (`zig cc`, clang/LLVM front end)
- **NASM** 2.16+
- **binutils** (`objcopy`)
- **QEMU** (`qemu-system-x86_64`) — `C:\Program Files\qemu` or on `PATH`
- **PowerShell 7+** (build driver is `scripts/build.ps1`)

The `Makefile` wraps the PowerShell driver:

```sh
make build     # compile + link + reframe kernel.elf
make run       # boot kernel.elf in QEMU (-serial stdio)
make test      # headless boot: harness drives keyboard/REPL and asserts output
make clean     # remove build/
```

Or call the script directly: `pwsh -NoProfile -File scripts/build.ps1 -Action test`.

`make test` runs the full acceptance harness against a headless QEMU instance:
boot self-test, NOC compile/run checks, bare-command builtins, line-editor
history and Ctrl+C, Esc-interrupt recovery, and fault trapping.

## Docs

- [`docs/SPEC.md`](docs/SPEC.md) — architecture specification and design decisions
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — milestone plan
- [`docs/NOC.md`](docs/NOC.md) — the NOC language reference
- [`docs/M3-USERMODE.md`](docs/M3-USERMODE.md) — user mode & multitasking spec
- [`docs/M4-FILESYSTEM.md`](docs/M4-FILESYSTEM.md) — filesystem milestone (next)
- [`docs/M8-GRAPHICS.md`](docs/M8-GRAPHICS.md) — graphics milestone design (VGA mode 0x12, sprites)

## Status

M3 (user mode & multitasking) complete — boot, test, and acceptance harness all
green (`TEST PASS`). The "AI-assisted" part of the roadmap — a kernel-resident
micro-transformer that learns from your NOC history, drafts programs, and runs
them sandboxed — is milestone M5, which builds on the M4 filesystem for model
and corpus persistence.
