#ifndef NOOS_ALLOCATOR_INTERFACE_H
#define NOOS_ALLOCATOR_INTERFACE_H

#include "types.h"
#include "pmm.h"

/**
 * @file allocator_interface.h
 * @brief Dynamic Memory Allocation Interface for NO_OS Growth Model
 *
 * This interface defines the contract between the memory subsystem and
 * opportunistic consumers (AI/ML, disk cache, elastic growth). Implementing
 * these functions enables the system to organically scale its capabilities
 * with available hardware resources while maintaining deterministic guarantees
 * for critical paths.
 *
 * IMPLEMENTATION GUIDE FOR M3/M4:
 * 1. Implement pmm_total_bytes() and pmm_free_bytes() in pmm.c (trivial)
 * 2. Add sched_register_idle_hook() and sched_idle() hook in sched.c (trivial)
 * 3. The memory_enforce_policy() function (to be implemented in M4) will
 *    use these to dynamically adjust zone limits
 *
 * ALL FUNCTIONS ARE SAFE TO IMPLEMENT NOW:
 * - Add zero runtime overhead when unused
 * - Require no changes to existing allocation paths
 * - Are backward compatible with current M3 implementation
 * - Provide the exact telemetry needed for future memory scaling
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* CORE MEMORY TELEMETRY (IMPLEMENT IN M3/M4)                                */
/* ========================================================================== */

/**
 * @brief Get total managed RAM in bytes
 *
 * @return Total RAM available to the physical memory manager
 *
 * @note Implement by returning pmm_total_frames * PAGE_SIZE
 * @note Called periodically by memory allocation policy
 */
u64 pmm_total_bytes(void);

/**
 * @brief Get currently free RAM in bytes
 *
 * @return RAM currently available for allocation
 *
 * @note Implement by returning pmm_free_count * PAGE_SIZE
 * @note Used to detect memory pressure and opportunistic availability
 */
u64 pmm_free_bytes(void);

/**
 * @brief Set low watermark for memory reclamation triggers
 *
 * @param bytes Free RAM threshold below which reclamation should increase
 *
 * @note Optional but recommended for M4 implementation
 * @note Allows fine-tuning of reclamation aggressiveness
 */
void pmm_set_low_watermark(u64 bytes);

/* ========================================================================== */
/* SCHEDULER HOOKS FOR OPPORTUNISTIC ALLOCATION (IMPLEMENT IN M3)           */
/* ========================================================================== */

/**
 * @brief Register a function to be called during scheduler idle periods
 *
 * @param fn Function pointer to call when no tasks are runnable
 *
 * @note Implement by storing fn in a static function pointer
 * @note Called from sched_idle() loop with interrupts enabled
 * @note Safe to call multiple times - last registration wins
 * @note Passing NULL unregisters the hook
 *
 * @example
 *   void my_idle_work(void) {
 *       if (pmm_free_bytes() > 16*MB) {
 *           // Do opportunistic work (e.g., ML training)
 *       }
 *   }
 *   sched_register_idle_hook(my_idle_work);
 */
void sched_register_idle_hook(void (*fn)(void));

/* ========================================================================== */
/* MEMORY ZONE DEFINITIONS (FOR FUTURE REFERENCE)                           */
/* ========================================================================== */

/**
 * @enum mem_zone_t
 * @brief Memory zones in order of allocation priority
 *
 * Zones are allocated in strict priority order. Lower numbers = higher priority.
 * All allocations in a zone must succeed before moving to the next zone.
 */
typedef enum {
    MEM_ZONE_KERN_CORE = 0,   /* Non-pageable kernel core (scheduler, interrupts, pmm/vmm core) */
    MEM_ZONE_USER_MIN,        /* Guaranteed per-task minimums (stack + heap) */
    MEM_ZONE_OPPORTUNISTIC,   /* AI/ML workloads, disk cache, speculative prefetch */
    MEM_ZONE_ELASTIC          /* User heap overcommit, secondary caches, etc */
} mem_zone_t;

/* ========================================================================== */
/* FUTURE ML SCHEDULER INTERFACE (PLACEHOLDER FOR M5)                       */
/* ========================================================================== */

/**
 * @enum ml_priority_t
 * @brief Priority levels for ML allocation requests
 *
 * Higher numbers = higher priority. Used when multiple ML subsystems
 * compete for opportunistic memory.
 */
typedef enum {
    ML_PRIO_BACKGROUND = 0,   /* Training, corpus updates - can be deferred */
    ML_PRIO_INTERACTIVE = 1,  /* Inference, predictions - low latency preferred */
    ML_PRIO_CRITICAL = 2      /* Safety-critical predictions - must not fail */
} ml_priority_t;

/**
 * @brief Request memory for ML workloads
 *
 * @param bytes   Number of bytes requested
 * @param prio    Priority level of the request
 *
 * @return Pointer to allocated memory, or NULL if request cannot be satisfied
 *
 * @note Will be implemented in M5 using the memory zoninng system
 * @note Must check pmm_free_bytes() before calling to avoid unnecessary failures
 * @note Memory obtained via this function is reclaimable under pressure
 */
void* ml_alloc_request(size_t bytes, ml_priority_t prio);

/**
 * @brief Provide usage feedback to the ML scheduler
 *
 * @param used    Bytes actually used since last feedback
 * @param peak    Peak bytes used since last feedback
 *
 * @note Enables dynamic right-sizing of ML allocations
 * @note Called periodically by ML subsystems (e.g., after each inference batch)
 */
void ml_usage_feedback(size_t used, size_t peak);

/* ========================================================================== */
/* MEMORY POLICY ENFORCEMENT (TO BE IMPLEMENTED IN M4)                      */
/* ========================================================================== */

/**
 * @brief Enforce dynamic memory allocation policy
 *
 * @note Call periodically (e.g., from scheduler tick) to adjust zone limits
 * @note Uses pmm_total_bytes() and pmm_free_bytes() to make decisions
 * @note Will be implemented in M4 once basic filesystem exists
 *
 * @example
 *   // In scheduler tick handler:
 *   if (ticks() % 100 == 0) { // Every 100ms
 *       memory_enforce_policy();
 *   }
 */
void memory_enforce_policy(void);

#ifdef __cplusplus
}
#endif

#endif /* NOOS_ALLOCATOR_INTERFACE_H */