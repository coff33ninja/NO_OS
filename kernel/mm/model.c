#include "model.h"
#include "vmm.h"
#include "pmm.h"
#include "train.h"
#include "string.h"
#include "printk.h"

#define MODEL_PG 4096

bool model_in_range(u64 addr)
{
    return addr >= USER_MODEL_BASE && addr < USER_MODEL_END;
}

/* Resident page count derived from the task's bitmap (each is 4 KiB). */
u32 model_resident_pages(const task_t *t)
{
    u32 n = 0;
    for (usize i = 0; i < USER_MODEL_PAGES; i++)
        if (t->model_map & (1u << i))
            n++;
    return n;
}

/* Service a user page fault on the model region. Only not-present faults
   are demand-paging requests; a present fault means a write to a read-only
   model page and is refused (the caller kills the task). Returns true when
   the fault was handled and the process may retry the instruction. */
bool model_demand_fault(task_t *t, u64 cr2, u64 err)
{
    if (!model_in_range(cr2))
        return false;
    if (err & 1) /* page was present: write to read-only model page */
        return false;

    usize pg = (usize)((cr2 - USER_MODEL_BASE) / MODEL_PG);
    if (pg >= USER_MODEL_PAGES)
        return false;
    if (t->model_map & (1u << pg))
        return true; /* already resident: nothing to fault in */

    if ((u64)t->model_weights_kb + 4 > (u64)t->model_budget_kb) {
        printk("model: pg %u denied (budget %u KB)\n", (unsigned)pg,
               (unsigned)t->model_budget_kb);
        return false;
    }

    u64 frame = pmm_alloc_frame();
    if (!frame) {
        printk("model: pg %u out of frames\n", (unsigned)pg);
        return false;
    }

    const u8 *weights = train_weights();
    memcpy((void *)frame, weights + pg * MODEL_PG, MODEL_PG);
    vmm_map(t->cr3, USER_MODEL_BASE + pg * MODEL_PG, frame, VMM_USER);
    t->model_frames[pg] = frame;
    t->model_map |= (1u << pg);
    t->model_weights_kb += 4;
    t->model_faults++;
    printk("model: fault-in pg=%u resident=%u used=%u KB\n", (unsigned)pg,
           (unsigned)model_resident_pages(t), (unsigned)t->model_weights_kb);
    return true;
}

/* Explicitly evict a resident model page: unmap, free the frame, refund the
   budget. The next touch re-faults the page in from the canonical copy.
   Returns false when the page was not resident (nothing to do). */
bool model_evict_page(task_t *t, usize pg)
{
    if (pg >= USER_MODEL_PAGES)
        return false;
    if (!(t->model_map & (1u << pg)))
        return false;

    u64 va = USER_MODEL_BASE + pg * MODEL_PG;
    vmm_unmap(t->cr3, va);
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    pmm_free_frame(t->model_frames[pg]);
    t->model_frames[pg] = 0;
    t->model_map &= ~(1u << pg);
    t->model_weights_kb -= 4;
    printk("model: evict pg=%u resident=%u used=%u KB\n", (unsigned)pg,
           (unsigned)model_resident_pages(t), (unsigned)t->model_weights_kb);
    return true;
}
