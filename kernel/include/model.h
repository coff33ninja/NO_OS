#ifndef NOOS_MODEL_H
#define NOOS_MODEL_H

#include "types.h"
#include "sched.h"

/* Demand-paged, read-only window onto the transformer weights (the blob
   served by trans_weights(); it carries a 32-byte config header plus the
   int8 weights). The window is dynamically sized: model_window_pages() is
   set by trans_init / trans_config to exactly the blob's page count, so it
   scales with the RAM-driven model.

   Model pages are not mapped at process spawn. The first user access to a
   page takes a page fault; the kernel copies the canonical weight bytes
   into a fresh frame, maps it read-only in the process, and charges 4 KiB
   against the task's model RAM budget. A PRESENT page that faults is a
   write to a read-only model page and is refused (the caller kills the
   task). Eviction is explicit (ModelEvict): the frame is freed, the budget
   refunded, and the page re-faults on the next touch. trans_weights() is
   the only source of truth, so eviction is always safe.

   Per-task state (model_frames, model_map) is allocated lazily on the first
   demand fault and freed at task exit, so a kernel build for any RAM size
   costs nothing until a user process actually touches the model. */

bool model_in_range(u64 addr);
bool model_demand_fault(task_t *t, u64 cr2, u64 err);
bool model_evict_page(task_t *t, usize pg);
u32  model_resident_pages(const task_t *t);

/* Prefetch action: after a model-window fault is handled, ask the page-access
   predictor which page it expects next and pre-load it if it is another model
   page, not resident, and within budget. No-op when the prediction is out of
   window / already resident / denied. */
void model_prefetch(task_t *t);

/* Current resident model window size in pages (derived from the weight
   blob). */
u32  model_window_pages(void);

/* Re-derive the window size from the current blob. Called by trans_init /
   trans_config / trans_set_act_cap after (re)allocating the blob. */
void model_window_setup(void);

/* Free a task's per-task model arrays (frames + bitmap). Called at task
   exit; resident model frames are unmapped with the address space, so only
   the arrays themselves need freeing. */
void model_exit_task(task_t *t);

/* Drop every user task's resident model pages (unmap + free + refund budget)
   and clear the bitmaps. Called after the blob is reallocated (trans_config /
   trans_reset), so stale frames can never survive a model rebuild. The
   per-task arrays themselves survive and are reused on the next fault. */
void model_invalidate_all(void);

#endif
