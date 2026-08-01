# Growth Roadmap — NO_OS Memory-Aware Evolution

This document maps the **biological growth metaphor** ("womb → infant → child → teen → adult") to concrete, measurable milestones in NO_OS's development. Each stage defines:
- **Hardware threshold** (minimum RAM to enter stage)
- **Guaranteed capabilities** (what *must* work at this stage)
- **Opportunistic features** (what scales with available RAM)
- **Validation criteria** (how to prove you've reached the stage)
- **Next-step enablers** (what you build to reach the next stage)

This is not a feature roadmap — it's a **resource-aware capability roadmap**. Progress is measured by **demonstrated behavior on hardware of a given class**, not by checklist completion.

---

## 🌱 STAGE 0: WOMB (< 16 MB RAM)
*"The system is a deterministic real-time kernel. No AI, no frills — just reliable execution."*
| Aspect | Specification |
|--------|---------------|
| **Hardware Threshold** | Boots and runs reliably on ≤16 MB RAM |
| **Guaranteed Capabilities** | • Deterministic interrupt response (<10µs jitter)<br>• Preemptive multitasking with 10ms timeslice<br>• NOC REPL with line editing<br>• Basic syscalls (exit, putc, puts, sleep, ticks, keyboard)<br>• Memory protection via PML4 isolation |
| **Opportunistic Features** | **None** - All available RAM goes to Z0 (kernel core) and Z1 (user process minimums) |
| **Validation Criteria** | • Passes M3 acceptance test on QEMU `-m 16M`<br>• Worst-case interrupt latency < 50µs<br>• Context switch time < 5µs<br>• Zero spontaneous reboots under load |
| **Next-Step Enablers** | • Working pmm_total_bytes()/free_bytes() telemetry<br>• Scheduler idle hook for opportunistic work<br>• Basic VMM with user address space isolation |

## 👶 STAGE 1: INFANT (16-32 MB RAM)
*"The system begins to observe and predict simple patterns to save trivial effort."*
| Aspect | Specification |
|--------|---------------|
| **Hardware Threshold** | Sustainably >16 MB free RAM for >5 minutes |
| **Guaranteed Capabilities** | All Womb capabilities, plus:<br>• Kernel telemetry: pmm_total_bytes(), pmm_free_bytes()<br>• Scheduler idle hook interface<br>• Basic circular buffer logging (/var/interact.log) |
| **Opportunistic Features** | • **Z2 Allocation**: 0-4 MB available<br>• **ML Workload**: 1-gram/2-graph character predictor<br>• **Function**: Predicts next character in command stream (e.g., after `ls ` → suggests `-l`)<br>• **Value**: Saves 2-5 seconds/hour by reducing repetitive typing |
| **Validation Criteria** | • System allocates ≥2MB to Z2 when >16MB free<br>• Character prediction accuracy >60% on held-out log data<br>• Measurable reduction in keystrokes for repetitive patterns<br>• No increase in worst-case interrupt latency |
| **Next-Step Enablers** | • Filesystem for persistent model storage<br>• Increased Z2 allocation headroom<br>• Basic model validation sandbox |

## 👦 STAGE 2: CHILD (32-64 MB RAM)
*"The system generates useful code snippets to automate routine tasks."*
| Aspect | Specification |
|--------|---------------|
| **Hardware Threshold** | Sustainably >32 MB free RAM for >10 minutes AND model perplexity <2.0 |
| **Guaranteed Capabilities** | All Infant capabilities, plus:<br>• Filesystem (M4) for model/corpus persistence<br>• ml_alloc_request() interface<br>• Sandboxed model execution (500ms watchdog) |
| **Opportunistic Features** | • **Z2 Allocation**: 4-16 MB available<br>• **ML Workload**: 1-5M parameter byte-level transformer<br>• **Function**: Generates valid 1-2 line NOC snippets (e.g., `Print("hello"); 5+3;` → `Print("hello\n8");`)<br>• **Value**: Saves 15-30 seconds/hour by eliminating boilerplate lookup |
| **Validation Criteria** | • System allocates ≥8MB to Z2 when >32MB free<br>• Generated NOC passes lexer/type check >70% of time<br>• Sandboxed execution success rate >50%<br>• User reports noticing time savings in routine tasks |
| **Next-Step Enablers** | • JIT compiler (M6) to reduce inference cost<br>• Increased context length for better pattern capture<br>• Model distillation for efficiency gains |

## 👨‍🎓 STAGE 3: TEEN (64-256 MB RAM)
*"The system suggests multi-step automations and learns from correction."*
| Aspect | Specification |
|--------|---------------|
| **Hardware Threshold** | Sustainably >64 MB free RAM AND model utilization >60% |
| **Guaranteed Capabilities** | All Child capabilities, plus:<br>• JIT-compiled NOC bytecode (M6)<br>• ml_rudimentary RAG over corpus<br>• Feedback loop for rejected suggestions |
| **Opportunistic Features** | • **Z2 Allocation**: 16-64 MB available<br>• **ML Workload**: 10-50M parameter model with attention<br>• **Function**: Suggests multi-line functions based on recent patterns (e.g., sees `for(i=0;i<10;i++){Print(i);}` → suggests `print_range(0,9);`)**<br>• **Value**: Saves 1-3 minutes/hour by automating multi-step workflows |
| **Validation Criteria** | • System allocates ≥32MB to Z2 when >64MB free<br>• Multi-line suggestion success rate >40%<br>• Correction feedback reduces repeat errors by >30%<br>• JIT provides ≥5x speedup over interpreter |
| **Next-Step Enablers** | • Continuous learning (remove idle wait requirement)<br>• Cross-modal inputs (if sensors present)<br>• Advanced retrieval techniques |

## 👨‍💼 STAGE 4: ADULT (>256 MB RAM)
*"The system proactively anticipates needs and evolves with the user."* |
| Aspect | Specification |
|--------|---------------|
| **Hardware Threshold** | Sustainably >128 MB free RAM AND user satisfaction >0.8 |
| **Guaranteed Capabilities** | All Teen capabilities, plus:<br>• Continuous learning (no idle wait)<br>• Cross-modal input integration<br>• Hierarchical memory (hot/warm/cold tiers) |
| **Opportunistic Features** | • **Z2 Allocation**: 64 MB+ available<br>• **ML Workload**: 50M+ parameter model with retrieval augmentation<br>• **Function**:<br>  - Learns from edit history: `mv file.txt bak/` → suggests `bak() { mv $1 ${1}.bak; }`<br>  - Predicts command chains from partial input: `git com` → `git commit -m "..."`<br>  - Suggests optimizations: notices `for(i=0;i<1000;i++){strcat(buf,str);}` → recommends buffer preallocation<br>• **Value**: Saves 5+ minutes/hour by reducing cognitive load and preventing errors |
| **Validation Criteria** | • System allocates ≥64MB to Z2 when >128MB free<br>• Proactive suggestion acceptance rate >35%<br>• Reduction in repetitive strain indicators (backspace/correction rate)<br>• User reports "feeling like the system anticipates me" |
| **Next-Step Enablers** | • Domain-specific fine-tuning<br>• Multimodal interaction (voice, gesture)<br>• Federated learning with privacy guarantees |

---

## 🔬 HOW TO MEASURE PROGRESS: THE VALUE DENSITY METRIC

Forget parameter counts. The **only** metric that matters is:

```
Value Density = (Time Saved by Automation) / (RAM Used by Opportunistic Features)
```

### Measurement Protocol
1. **Baseline**: Measure time to complete a standard workflow (e.g., create, compile, and run a simple NOC program) without predictions
2. **Test**: Measure time with predictions enabled
3. **Time Saved** = Baseline Time - Test Time
4. **RAM Used** = `pmm_total_bytes() - pmm_free_bytes() - kernel_core_reserve - (task_count * 68KB)`
5. **Value Density** = Time Saved (seconds) / RAM Used (MB)

### Acceptance Thresholds by Stage
| Stage | Minimum Value Density | Rationale |
|-------|------------------------|-----------|
| **Womb** | N/A (0) | No opportunistic features |
| **Infant** | > 0.05 sec/MB | 2.5 sec saved per 50MB RAM = worth it for typing relief |
| **Child** | > 0.25 sec/MB | 12.5 sec saved per 50MB RAM = worth it for boilerplate elimination |
| **Teen** | > 0.75 sec/MB | 37.5 sec saved per 50MB RAM = worth it for workflow automation |
| **Adult** | > 2.0 sec/MB | 100 sec saved per 50MB RAM = worth it for cognitive offloading |

> 💡 **Example**: If a Child-stage system uses 12MB RAM for ML and saves 3 seconds/minute (180 seconds/hour), its Value Density = 180s / 12MB = 15 sec/MB·hour → **Exceeds threshold by 60x**.

---

## 📜 HOW THIS MAPS TO YOUR EXISTING ROADMAP

| NO_OS Milestone | Primary Stage Enabled | Critical Enabler for Next Stage |
|-----------------|------------------------|---------------------------------|
| **M0** | Womb (baseline) | None (foundational truth) |
| **M1** | Womb | Timing/infrastructure for determinism |
| **M2** | Womb → Infant | NOC as interaction language (enables observation) |
| **M2.5** | Infant | Interruptible VM (enables safe observation during execution) |
| **M3** | **Infant** (16-32MB) | **User mode = SAFE SANDBOX FOR MODEL EXECUTION** ✅ DONE |
| **M4** | Infant → Child | **Filesystem = PERSISTENT STORAGE FOR MODELS/CORPUS** ← **CURRENT FOCUS** |
| **M5** | Child → Teen | **model_budget(n) syscall = FORMAL ALLOCATION INTERFACE** |
| **M6** | Teen → Adult | **JIT COMPILER = ENABLES LARGER MODELS IN SAME RAM** |
| **M7+** | Adult | Domain specialization, RAG, NL→NOC |
| **M9** | Teen | Services & task manager = managed CPU budgets for model/service coexistence |
| **M10** | Teen → Adult | NOClang TUI = the CLI core where prediction/completion surface to the user |
| **Future (unnumbered)** | Adult | Graphics/GUI — deferred; text surface (M10) completes first |

**KEY INSIGHT**: You **cannot** reach Child stage (M5) without completing Infant stage (M3). The user-mode sandbox is non-negotiable for safe model execution. This is why we **must** get M3 rock-solid before adding "AI" features — it's not optional architecture, it's a **safety requirement**.

---

## ✅ M3 HOOKS — IMPLEMENTED (verified by boot-test TEST PASS)

The two hooks below were added during M3 bug fixing and are now live. Note: the codebase uses `FRAME_SIZE` (not `PAGE_SIZE`) and the pmm getters are functions, so the final implementations are `pmm_total_frames() * FRAME_SIZE` / `pmm_avail_frames() * FRAME_SIZE`.

### 1. In `kernel/mm/pmm.c` (implemented):
```c
u64 pmm_total_bytes(void) { return pmm_total_frames() * FRAME_SIZE; }
u64 pmm_free_bytes(void)  { return pmm_avail_frames()  * FRAME_SIZE; }
```

### 2. In `kernel/kern/sched.c` (implemented, hook kept static per docs):
```c
static void (*sched_idle_hook)(void) = NULL;
void sched_register_idle_hook(void (*fn)(void)) { sched_idle_hook = fn; }
void sched_idle(void) {
    for (;;) {
        __asm__ volatile("sti");
        if (sched_idle_hook) sched_idle_hook();
        __asm__ volatile("hlt");
    }
}
```
Public declarations live in `kernel/include/sched.h`; `sched_idle()` is called from `kmain` after the REPL.

### 2. In `kernel/kern/sched.c` (add near other function prototypes):
```c
/*
 * REQUIRED FOR GROWTH ROADMAP - SAFE TO ADD DURING M3 BUG FIXING
 * Enables opportunistic work during idle cycles (critical for M4/M5)
 */
static void (*sched_idle_hook)(void) = NULL;
void sched_register_idle_hook(void (*fn)(void)) { sched_idle_hook = fn; }
/* Then in your existing sched_idle() loop: */
/* for (;;) { */
/*   __asm__ volatile("sti; hlt"); */
/*   if (sched_idle_hook) sched_idle_hook(); */
/* } */
```

**WHY THIS IS SAFE FOR YOUR CURRENT WORK:**
- ✅ **Zero risk to M3**: Adds no execution paths, no memory overhead, no API changes
- ✅ **Zero cost when idle hook unused**: Function pointer check is cheaper than a cache miss
- ✅ **Zero rework later**: These are the exact hooks needed for M4/M5 memory scaling
- ✅ **Aligns with your workflow**: You're likely touching these files anyway for bug fixes
- ✅ **Enables your vision**: Lets the system *naturally* discover its optimal capability level

Once these are in place, your current M3 work remains 100% valid, and you've laid the foundation for the system to **organically grow its AI capabilities** as you progress through M4 (filesystem) and M5 (ML integration) — all while staying true to your sacred `spec → implement → boot-test → commit` rhythm.

Remember: **The goal isn't to build an "AI OS" — it's to build an OS that *naturally becomes* an AI assistant when the hardware allows it.** This is how you avoid process cosplay and deliver real value at every stage.

---
*Last updated: 2026-08-01*  
*Status: M3 hooks implemented and verified (TEST PASS). Next: M4 filesystem → Infant-to-Child transition*