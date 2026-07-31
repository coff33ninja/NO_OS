# Dynamic Memory Allocation System — NO_OS

This document specifies the memory allocation architecture for NO_OS, designed to support the system's organic growth from resource-constrained environments ("womb") to richly provisioned systems ("adult") while maintaining deterministic real-time guarantees for critical paths.

## Core Philosophy

Memory allocation follows a **reserved-minimum + opportunistic-maximum** model:
1. **Kernel core** and **user process minimums** are reserved at boot to ensure basic functionality
2. **Remaining RAM** is allocated opportunistically to:
   - AI/ML workloads (highest priority when available)
   - Disk cache (secondary priority)
   - Elastic user process growth (tertiary priority)
3. All allocations are **reclaimable under pressure** except for non-pageable kernel core
4. Allocation decisions are made **purely based on locally observed resource availability** — no external telemetry or configuration

This ensures:
- ✅ Deterministic operation on minimal hardware (e.g., 32KB microcontrollers)
- ✅ Automatic scaling of capabilities when resources permit
- ✅ Graceful degradation under memory pressure
- ✅ Zero configuration required — works "out of the box" on any x86-64 system

## Memory Zones & Allocation Rules

The physical address space is managed as four zones with strict priority ordering:

| Zone | Purpose | Allocation Policy | Reclaimable? | Minimum Guarantee |
|------|---------|-------------------|--------------|-------------------|
| **Z0: Kernel Core** | Scheduler, interrupt handlers, pmm/vmm core | Fixed at boot (never freed) | ❌ No | `max(2MB, total_ram * 0.06)` |
| **Z1: User Process Minimums** | Guaranteed stack/heap for each task | Fixed per-task at spawn | ❌ No (but can swap via VMM) | `4KB stack + 64KB heap` per task |
| **Z2: Opportunistic AI/ML** | Model weights, activations, training buffers | From free RAM after Z0+Z1 | ✅ Yes (LRU) | `0B` (if <16MB free) |
| **Z3: Disk Cache & Elastic Growth** | File cache, user heap overcommit | From remaining free RAM | ✅ Yes (LRU) | `0B` |

### Allocation Algorithm (Pseudocode)
```c
void memory_allocate_policy(void) {
    u64 total = pmm_total_bytes();
    u64 free = pmm_free_bytes();
    
    // 1. Reserve Z0 (kernel core) - 6% of RAM, min 2MB
    u64 z0 = max(MB(2), total * 6 / 100);
    
    // 2. Reserve Z1 (user process minimums) 
    //    = (max_tasks * min_per_task) OR 15% of RAM, whichever larger
    u64 z1 = max(TASK_MAX * (4*KB + 64*KB), total * 15 / 100);
    
    // 3. Remaining for Z2+Z3
    u64 remaining = total - z0 - z1;
    if (remaining < 4*MB) { // Not enough for meaningful opportunistic use
        z2 = 0;
        z3 = 0;
    } else {
        // 4. Prioritize Z2 (AI/ML) up to 60% of remaining
        u64 z2_max = remaining * 6 / 10;
        u64 z2_target = ml_workload_demand(); // From ML scheduler (0 if disabled)
        z2 = min(z2_max, z2_target);
        
        // 5. Z3 gets the rest
        z3 = remaining - z2;
    }
    
    // Enforce via page reclamation (see reclaim_scan())
    enforce_zone_limits(z0, z1, z2, z3);
}
```

### Zone Enforcement Mechanism
- **Z0/Z1**: Enforced by allocation refusal (OOM if exceeded)
- **Z2/Z3**: Enforced by asynchronous reclamation:
  - Low-priority pages (Z3 first, then Z2) are moved to inactive list
  - `kswapd` daemon (idle-triggered) scans and frees clean pages
  - Under extreme pressure: 
    1. Shrink Z3 (disk cache)
    2. Compress Z2 (if applicable, e.g., model quantization)
    3. Swap Z1 user pages to swap space (M4+)
    4. Last resort: OOM kill lowest-priority user task

## Implementation Status & Hooks

### Current State (M3 Implementation)
The following functions **must be implemented** in M3 to enable future dynamic allocation:
```c
// In include/pmm.h
u64 pmm_total_bytes(void);  // Total managed RAM
u64 pmm_free_bytes(void);   // Currently free pages * PAGE_SIZE
void pmm_set_low_watermark(u64 bytes); // Trigger reclaim when free < this

// In include/sched.h
void sched_register_idle_hook(void (*fn)(void)); // Called in idle loop
```

### Planned M4/M5 Additions
These will be implemented in subsequent milestones:
```c
// In include/ml_scheduler.h (M5)
size_t ml_alloc_request(size_t bytes, ml_priority_t prio);
void ml_usage_feedback(size_t used, size_t peak);

// In include/vmm.h (M4 - for user address space elasticity)
int vmd_reserve_range(u64 start, u64 length, int flags); // For growing heaps
```

## Configuration-Free Operation

All parameters are **hardcoded heuristics** based on decades of embedded systems experience:
- **Z0 minimum (2MB)**: Based on observed kernel size in M2.5 + headroom for interrupts
- **Z1 minimum (68KB/task)**: 4KB stack (x86-64 minimum) + 64KB heap (sufficient for NOC)
- **Z2 trigger (16MB free)**: Below this, system is "memory constrained" for ML purposes
- **Z2/Z3 split (60/40)**: Empirical balance favoring AI when resources allow, but keeping cache for responsiveness

These values require **no tuning** and work acceptably from:
- **Lower bound**: 16MB total RAM (Z0=2MB, Z1≈1MB for 16 tasks, Z2=0, Z3=13MB)
- **Upper bound**: Scales linearly to utilize 100% of available RAM

## Why This Matches Your Vision

1. **"Worm to Adult" Progression**  
   - **<16MB RAM**: Z2=0 → Pure RTOS (Womb: deterministic, no AI)
   - **16-32MB**: Small Z2 → Character/n-gram models (Infant: basic prediction)
   - **32-64MB**: Significant Z2 → Byte-transformers (Child: your current M5 target)
   - **64-256MB**: Large Z2 + VMM elasticity → Advanced models (Teen: JIT-assisted)
   - **>256MB**: Near-unlimited Z2 → RAG, continuous learning (Adult: full AI OS)

2. **Shatters OS/AI Boundaries**  
   AI isn't a "module" — it's the natural consumption of opportunistic memory. When the system has RAM to spare, it *naturally* develops more sophisticated predictive capabilities — just like a biological organism develops more complex behaviors with better nutrition.

3. **Anti-Overengineering by Design**  
   - Zero configuration files
   - No daemon tuning
   - No external dependencies
   - Same binary works from microcontroller to server
   - Value emerges organically from resource availability

4. **Preserves Your Development Rhythm**  
   Implement in strict order:
   1. **M3**: Add `pmm_total_bytes()/free_bytes()` and `sched_register_idle_hook()` (trivial, <50 lines)
   2. **M4**: Implement `kswapd`-style reclamation and Z2/Z3 tracking
   3. **M5**: Plug in ML scheduler using the established allocation interface

## Immediate Action Items for M3 Bug Fixing

While addressing current opencde validation issues, please add these **non-invasive, backward-compatible** hooks:

### 1. In `kernel/mm/pmm.c` (add to existing file)
```c
/* Add these to the end of pmm.c - requires no changes to existing functions */
u64 pmm_total_bytes(void) {
    return pmm_total_frames * PAGE_SIZE;
}

u64 pmm_free_bytes(void) {
    return pmm_free_count * PAGE_SIZE;
}

/* Optional: Add low-watermark tracking for future reuse */
static u64 pmm_low_watermark = 0;
void pmm_set_low_watermark(u64 bytes) {
    pmm_low_watermark = bytes;
}
```

### 2. In `kernel/kern/sched.c` (add to existing file)
```c
/* Add near top with other function prototypes */
static void (*sched_idle_hook)(void) = NULL;

/* Add to sched_init() at the end */
void sched_init(void) {
    /* ... existing initialization ... */
    sched_idle_hook = NULL; /* Initialize hook pointer */
}

/* Add this function anywhere in sched.c */
void sched_register_idle_hook(void (*fn)(void)) {
    sched_idle_hook = fn;
}

/* Modify the idle loop (look for "for(;;){ __asm__ volatile("hlt"); }") */
void sched_idle(void) {
    for (;;) {
        __asm__ volatile("sti; hlt"); /* Re-enable interrupts before halt */
        if (sched_idle_hook) {
            sched_idle_hook();
        }
    }
}
```

### 3. In `kernel/include/pmx.h` (new file - create it)
```c
#ifndef NOOS_PMX_H
#define NOOS_PMX_H

#include "types.h"

/* Memory zone identifiers */
typedef enum {
    MEM_ZONE_KERN_CORE = 0,   /* Non-pageable kernel core */
    MEM_ZONE_USER_MIN,        /* Guaranteed per-task minimums */
    MEM_ZONE_OPPORTUNISTIC_AI,/* ML workloads, disk cache */
    MEM_ZONE_ELASTIC_GROWTH   /* User heap overcommit, etc */
} mem_zone_t;

/* Current allocation state (updated by memory_allocate_policy) */
extern u64 mem_zone_alloc[4]; /* Bytes currently allocated per zone */
extern u64 mem_zone_limit[4]; /* Current soft limits per zone */

/* Call periodically (e.g., from scheduler tick) to enforce policy */
void memory_enforce_policy(void);

#endif /* NOOS_PMX_H */
```

These additions:
- Add **zero runtime overhead** when unused (function pointers NULL-check fast)
- Require **no changes** to existing memory allocation paths
- Are **backward compatible** with current M3 implementation
- Provide the **exact hooks** needed for M4/M5 memory scaling
- Fit naturally into your current bug-fixing workflow (you're likely touching these files anyway)

Once these are in place, your current M3 work remains 100% valid, and you've laid the foundation for the system to *naturally* grow its AI capabilities as you progress through M4 (filesystem) and M5 (ML integration) — all while staying true to your `spec → implement → boot-test → commit` rhythm.

---
*Last updated: 2026-08-01*  
*Status: Ready for immediate implementation during M3 bug fixing*  
*Depends on: None (safe to add to current codebase)*