# NO_OS — Roadmap

Each milestone ends with bootable, testable acceptance criteria. Work flows:
spec -> implement -> boot-test -> commit.

## M0 — Toolchain & first boot (DONE)
- [x] Verify `zig cc` emits ELF64 freestanding for x86-64
- [x] Repo skeleton, docs, build scripts
- [x] `boot.s`: multiboot header, long mode entry
- [x] `kernel.c`: VGA text + serial banner
- [x] Boots in QEMU; serial log shows banner
      (QEMU `-kernel` refuses ELFCLASS64 multiboot images; two-stage
      link + `objcopy -O elf32-i386` reframes the container.)

## M1 — Core services (DONE)
- [x] GDT, IDT with fault handlers (log RIP/CR2, halt cleanly)
- [x] PIC remap, PIT 100 Hz timer
- [x] PS/2 keyboard driver (scancodes -> ring buffer)
- [x] Physical frame allocator + paging; kernel heap
- [x] `printk` (printf-style) to VGA + serial
- [x] Accept: boot to prompt, keys echo, deliberate fault shows clean trap

## M2 — NOC language (centerpiece)
- [ ] NOC spec written (`docs/NOC.md`)
- [ ] Lexer, parser, AST (arena-allocated per line)
- [ ] Bytecode compiler + stack VM (all values 64-bit)
- [ ] Builtins: console (`Print`/`PrintLn`), memory, keyboard, time
- [ ] REPL is the default shell; kernel admin commands become NOC builtins
- [ ] Accept: `Print("Hello"); 40+2;` compiles+runs; loops and default args work

## M3 — Graphics (TempleOS soul)
- [ ] VGA 640x480 16-color graphics mode
- [ ] 2D primitives; bitmap font rendering
- [ ] Sprites drawable from NOC source
- [ ] Accept: a NOC program animates a moving sprite

## M4 — User mode & multitasking
- [ ] User-mode NOC processes, syscall gate
- [ ] Preemptive PIT scheduling
- [ ] Accept: two NOC scripts preemptively interleave

## M5 — Filesystem
- [ ] QEMU IDE disk driver
- [ ] Own FAT-like FS (RedSea homage)
- [ ] Save/load/run `.noc` scripts across reboot

## M6 — Stretch
- [ ] Bytecode -> x86-64 JIT in a code heap
- [ ] Self-host the NOC compiler
- [ ] PC-speaker audio
- [ ] Games / demo programs
