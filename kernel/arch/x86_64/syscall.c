#include "sched.h"
#include "syscall.h"
#include "pit.h"
#include "kbd.h"
#include "vmm.h"
#include "pmm.h"
#include "noc_os.h"
#include "model.h"
#include "string.h"
#include "printk.h"

#define PIT_HZ 100

/* Copy a user string with a bounds check against the user region. */
static void sys_puts(const char *s)
{
    u64 a = (u64)s;
    if (a < USER_IMAGE_BASE || a >= USER_REGION_END)
        return;
    for (usize i = 0; i < 4096; i++) {
        char c = s[i];
        if (!c)
            break;
        noc_os_putc(c);
    }
}

static u64 sys_alloc(task_t *t, usize n)
{
    n = (n + 15) & ~(usize)15;
    if (t->heap_used + n > 0x200000)
        return 0;
    u64 base = USER_HEAP_BASE + t->heap_used;
    usize want = (t->heap_used + n + 4095) / 4096;
    while (t->heap_pages < want) {
        u64 fr = pmm_alloc_frame();
        if (!fr)
            return 0;
        memset((void *)fr, 0, 4096);
        vmm_map(t->cr3, USER_HEAP_BASE + t->heap_pages * 4096, fr,
                VMM_USER | VMM_WRITE);
        t->heap_pages++;
    }
    t->heap_used += n;
    return base;
}

void syscall_dispatch(struct regs *r)
{
    u64 num = r->rax;
    u64 a1  = r->rdi;
    task_t *t = sched_current();

    switch (num) {
    case SYS_EXIT:
        sched_exit_user(r, (i64)a1);
        break; /* not reached */
    case SYS_PUTC:
        noc_os_putc((char)a1);
        r->rax = 0;
        break;
    case SYS_PUTS:
        sys_puts((const char *)a1);
        r->rax = 0;
        break;
    case SYS_SLEEP: {
        u64 ms = a1;
        r->rax = 0;
        sched_block_user(r, pit_ticks() + ms * PIT_HZ / 1000, 0);
        break;
    }
    case SYS_TICKS:
        r->rax = pit_ticks();
        break;
    case SYS_KBD_POLL:
        r->rax = (u64)(i64)kbd_getc();
        break;
    case SYS_KBD_PEEK:
        r->rax = (u64)(i64)kbd_peekc();
        break;
    case SYS_KBD_WAIT:
        for (;;) {
            int c = kbd_getc();
            if (c >= 0) {
                r->rax = (u64)(i64)c;
                break;
            }
            sched_block_user(r, 0, 1);
        }
        break;
    case SYS_ALLOC:
        r->rax = sys_alloc(t, (usize)a1);
        break;
    case SYS_FREE:
        r->rax = 0; /* user heap is a bump allocator for M3 */
        break;
    case SYS_YIELD:
        sched_preempt(r);
        break;
    case SYS_MODEL_BUDGET: {
        u64 kb = a1;
        if (kb > 65536)
            kb = 65536;
        t->model_budget_kb = (u32)kb;
        r->rax = kb;
        break;
    }
    case SYS_MODEL_COMMIT: {
        u64 need = a1 * 4; /* 4 KiB per weight page */
        if ((u64)t->model_weights_kb + need > (u64)t->model_budget_kb) {
            r->rax = (u64)-1; /* over budget: caller must degrade gracefully */
        } else {
            t->model_weights_kb += (u32)need;
            r->rax = 0;
        }
        break;
    }
    case SYS_MODEL_TOUCH: {
        usize pg = (usize)a1;
        if (pg >= model_window_pages()) {
            r->rax = (u64)-1;
            break;
        }
        r->rax = model_demand_fault(t, USER_MODEL_BASE + pg * 4096, 0)
                     ? 0 : (u64)-1;
        break;
    }
    case SYS_MODEL_EVICT:
        r->rax = model_evict_page(t, (usize)a1) ? 0 : (u64)-1;
        break;
    case SYS_MODEL_STATS: {
        printk("model: faults=%u resident=%u used=%u KB budget=%u KB\n",
               (unsigned)t->model_faults,
               (unsigned)model_resident_pages(t),
               (unsigned)t->model_weights_kb,
               (unsigned)t->model_budget_kb);
        r->rax = 0;
        break;
    }
    default:
        r->rax = (u64)-1;
        break;
    }
}
