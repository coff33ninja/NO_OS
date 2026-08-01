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

## M3 — User mode & multitasking ✅
_(CLI-first foundation: sandboxed NOC processes are what make self-evolution safe)_
- [x] M3 spec written (`docs/M3-USERMODE.md`)
- [x] User-mode NOC processes, syscall gate (`int 0x80`)
- [x] Preemptive PIT scheduling (frame-swap context switch, round-robin)
- [x] REPL stays alive as a kernel task while user processes run
- [x] Accept: two NOC scripts preemptively interleave

## M4 — Filesystem
_(persists the NOC corpus and the trained model across reboot)_
- [x] QEMU IDE PIO driver + LBA0 self-test
- [x] Own FAT-like FS (`NO_OSFS`, RedSea homage): dual superblocks, bitmap
      allocator, fixed inode table, linear directory
- [x] Builtin FS commands: `FormatDisk`/`SaveFile`/`ReadFile`/`DeleteFile`/
      `ListDir`/`StatFile`; short-block I/O is padding-safe (no reads past the
      caller's buffer)
- [x] Persistence across reboot: harness saves a file, `system_reset`, and
      verifies the second boot mounts without re-formatting and reads it back
- [x] `Run` builtin executes a saved `.noc` script: reads the file and feeds
      each line to the NOC engine (like the REPL), so function definitions
      defined on one line are callable on later lines; the builtin snapshots
      and restores the VM execution state so the enclosing chunk is unaffected
- [x] Accept: harness covers format/save/stat/list/read/delete plus
      reboot persistence and reports `TEST PASS`

## M5 — ML/LLM & self-evolution (CLI-first)
- [ ] Kernel-side micro-ANN: page/swap prefetcher + command predictor that
      learns user patterns (the memory-discipline core)
  - [x] NOC next-command predictor: bigram over the REPL command history
        (`Predict;`/`Hist;`/`ClearHist;`) with unigram fallback
  - [x] Page-access predictor core: bigram over the page-fault stream
        (`PgPred;`), fed by the page-fault dispatcher; `PageFault(addr);`
        deliberately faults a spawned user process for testing
  - [ ] Prefetch action (pre-map/pre-load the predicted page) once demand
        paging / swap exist
- [ ] Model weights are shared read-only pages, demand-paged from disk and
      evictable under pressure; `model_budget(n)` syscall enforces a hard
      memory cap per LLM process
  - [x] `model_budget` syscall: per-process weight RAM budget (default
        8192 KB), `ModelBudget`/`ModelCommit`/`ModelInfo` builtins; commits
        over budget are rejected so the model degrades gracefully
  - [ ] Demand-paged read-only weight pages, evictable under pressure
- [ ] Byte-level micro-transformer (~1-10M params, 8-bit) trained on the NOC
      command history; runs as a sandboxed user-mode process
- [ ] Self-evolution loop: log interactions -> idle retrain -> model drafts
      NOC code -> runs sandboxed -> output feeds the (versioned,
      rollback-safe) corpus
- [ ] Accept: the model predicts the next command from history; a
      model-drafted NOC program runs and its result shows in the shell

## M6 — Stretch: performance & self-hosting
- [ ] Bytecode -> x86-64 JIT in a code heap
- [ ] Self-host the NOC compiler

## M7 — Stretch: fun
- [ ] PC-speaker audio
- [ ] Games / demo programs

## M8 — Graphics (TempleOS soul) (GUI last)
- [ ] Graphics spec written (`docs/M8-GRAPHICS.md`)
- [ ] VGA 640x480 16-color graphics mode (mode 0x12, register-level mode set)
- [ ] 2D primitives (`Pixel`/`Line`/`Rect`/`FillRect`) + bitmap font `Text`
- [ ] Sprite bank drawable from NOC source (`Sprite`)
- [ ] Accept: a NOC program animates a moving sprite (screendump-verified)
