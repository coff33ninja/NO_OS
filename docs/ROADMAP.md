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

## M2 — NOC language (centerpiece) (DONE)
- [x] NOC spec written (`docs/NOC.md`)
- [x] Lexer, parser, AST (arena-allocated per line)
- [x] Bytecode compiler + stack VM (all values 64-bit)
- [x] Builtins: console (`Print`/`PrintLn`), memory, keyboard, time
- [x] REPL is the default shell; kernel admin commands become NOC builtins
- [x] Accept: `Print("Hello"); 40+2;` compiles+runs; loops and default args work

## M2.5 — Shell & stability (DONE)
- [x] Drop the legacy admin prompt; admin commands are pure NOC builtins
      (`Help`/`Version`/`MemInfo`/`Echo`/`FaultTest`/`Reboot`)
- [x] Bare-identifier auto-call (`version` == `Version()`) and optional
      trailing `;` on the last statement of a line
- [x] No `NOC: expected ';'` noise for admin commands; clear errors for
      unknown words
- [x] Interruptible NOC execution: Esc / Ctrl+C aborts a running program
- [x] Line editor: history (up/down), cursor (left/right/Home/End/Delete),
      Ctrl+C/Esc clears the line
- [x] Accept: harness drives bare commands, history recall, ctrl-c clear,
      Esc interrupt, and shell recovery

## M3 — Graphics (TempleOS soul) (deferred until shell is stable)
- [ ] Graphics spec written (`docs/M3-GRAPHICS.md`)
- [ ] VGA 640x480 16-color graphics mode (mode 0x12, register-level mode set)
- [ ] 2D primitives (`Pixel`/`Line`/`Rect`/`FillRect`) + bitmap font `Text`
- [ ] Sprite bank drawable from NOC source (`Sprite`)
- [ ] Accept: a NOC program animates a moving sprite (screendump-verified)

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
