#ifndef NOOS_MODEL_H
#define NOOS_MODEL_H

#include "types.h"
#include "sched.h"

/* Demand-paged, read-only window onto the 64 KiB training weights.
   Model pages are not mapped at process spawn. The first user access to a
   page takes a page fault; the kernel copies the canonical weight bytes
   into a fresh frame, maps it read-only in the process, and charges 4 KiB
   against the task's model RAM budget. A PRESENT page that faults is a
   write to a read-only model page and is refused (the caller kills the
   task). Eviction is explicit (ModelEvict): the frame is freed, the budget
   refunded, and the page re-faults on the next touch. train_weights() is
   the only source of truth, so eviction is always safe. */

bool model_in_range(u64 addr);
bool model_demand_fault(task_t *t, u64 cr2, u64 err);
bool model_evict_page(task_t *t, usize pg);
u32  model_resident_pages(const task_t *t);

#endif
