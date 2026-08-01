#include "model.h"
#include "vmm.h"
#include "pmm.h"
#include "trans.h"
#include "pgpred.h"
#include "string.h"
#include "printk.h"
#include "heap.h"

#define MODEL_PG 4096
#define MODEL_BITMAP_WORDS(n) (((n) + 31) / 32)

/* Kernel-wide model window size in pages. Set by model_window_setup() to the
   current transformer blob size (trans_weight_pages()); everything below
   (range checks, syscall page validation, per-task bitmap sizing) derives
   from it, never from a hardcoded constant. */
static u32 win_pages = 16; /* fallback before trans_init runs */

u32 model_window_pages(void)
{
    return win_pages;
}

void model_window_setup(void)
{
    u32 p = trans_weight_pages();
    if (p < 1)
        p = 1;
    if (p > USER_MODEL_MAX / MODEL_PG)
        p = (u32)(USER_MODEL_MAX / MODEL_PG);
    win_pages = p;
}

bool model_in_range(u64 addr)
{
    return addr >= USER_MODEL_BASE &&
           addr - USER_MODEL_BASE < (u64)win_pages * MODEL_PG;
}

/* Resident page count derived from the task's bitmap. */
u32 model_resident_pages(const task_t *t)
{
    u32 n = 0;
    u32 words = MODEL_BITMAP_WORDS(t->model_frames_n);
    if (t->model_map)
        for (u32 i = 0; i < words; i++)
            for (u32 b = t->model_map[i]; b; b &= b - 1)
                n++;
    return n;
}

/* Ensure the task's per-task model arrays cover `pages` model pages.
   Allocated lazily on the first demand fault; the resident-page budget and
   page indices live here, and model_exit_task() reclaims them. */
static bool model_ensure_task(task_t *t, u32 pages)
{
    if (t->model_frames_n >= pages && t->model_frames && t->model_map)
        return true;
    u32 words = MODEL_BITMAP_WORDS(pages);
    u64 *frames = kmalloc(sizeof(u64) * pages);
    u32 *map = kmalloc(sizeof(u32) * words);
    if (!frames || !map) {
        if (frames)
            kfree(frames);
        if (map)
            kfree(map);
        return false;
    }
    for (u32 i = 0; i < pages; i++)
        frames[i] = 0;
    for (u32 i = 0; i < words; i++)
        map[i] = 0;
    if (t->model_frames)
        kfree(t->model_frames);
    if (t->model_map)
        kfree(t->model_map);
    t->model_frames = frames;
    t->model_map = map;
    t->model_frames_n = pages;
    return true;
}

void model_exit_task(task_t *t)
{
    if (t->model_frames)
        kfree(t->model_frames);
    if (t->model_map)
        kfree(t->model_map);
    t->model_frames = NULL;
    t->model_map = NULL;
    t->model_frames_n = 0;
}

static bool model_page_resident(const task_t *t, usize pg)
{
    if (!t->model_map)
        return false;
    return (t->model_map[pg >> 5] >> (pg & 31)) & 1u;
}

static void model_page_set(task_t *t, usize pg, bool resident)
{
    u32 *w = &t->model_map[pg >> 5];
    u32 bit = 1u << (pg & 31);
    if (resident)
        *w |= bit;
    else
        *w &= ~bit;
}

/* Copy the canonical weight page into a fresh frame, map it read-only in the
   task, charge the budget. Shared by the demand-fault path and the prefetch
   action below. Returns false on allocation failure (caller reports). */
static bool model_fault_in_page(task_t *t, usize pg)
{
    u64 frame = pmm_alloc_frame();
    if (!frame)
        return false;
    const u8 *weights = trans_weights();
    memcpy((void *)frame, weights + pg * MODEL_PG, MODEL_PG);
    vmm_map(t->cr3, USER_MODEL_BASE + pg * MODEL_PG, frame, VMM_USER);
    t->model_frames[pg] = frame;
    model_page_set(t, pg, true);
    t->model_weights_kb += 4;
    return true;
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
    if (pg >= win_pages)
        return false;
    if (!model_ensure_task(t, win_pages))
        return false;
    if (model_page_resident(t, pg))
        return true; /* already resident: nothing to fault in */

    if ((u64)t->model_weights_kb + 4 > (u64)t->model_budget_kb) {
        printk("model: pg %u denied (budget %u KB)\n", (unsigned)pg,
               (unsigned)t->model_budget_kb);
        return false;
    }

    if (!model_fault_in_page(t, pg)) {
        printk("model: pg %u out of frames\n", (unsigned)pg);
        return false;
    }
    t->model_faults++;
    printk("model: fault-in pg=%u resident=%u used=%u KB\n", (unsigned)pg,
           (unsigned)model_resident_pages(t), (unsigned)t->model_weights_kb);
    return true;
}

/* Prefetch action (ROADMAP: "pre-map/pre-load the predicted page"): asked by
   the page-fault dispatcher after a model-window demand fault is handled.
   Asks the page-access predictor which page it expects next; if that page is
   another model page, not yet resident, and within budget, pre-load it so the
   next access is a hit instead of a fault. Prints what it did; no-op when the
   prediction is out of window, already resident, or denied. */
void model_prefetch(task_t *t)
{
    u64 p = pgpred_predict();
    if (!p)
        return;
    u64 vaddr = p << 12;
    if (!model_in_range(vaddr))
        return;
    usize pg = (usize)((vaddr - USER_MODEL_BASE) / MODEL_PG);
    if (pg >= win_pages)
        return;
    if (!model_ensure_task(t, win_pages))
        return;
    if (model_page_resident(t, pg))
        return; /* already resident: nothing to prefetch */

    if ((u64)t->model_weights_kb + 4 > (u64)t->model_budget_kb) {
        printk("model: prefetch pg=%u denied (budget %u KB)\n", (unsigned)pg,
               (unsigned)t->model_budget_kb);
        return;
    }
    if (!model_fault_in_page(t, pg)) {
        printk("model: prefetch pg=%u out of frames\n", (unsigned)pg);
        return;
    }
    printk("model: prefetch pg=%u resident=%u used=%u KB\n", (unsigned)pg,
           (unsigned)model_resident_pages(t), (unsigned)t->model_weights_kb);
}

/* Explicitly evict a resident model page: unmap, free the frame, refund the
   budget. The next touch re-faults the page in from the canonical copy.
   Returns false when the page was not resident (nothing to do). */
bool model_evict_page(task_t *t, usize pg)
{
    if (pg >= win_pages)
        return false;
    if (!t->model_map || !model_page_resident(t, pg))
        return false;

    u64 va = USER_MODEL_BASE + pg * MODEL_PG;
    vmm_unmap(t->cr3, va);
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    pmm_free_frame(t->model_frames[pg]);
    t->model_frames[pg] = 0;
    model_page_set(t, pg, false);
    t->model_weights_kb -= 4;
    printk("model: evict pg=%u resident=%u used=%u KB\n", (unsigned)pg,
           (unsigned)model_resident_pages(t), (unsigned)t->model_weights_kb);
    return true;
}

/* Per-task half of model_invalidate_all: drop every resident model page of
   one task. Iterates the full array length (model_frames_n, the window size
   the arrays were sized for), so pages that fall outside a *shrunk* window
   are still unmapped and refunded rather than silently stranded. */
static void model_invalidate_task(task_t *t)
{
    if (!t->model_frames || !t->model_map)
        return;
    for (usize pg = 0; pg < t->model_frames_n; pg++) {
        if (!model_page_resident(t, pg))
            continue;
        u64 va = USER_MODEL_BASE + pg * MODEL_PG;
        vmm_unmap(t->cr3, va);
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
        pmm_free_frame(t->model_frames[pg]);
        t->model_frames[pg] = 0;
        model_page_set(t, pg, false);
        t->model_weights_kb -= 4;
    }
    printk("model: invalidate pid=%u resident=0 used=%u KB\n",
           (unsigned)t->pid, (unsigned)t->model_weights_kb);
}

void model_invalidate_all(void)
{
    sched_foreach_user(model_invalidate_task);
}
