# NO_OS — Roadmap

Each milestone ends with bootable, testable acceptance criteria. Work flows:
spec -> implement -> boot-test -> commit.

**The CLI is the core of NO_OS.** M10 defines the NOClang TUI, a full-screen
text shell that is also the home of the AI/ML/LLM factors: prediction,
completion, code generation, self-evolution, and service management all
surface through the same text interface. The GUI is explicitly **deferred to
an unnumbered future stage** — text first, graphics later, TempleOS-style
but command-line-first.

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
- [x] Kernel-side micro-ANN: page/swap prefetcher + command predictor that
      learns user patterns (the memory-discipline core)
  - [x] NOC next-command predictor: bigram over the REPL command history
        (`Predict;`/`Hist;`/`ClearHist;`) with unigram fallback
  - [x] Page-access predictor core: bigram over the page-fault stream
        (`PgPred;`), fed by the page-fault dispatcher; `PageFault(addr);`
        deliberately faults a spawned user process for testing
  - [x] Prefetch action (pre-map/pre-load the predicted page): after each
        model-window demand fault, the dispatcher asks the predictor for the
        next page and pre-loads it (`model_prefetch`, `kernel/mm/model.c`)
        when it is another model page, non-resident, and within budget —
        harness-verified: a cold 0->1->2 fault sequence, evict of 1 and 2,
        then a fresh fault on 1 makes the predictor expect 2, which is
        faulted in by the prefetch action (`model: prefetch pg=2`) so the
        follow-up read of 2 hits instead of faulting
- [x] Model weights are shared read-only pages, demand-paged and evictable
      under pressure; `model_budget(n)` syscall enforces a hard memory cap
      per LLM process
  - [x] `model_budget` syscall: per-process weight RAM budget (default
         8192 KB), `ModelBudget`/`ModelCommit`/`ModelInfo` builtins; commits
         over budget are rejected so the model degrades gracefully
  - [x] Demand-paged read-only weight pages, evictable under pressure: the
         64 KiB weight table is exposed at a fixed user window
         (`USER_MODEL_BASE`, 16 pages); each first access faults a page in
         read-only from the canonical copy and charges 4 KiB to the budget;
         `ModelTouch(<pg>)`/`ModelEvict(<pg>)`/`ModelStats;` drive it, and a
         present+write fault (or an over-budget request) is refused — the
         former kills the writer, the latter denies the page. Harness-verified
         incl. evict-then-refault under a 4 KB budget.
- [x] Byte-level micro-transformer (~1-10M params, 8-bit) wired behind the
      demand-paged model window (`kernel/noc/trans.c`): int8 Q8.8 fixed-point
      attention/GELU/LayerNorm, config autotuned up / degraded down to boot
      RAM (`TransInfo`/`TransConfig`/`TransMem`), greedy `TransPredict`
      generation, and the window now serves `trans_weights()` — a spawned
      process reads the header magic and final-LN gamma/bias back through the
      mapping, writes are refused, and budget pressure evicts/refaults pages
      (harness-verified); `model_invalidate_all` drops every task's frames on
      reconfiguration
  - [x] SGD backprop training (`trans_train`/`trans_eval`) with fixed-point
        loss and per-tensor normalized weight updates, retraining from the
        interaction log tail: `TransTrain(<why>[, <passes>]);` runs full
        int8/Q8.8 backprop over the last `ctx+2` bytes (two next-byte
        targets), `TransEval;` reports in-corpus accuracy + loss, and
        `TransReset;` re-randomizes deterministically — reset + identical
        log reproduces the identical loss (harness-verified, loss 8.09
        bits/byte on both runs)
  - [x] Transformer idle auto-retrain: `trans_poll_idle` (mirrors
        `train_poll_idle`) is polled from `kbd_readc` alongside the bigram
        and fires `trans_train("idle", TRANS_BATCH, 1)` after the `TransIdle`
        threshold (default 30 s) AND >= 64 new log bytes past the watermark;
        `TransIdle(<secs>);` lowers the trigger for testing and `trans_info`
        reports `idle=N s` (harness-verified auto-fire)
- [x] Self-evolution loop: log interactions -> idle retrain -> model drafts
      NOC code -> runs sandboxed -> output feeds the (versioned,
      rollback-safe) corpus
  - [x] Persistent interaction log: every REPL command captured as
        `[TICK]`/`[CMD]`/`[OUT]`/`[ERR]` records in a 64 KiB ring
        (`LogInfo;`/`LogDump;`/`LogClear;`), plus `[SPAWN]`/`[EXIT]`
        process-event records hooked into `sched_spawn`/`sched_exit_user`;
        `LogSave;` writes a 4 KiB checkpoint to `interact.log` (flat FS,
        no `/var/` yet) that is restored at boot — the training corpus
        survives reboot
  - [x] Idle retrain loop feeding the micro-transformer from the log:
        a byte-bigram model (64 KiB of 8-bit weights, `kernel/noc/train.c`)
        polls from the keyboard idle loop, auto-retrains when the machine
        has been idle past a threshold (default 30 s) and >= 64 new log
        bytes have arrived; `Train;` forces a pass and reports integer
        fixed-point cross-entropy loss, `TrainIdle(<secs>);` lowers the
        trigger for testing, `ModelInfo` shows lifetime stats
  - [x] Generate from the trained model: `PredictBigram(<seed>);` greedily
        argmax-follows the last seed byte through the bigram table (tie ->
        lowest byte, so output is deterministic), stopping at unknown
        transitions; verified against a clean controlled corpus, where from
        seed `xyz` it reproduces the interaction-log byte structure
        (`pred: xyz\n[CK] Prin(")` — `[CMD]`'s C->M ties `[TICK]`'s C->K
        and the lowest byte wins). The `[TICK]` record carries a fixed
        token (not the wall-clock tick value) so the corpus -- and every
        generation -- is timing-independent.
  - [x] Model-drafted NOC program, transformer-first with a bigram
        fallback, syntax-gated and spawned sandboxed: `DraftRun(<seed>);`
        completes the seed with the micro-transformer first
        (`trans_generate`, logged as `draft: trans:`), and if that draft is
        empty or fails `noc_check_syntax` (lex+parse+compile, no run), the
        byte-bigram generator (`train_generate`, logged as `draft: bigram:`)
        supplies the completion — so a hallucinated draft is rejected
        instead of executed and the loop works before any transformer
        training. The accepted program is `sched_spawn`ed as a ring-3 user
        process whose output lands in the shell and back into the
        interaction log, and the next `TransTrain("evolve")` retrains over
        it (draft -> run -> log -> retrain). Verified against a controlled
        corpus of `PrintLn("DRAFT-OK");` x5 + `Train;` +
        `TransTrain("draft", 4)`: seed `PrintLn("DRAFT-OK"` (balanced
        string, closing quote in the seed) yields `draft: trans: [` — the
        poorly-trained transformer's draft fails the gate — then `draft:
        bigram: );`, the full `draft: PrintLn("DRAFT-OK");` spawns, the
        bare `DRAFT-OK` reaches the shell, and `TransTrain("evolve", 1)`
        absorbs the drafted output. (Held-out evaluation is a later slice.)
  - [x] Versioned, rollback-safe corpus: `DraftRun` writes accepted drafts to
        `corpNNNN.noc` with a `@@ GENERATED:` metadata header and advances
        the version (`CorpusInfo;`); a rejected (syntax-failing) draft leaves
        the corpus untouched (verified by round-trip through `ReadFile`); and
        `CorpusRollback;` re-spawns the last known good generation. The
        versioned files are the persistent corpus that survives reboot.
- [x] Accept: the model predicts the next command from history; a
      model-drafted NOC program runs and its result shows in the shell
      (`DraftRun` + `noc_check_syntax` + `sched_spawn`, harness-verified)

## M6 — Stretch: performance & self-hosting
- [ ] Bytecode -> x86-64 JIT in a code heap
- [ ] Self-host the NOC compiler

  **Naming twist:** once NOC matures enough to self-host, it earns the
  name **NOClang** (NOC + "lang") — the day the language compiles itself,
  it stops being "Not Quite C" and becomes a real compiler target. The M10
  TUI already previews the name informally; M6 is where it becomes official.

## M7 — Stretch: fun
- [ ] PC-speaker audio
- [ ] Games / demo programs

## M9 — Services & Task Manager (NOC)
The task manager is the system-service layer: the OS's own services (fs,
interact logger, idle trainer, predictor) and user services are managed the
same way Windows SCM / Linux systemd manage theirs. The AI/ML/LLM machinery
is one service class, not the whole story. Full spec: `docs/M9-TASKMANAGER.md`
- [ ] Service registry (`svc_t`: `SVC_KERNEL`/`SVC_USER`, autostart, restart
      policy) wired to the process table (`kernel/kern/sched.c`)
- [ ] Syscall surface 16-21: `SYS_SVC_COUNT/PID/FIELD/BUDGET/PAUSE/KILL`
      (extends `kernel/include/syscall.h`, currently 0-15)
- [ ] CPU budget per service: `cpu_budget_ms` + per-tick `cpu_ticks` accounting
      in `sched_on_tick()` (`sched.c:134`) — implements audit finding #7
- [ ] Restart policy enforced in `sched_exit_user` (`sched.c:120`) for
      `SVC_RESTART`/`SVC_RESTART_EXITCODE` services
- [ ] NOC builtins: `SvcCount`, `SvcPid`, `SvcField`, `SvcBudget`, `SvcPause`,
      `SvcKill`, `Ps` (process table dump)
- [ ] `taskmgr.noc` — the manager itself as a NOC policy program
- [ ] Accept: `Ps;` lists services and their CPU budgets; a paused service is
      suspended (`SUSPENDED`) and resumes when unpaused; a killed service
      restarts per policy (harness-verified)

## M10 — NOClang TUI (the CLI — biggest part, home of the AI factors)
The full-screen text shell. This is the **core of NO_OS**: prediction,
completion, code generation, self-evolution, and service management all
surface through the same text interface. It is a TUI, not a GUI — pure
80x25 VGA text cells. Full spec: `docs/M10-CLI.md`
- [ ] Screen model: status bar (row 0), output viewport (rows 1-22), input
      editor (rows 23-24); VGA API v2 (`vga_clear_rect`, `vga_putc_at`,
      `vga_puts_at`, `vga_set_cursor`, `vga_scroll_region`) on top of the
      existing `kernel/drivers/vga.c` (80x25, u16 cells at 0xB8000)
- [ ] Keyboard v2: decode F1-F10 (scancodes 0x3B-0x44, currently undecoded in
      `kernel/include/kbd.h`) as `KBD_F1..F10` (0x90-0x99) + `KBD_PGUP/PGDN`
      (0x9A/0x9B)
- [ ] Key map: F1 Help, F2 Ps, F3 MemInfo, F5 Run, F9 service pane, F10
      ListDir; `SetKey` rebindable
- [ ] Completion popup over the output viewport fed by the M5 predictor
      (history, `corpNNNN.noc` corpus, symbol table)
- [ ] Multi-line NOClang via `line_balanced()` + continuation `▸` (breaks
      length-limited REPL single-line `line.c` buffer)
- [ ] Service pane (M9) embedded as a screen region, not a separate app
- [ ] Serial stays a byte-exact mirror so the existing harness asserts remain
      valid (the TUI does not break automation)
- [ ] Accept: an interactive multi-line NOC program is edited, run, and its
      output scrolls in the viewport; completion popup suggests a command
      from the corpus; F9 shows live service state (screendump + serial
      verified)

## Future (unnumbered) — Graphics / GUI
Deferred by design. Text first, graphics later — the CLI is the core, and the
TempleOS-soul graphics milestone waits until the text surface is complete.
- [ ] VGA 640x480 16-color graphics mode (mode 0x12, register-level mode set)
- [ ] 2D primitives (`Pixel`/`Line`/`Rect`/`FillRect`) + bitmap font `Text`
- [ ] Sprite bank drawable from NOC source (`Sprite`)
- [ ] Accept: a NOC program animates a moving sprite (screendump-verified)
  (design retained in `docs/M8-GRAPHICS.md`; unnumbered, parked behind M10)
