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
[TICK] u64 timestamp
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