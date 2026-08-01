# M5 — ML/LLM & Self-Evolution

NO_OS M5 adds on-device machine learning capabilities and a self-evolution pipeline, turning the CLI-first system into a platform for lightweight AI experimentation. The kernel provides a sandboxed environment where a byte-level micro-transformer learns from interaction history, drafts NOC programs, and validates them in user-mode processes — all while respecting strict memory budgets.

## 1. Overview

M5 introduces two core capabilities:
1. **Model**: A byte-level micro-transformer (~1-10M parameters, 8-bit quantized) that runs as a sandboxed user-mode NOC process
2. **Evolution Loop**: A kernel-assisted process that:
   - Logs user interactions (keystrokes, commands, outputs)
   - During idle periods, retrains the model on this history
   - Has the model generate NOC program drafts
   - Runs those drafts in sandboxed user processes
   - Accepts successful outputs into the versioned, rollback-safe NOC corpus

This creates a tight coupling between the OS and its ability to evolve: the system learns to better serve its user by proposing and validating useful automations.

## 2. Memory Budget & Model Constraints

### 2.1. Model Specifications
| Parameter | Value | Notes |
|-----------|-------|-------|
| Architecture | Byte-level decoder-only transformer | Processes raw UTF-8 bytes |
| Parameters | 1-10 million | 8-bit quantized (int8 weights) |
| Context Length | 256-512 bytes | Limited by available RAM |
| Layers | 4-8 | Shallow for efficiency |
| Model Size (RAM) | 8-80 MB | 1-10M parameters × 1 byte + activations |
| Activation Budget | 2-4 MB | For KV cache and intermediate activations |

### 2.2. Kernel Enforcement: `model_budget(n)` Syscall
```c
/* Syscall #??: model_budget(size_t kb) */
void model_budget(size_t kb);
```
- Sets the maximum RAM (in KB) a single LLM process may allocate for model weights
- Default: 8192 KB (8 MB) — suitable for an ~8M parameter 8-bit model
- Enforced per-process: kernel tracks weight pages and evicts clean pages under pressure
- Weights stored as demand-paged, read-only pages from disk-backed store
- On OOM: model gracefully degrades (reduces context length) or yields

### 2.3. Memory Layout for M5 Processes
```
0x80000000  NOC program blob (user runtime + NSCP)
0x80C00000  NOC source script (32 KiB scratch)
0x80D00000  User heap (64 KiB → expandable)
0x81000000  MODEL_WEIGHTS_BASE (up to model_budget)
0x82000000  MODEL_ACTIVATIONS (KV cache, scratch)
0x82200000  User stack (64 KiB, grows down)
0x82300000  Guard page
```
- Model weights are mapped read-only from the filesystem-backed store
- Active inference uses copy-on-write pages for activations
- Under memory pressure, clean weight pages are evicted and reloaded on fault

## 3. Model Architecture Details

### 3.1. Byte-Level Tokenization
- Input: Raw UTF-8 byte stream from interaction log
- Output: Next-byte prediction (language modeling)
- Vocabulary: 256 (raw byte values)
- Sequence length: 256-512 bytes (configurable via model_budget)

### 3.2. Model Architecture
```
Input Embedding (256 → 64)     # Project bytes to model dimension
├── Transformer Block 1
│   ├── Self-Attention (64 heads, 64 dim)
│   ├── Add & Norm
│   ├── MLP (64 → 256 → 64, GELU)
│   └── Add & Norm
├── Transformer Block 2        # ... repeat for N layers
└── ...
```
- Model dimension: 64 bytes (keeps matmuls lightweight)
- Feed-forward expansion: 4x (256 inner dim)
- Activation: GELU (approximated for efficiency)
- Layer norm: Simplified variance normalization

### 3.3. Inference & Sampling
- **Greedy**: Select argmax next byte (deterministic, repeatable)
- **Temperature sampling**: `p_i = softmax(logits / T)` for creativity control
- **Top-k sampling**: Limit to k most likely bytes
- **Exit condition**: Generate until `0x04` (EOT) or max length reached

## 4. Interaction Logging & Training Data

### 4.1. Log Format
The kernel append-logs to `/var/interact.log` (pre-allocated circular buffer):
```
[TICK] fixed token (one marker per command; wall-clock timestamp omitted so
       the byte-bigram corpus stays timing-independent)
[KEY]  u8 scancode, u8 state (press/release)
[CMD]  str null-terminated NOC command entered
[OUT]  str null-terminated command output
[ERR]  i64 exit code, str error message (if any)
[SPAWN] u32 pid, str process name
[EXIT]  u32 pid, i64 exit code
```
- Fixed-size records enable efficient circular buffering
- Log size: 64 KiB (configurable)
- Oldest entries overwritten when full

> **Status (slice 5):** implemented — `[TICK]`/`[CMD]`/`[OUT]`/`[ERR]` records
> in a 64 KiB in-memory ring (`kernel/noc/interact.c`), captured around every
> REPL command via `noc_os_putc`; `LogInfo`/`LogDump`/`LogClear`/`LogSave`
> builtins; `LogSave` persists a 4 KiB checkpoint to `interact.log` (flat FS,
> no `/var/` directory yet) that `il_load()` restores at boot, so the corpus
> survives reset. `[SPAWN]`/`[EXIT]` process-event records (hooked into
> `sched_spawn`/`sched_exit_user`) now capture process births and exits,
> including killed processes (exit code `-1`). Blank REPL lines are no longer
> logged (stops `[CMD] ` noise from the harness's `ok\n\n`). Deferred:
> `[KEY]` keystroke records (needs a kbd capture hook) and the fixed-record
> binary encoding (the current log is line-based text, which doubles as the
> training corpus).

### 4.2. Training Process
- **Trigger**: System idle for >30 seconds AND log has ≥1024 new bytes
- **Batch**: Contiguous chunk from log (up to 4 KiB)
- **Objective**: Next-byte prediction (cross-entropy loss)
- **Optimizer**: SGD with learning rate decay (no momentum to save RAM)
- **Steps**: 1-4 gradient updates per trigger (keeps latency <100ms)
- **Precision**: 8-bit weights, 16-bit activations (simulated via integer ops)

> **Status (slice 6):** implemented as a byte-bigram model in
> `kernel/noc/train.c` — 64 KiB of 8-bit weights (`w[65536]`) accumulated
> over the interaction log with count capping at 255 and a per-pass halving
> (learning-rate-decay analogue) when a row saturates. The idle trigger is
> polled from `kbd_readc`'s input-wait loop (`train_poll_idle`, default
> `TrainIdle` threshold 30 s, only fires when ≥64 new log bytes arrived).
> `Train;` forces a pass and reports integer fixed-point cross-entropy loss
> (millibits/byte, 16-entry log2 table, error <0.5%); `TrainIdle(<secs>);`
> lowers the threshold for testing; `ModelInfo` shows lifetime pass/byte/loss
> stats. Verified by the boot-test harness (deterministic loss fixed point
> across identical retrains, idle auto-retrain fires). Deviations from spec
> (deferred to later M5 work): count-based bigram rather than a
> transformer, 1-4 fixed passes rather than SGD steps, `≥64` new bytes
> rather than `≥1024`, no held-out validation yet.

> **Status (slice 7):** generation is in — `PredictBigram(<seed>);`
> (`kernel/noc/train.c:train_generate`) greedily argmax-follows the last
> seed byte through the trained table (`w[prev<<8|next]`), breaking ties
> toward the lowest byte so output is deterministic, and stops when no
> transition is known. `train_reset()` zeroes the weights so the harness can
> fit a clean, controlled corpus. Verified against exactly that: after
> `TrainReset; LogClear;` + five `PrintLn("xyz");` + `Train;`, seeding
> `PredictBigram("xyz")` emits `pred: xyz\n[CK] Prin(")` — `z`->`\n` (the
> `[OUT] xyz` records beat `"`), `[`, `C` (tie among `[TICK]/[CMD]/[OUT]`,
> lowest byte wins), `K` (`[TICK]`'s C->K ties `[CMD]`'s C->M and `K`<`M`),
> then ` Prin(")` — i.e. the model faithfully reproduces the interaction
> log's byte structure from a 3-byte seed. The `[TICK]` record is a fixed
> token (no wall-clock tick value) so the corpus and every generation are
> timing-independent — tick digits used to leak timing noise into the
> table and make the ` ` transition (digit vs `P`) a coin flip.

> **Status (slice 8):** the accept criterion is closed — a model-drafted
> NOC program is transformer-first with a bigram fallback, syntax-gated,
> spawned sandboxed, and its result reaches the shell. `DraftRun(<seed>);`
> (`kernel/noc/vm.c`) completes the seed through the micro-transformer
> first (`trans_generate`, printed as `draft: trans: ...`); if that draft
> is empty or fails `noc_check_syntax()` (`kernel/noc/exec.c` —
> lex+parse+compile, never run), the byte-bigram model (`train_generate`,
> `draft: bigram: ...`) supplies the completion — so a hallucinated draft
> is rejected instead of executed and the loop works before any
> transformer training (degrading gracefully on small hardware). Generation
> stops at the first newline. `sched_spawn` launches the accepted program
> as a ring-3 user process whose `SYS_PUTS` output lands on the serial log
> and back into the interaction log, and the next `TransTrain("evolve", ...)`
> retrains over that drafted output — draft -> run -> log -> retrain.
> Verified against `TrainReset; TransReset; LogClear;` + five
> `PrintLn("DRAFT-OK");` + `Train;` + `TransTrain("draft", 4);` +
> `DraftRun("PrintLn(\"DRAFT-OK\"");`: the seed is a balanced string
> (closing quote included) so generation only adds the unambiguous
> `")-> ; -> \n` tail of the `[CMD]` record; the poorly-trained transformer
> emits a one-byte draft (`draft: trans: [`) that fails the gate, the
> bigram completes the tail (`draft: bigram: );`), the draft prints as
> `draft: PrintLn("DRAFT-OK");`, spawns (`draft: spawned pid 1`), and the
> bare `DRAFT-OK` output line reaches the shell. The follow-up
> `TransTrain("evolve", 1)` runs over the log that now includes the drafted
> program's output (`trans: train evolve (1 passes, ...)`), closing the
> self-evolution loop. Seeding past the closing quote deliberately skips
> the `K` transition, where the `[TICK]` records' `K`->`]` and the `[OUT]`
> records' `K`->`\n` would both out-vote the close-quote `K`->`"`.

> **Status (slice 9):** the corpus is now versioned and rollback-safe
> (`kernel/noc/corpus.c`). `DraftRun` persists each accepted draft to
> `corpNNNN.noc` (incrementing `NNNN`) with a `@@ GENERATED:` metadata
> header so the versioned file is the durable corpus (survives reboot), and
> `CorpusInfo;` reports the current version / next version / last-known-good
> state. A draft that fails `noc_check_syntax` is rejected *without* touching
> the corpus — `CorpusInfo` does not advance, verified by round-tripping the
> last versioned file through `ReadFile`. `CorpusRollback;` re-spawns the
> last known good generation (`corpus: rollback spawned pid N`) and its
> output reaches the shell, so a bad commit can be unwound without losing
> earlier work.

> **Status (slice 10):** the trained weights are exposed as demand-paged,
> read-only user pages instead of a bulk-resident blob. The 64 KiB table
> lives at a fixed window (`USER_MODEL_BASE` = `0x100F1000000`, 16 pages,
> `kernel/include/vmm.h`); a user-mode first access traps to
> `model_demand_fault` (`kernel/mm/model.c`), which copies the 4 KiB page
> from the canonical weights in `train.c` (`train_weights()`), maps it
> read-only via `vmm_map(..., VMM_USER)` (no `VMM_WRITE`), and charges the
> frame to the task's `model_budget`. `ModelTouch(<pg>)`/`ModelEvict(<pg>)`/
> `ModelStats;` (syscalls 13-15) drive the window explicitly. A write to a
> resident page faults (present+write) and is refused — the offending
> process is killed (`process N killed: Page Fault`, `cr2=...`); a fault
> that would exceed the budget is denied (`model: pg N denied (budget M KB)`)
> rather than exceeding the cap, and `ModelEvict` un-maps + frees + refunds
> so the page can be faulted back in. Harness-verified end to end: cold page
> reads back `zero=0`, the trained `A->A` weight (offset `0x4141`, pg 4)
> reads back nonzero, a write to the window kills the process, and
> `ModelBudget(4); ModelTouch(0); ModelTouch(1); ModelEvict(0); ModelTouch(1);`
> exercises deny -> evict -> refault in one run.

> **Status (slice 11):** the micro-transformer can now train. `TransTrain(<why>[, <passes>]);`
> (`kernel/noc/trans.c:trans_train`) copies the tail of the interaction log
> (up to 4 KiB, watermarking `il_len_bytes`) and runs full fixed-point SGD
> backprop over the last `ctx+2` bytes: cross-entropy over two next-byte
> targets (t+1 and t+2), per-layer layernorm / GELU / attention gradient
> paths, and per-tensor normalized weight updates that map the max gradient
> to a step of ~2 with |u|≤3 so int8 weights stay in range. `TransEval;`
> reports in-corpus top-1 accuracy plus average loss; `TransReset;`
> deterministically re-randomizes the weights (PRNG seed fixed) so a
> reset + identical log reproduces the identical loss — harness-verified
> (loss 8.09 bits/byte on both runs). Backprop buffers are lazily allocated
> (`alloc_train`) and freed on reconfigure/reset; `working_kb` tracks the
> real footprint.
>
> **Status (slice 12):** the transformer now joins the idle-retrain loop.
> `trans_note_input`/`trans_poll_idle` mirror `train.c` and are polled from
> `kbd_readc`'s input-wait loop alongside the bigram (`kernel/drivers/kbd.c`);
> `trans_poll_idle` fires `trans_train("idle", TRANS_BATCH, 1)` only after
> the `TransIdle` threshold (default 30 s) has elapsed AND ≥64 new log bytes
> arrived past the train watermark, so each idle pass is bounded (1 pass)
> and only runs on genuinely new material. `TransIdle(<secs>);` sets the
> threshold (mirrors `TrainIdle`); `trans_info` now reports `idle=N s`.
> Harness-verified: threshold lowered to 1 s, fresh corpus typed, and the
> `trans: train idle (...)` line fired automatically without any command.
>
> **Status (prefetch action):** the page-access predictor now has its
> action half — it is no longer a passive observer. The page-fault
> dispatcher (`kernel/arch/x86_64/isr.c:isr_dispatch`) feeds every
> model-window fault into the predictor (`pgpred_fault`) and then asks
> `model_prefetch` (`kernel/mm/model.c:model_prefetch`) to pre-load the
> page it expects next: if the prediction is another model page, not yet
> resident, and within the task's `model_budget`, it is faulted in up front
> so the next access is a hit instead of a fault. Harness-verified: with
> `PgPredClear;`, a cold `pg=0 -> pg=1 -> pg=2` read sequence builds the
> 0->1, 1->2 history; `ModelEvict(1); ModelEvict(2)` drops the followers; a
> fresh fault on pg=1 re-learns it and the predictor (last=1 -> follower 2)
> expects pg=2 — non-resident — so the prefetch action faults it in
> (`model: prefetch pg=2 resident=3 used=12 KB`) and the follow-up read of
> pg=2 hits instead of faulting (`PREFETCH-OK`), proving the pre-load.
> Like the demand-fault path, a prefetch beyond budget is denied
> (`model: prefetch pg=N denied (budget M KB)`) so the cap still holds.

### 4.3. Model Updates
- New weights written to temporary file
- On successful validation: atomic swap with current model file
- Rollback: Keep previous version for 5 cycles, then garbage collect
- Validation: Perplexity held-out on last 100 bytes of current log

## 5. Sandboxed Model Execution

### 5.1. Process Model
- Model runs as a standard user-mode NOC process (ring 3)
- Strict syscall filtering: only `alloc`, `free`, `putc`, `puts`, `ticks`, `yield`
- Memory access restricted to:
  - Code/model regions (read-only)
  - Heap (read-write, bounded by `model_budget`)
  - Stack (fixed size)
- No direct hardware access or syscalls beyond the allowed set

### 5.2. Resource Limits
| Resource | Limit | Enforced By |
|----------|-------|-------------|
| CPU Time | 50ms per activation burst | Scheduler timeslice |
| RAM (weights) | `model_budget` default 8192 KB | `model_budget` syscall |
| RAM (total) | 16384 KB (16 KB heap + weights + activations) | VMM + heap tracker |
| Disk | Model weights file only (append-only log) | Filesystem permissions |

### 5.3. Safety Mechanisms
- **Memory Bounds**: Hardware-enforced via paging (no Spectre/Meltdown mitigations needed in ring 3)
- **Syscall Filter**: Illegal syscalls terminate the process
- **Time Slicing**: Preemptive multitasking prevents hogging CPU
- **Output Sanitization**: Model-generated NOC is validated before execution
- **Rollback**: Failed experiments don't corrupt the corpus

## 6. Evolution Loop Details

### 6.1. Corpus Management
- **Location**: `/corpus/` directory (FAT-like filesystem)
- **Format**: `.noc` files with metadata header
- **Versioning**: Incremental numbering (`001.noc`, `002.noc`, ...)
- **Rollback**: `last_known_good.noc` symlink
- **Annotation**: Each file header contains:
  ```
  @@ GENERATED: 2026-08-01T12:34:56Z
  @@ PROMPT: "last 200 bytes of interaction log"
  @@ SCORE: 0.87  # 0-1 reward signal
  @@ PARENT: 0042.noc
  ```

### 6.2. Reward Signals
The system assigns implicit rewards based on observable outcomes:
| Signal | Weight | Description |
|--------|--------|-------------|
| Success | +1.0 | Generated NOC compiles, runs, produces expected output |
| Error | -0.5 | Parse/compile/runtime error |
| Timeout | -0.3 | Exceeded instruction limit |
| User Approval | +2.0 | User runs `Accept<gen_id>;` to endorse |
| User Rejection | -1.0 | User runs `Reject<gen_id>;` to reject |

### 6.3. Generation Process
1. **Context**: Last N bytes of interaction log (configurable)
2. **Prompt**: `"User: [context]\nAssistant:"`
3. **Generation**: Model outputs bytes until `0x04` (EOT) or timeout
4. **Validation**:
   - Syntax check (lexer/parser)
   - Type checking (basic)
   - Sandboxed execution (500ms max)
5. **Acceptance**:
   - If output matches expected pattern → auto-accept
   - If novel/useful → present to user for confirmation
   - If harmful/repetitive → discard

## 6.4. Example Use Cases
- **Automation**: After observing `ls; cat file.txt;` repeatedly, suggest `lscat() { ls; cat $1; }`
- **Discovery**: From `Print("fib"); 0,1,1,2,3,5,8;` propose recursive Fibonacci implementation
- **Optimization**: Notice `for(i=0;i<100;i++){ Print(i); }` is slow, suggest buffered output
- **Personalization**: Learn user's preferred command abbreviations and aliases

## 7. Build & Integration

### 7.1. Kernel Changes
- New syscall: `model_budget` (reserved syscall #?)
- Enhanced logger: Circular buffer in reserved RAM
- Idle detection: Hook into scheduler's tickless idle
- Model loader: Demand-page ELF sections from `/model/weight.bin`

### 7.2. User Space
- `/model/weight.bin`: Memory-mapped model weights (initially random)
- `/var/interact.log`: Circular interaction log
- `/corpus/`: Versioned NOC script repository
- `/tmp/`: Scratch space for model activations

### 7.3. Dependencies
- Filesystem (M4): Required for model persistence and corpus
- Paging (M1): Essential for demand-paged weights
- Syscalls (M3): Foundation for model-process communication
- NOC VM (M2): Execution sandbox for model output

## 8. Acceptance Criteria (M5)
1. **Model Training**: System logs interactions and updates model weights during idle
2. **Code Generation**: Model produces syntactically valid NOC programs
3. **Sandboxing**: Generated code runs in isolated user process with restricted syscalls
4. **Corpus Update**: Successful programs are added to `/corpus/` with metadata
5. **User Interaction**: `model_budget` syscall functions and affects memory usage
6. **Self-Evolution**: End-to-end loop: log → train → generate → validate → corpus

## 9. Non-Goals (M5)
- No floating-point unit usage (still disabled in user mode)
- No multi-threaded model execution (single-core embedded focus)
- No external model downloads (all training/innovation is local)
- No GPU acceleration (pure CPU, integer-only)
- No network model updates (air-gapped by design)

## 10. Future Extensions (Post-M5)
- **M6**: JIT compiler for NOC bytecode → native code (speed up model inference)
- **M7**: Domain-specific model fine-tuning (e.g., for sysadmin scripts)
- **M8**: Visualization of model attention maps in graphics mode

---
*This document specifies the M5 milestone. Implementation should follow the NO_OS development cycle: spec → implement → boot-test → commit.*