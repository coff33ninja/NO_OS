# NO_OS — Architecture Specification

NO_OS is a from-scratch, AI-assisted, TempleOS-inspired operating system for
x86-64, developed in a custom HolyC-like language called **NOC**. Unlike
TempleOS, NO_OS builds on a modern foundation: paging, interrupts, and clean
fault handling — so crashes are debuggable, not fatal.

This document is the canonical spec. Milestone definitions live in
`docs/ROADMAP.md`; design changes are recorded here as they are decided.

## 1. Goals

- A complete, self-contained operating system written from scratch.
- A custom C-like language (NOC) as the shell and application language, with
  immediate execution (the REPL is the shell), default arguments, and
  first-class graphics access — the HolyC experience.
- Run on real x86-64 hardware, developed and tested in QEMU.
- Keep the codebase small enough that one engineer plus an AI holds the whole
  architecture in context.

## 2. Non-goals

- Networking (deliberately excluded, like TempleOS).
- POSIX compatibility.
- Portability to non-x86-64 targets.
- A vast standard library.

## 3. Target platform

- CPU: x86-64 (long mode), QEMU `qemu-system-x86_64` for development.
- Boot: Multiboot-compliant kernel (`-kernel kernel.elf`), later an ISO/GRUB
  path for real hardware.
- Display: VGA text mode initially; 640x480 16-color graphics mode in a later
  milestone.
- Debug: serial console (COM1, `-serial stdio`), trap handlers, QEMU monitor.

## 4. Architecture decisions

| Area | Decision |
|---|---|
| Privilege | Everything in ring 0 until the user-mode milestone (M4). |
| Memory | Identity-mapped 2 MiB pages at boot; proper frame allocator + heap in M1. |
| Multitasking | Cooperative in early milestones; preemptive (PIT-driven) at M4. |
| Interrupts | GDT/IDT/PIC from M1; fault handlers log and halt cleanly. |
| Language | C for the kernel substrate; NOC (custom) for shell and apps. |
| Kernel object | ELF64 freestanding, linked at 1 MiB, multiboot header first. |

## 5. NOC language (high-level design)

- C-like syntax; explicit-width types `I8/I16/I32/I64`, `U8/U16/U32/U64`,
  `F64`, `Bool`, `Str`.
- Functions with default arguments: `U0 Greet(Str name, I64 times=1)`.
- Immediate execution: any expression at the shell is compiled and run.
- Compile path: source -> tokens -> AST -> bytecode -> stack VM. An x86-64
  JIT is a later stretch milestone.
- Builtins map to kernel services: console I/O, graphics primitives,
  keyboard, time, memory.
- Full spec written before the M2 implementation milestone.

## 6. Toolchain (verified on this machine)

- Compiler: `zig cc -target x86_64-freestanding` (clang/LLVM, emits ELF64).
- Assembler: NASM 2.16.
- Linker: lld via `zig cc -Wl,-T,linker.ld`.
- Build driver: `scripts/build.ps1` (pwsh). Makefile wraps it.
- Emulator: `C:\Program Files\qemu\qemu-system-x86_64.exe`.

### Freestanding flags: clang 21 ignores `-mgeneral-regs-only`

clang 21 (zig 0.16) still emits SSE/vector instructions under
`-mgeneral-regs-only`: `va_start` saves the whole XMM register file
(`movaps`), and string/format code is vectorized. With CR4.OSFXSR clear
(boot code does not enable SSE) any XMM instruction raises #UD, then double
fault -> triple fault. The build therefore also passes
`-mno-sse -mno-sse2 -mno-mmx` so the kernel is pure scalar GPR code.

## 7. Boot process

1. BIOS loads Multiboot loader (QEMU `-kernel`); CPU in 32-bit protected mode.
2. `boot.s`: multiboot header, stack setup, identity-map first 1 GiB with
   2 MiB pages, enable PAE + long mode, load 64-bit GDT, far jump to C code.
3. `kmain()`: serial + VGA init, print banner, halt loop.

### QEMU `-kernel` and ELF class

QEMU's multiboot loader rejects `ELFCLASS64` kernels. The build therefore
links a true 64-bit ELF (`kernel.elf64`) and reframes it with
`objcopy -O elf32-i386` into `kernel.elf` — a 32-bit ELF container holding
byte-identical 64-bit code. QEMU starts `_start` in 32-bit protected mode;
`boot.s` handles the transition to long mode. GRUB later accepts the 64-bit
ELF directly.

## 8. Source layout

```
kernel/
  arch/x86_64/   boot.s, gdt, idt, interrupts
  drivers/       vga, serial, keyboard, pit
  mm/            paging, alloc
  kern/          main, printk
noc/             lexer, parser, ast, vm, builtins
programs/        *.noc demo scripts
scripts/         build.ps1, run-qemu.ps1
docs/            SPEC.md, ROADMAP.md
```
