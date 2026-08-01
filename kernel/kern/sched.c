#include "sched.h"
#include "vmm.h"
#include "gdt.h"
#include "tss.h"
#include "heap.h"
#include "printk.h"
#include "string.h"
#include "pit.h"
#include "pmm.h"
#include "interact.h"

extern int coro_save(coro_t *out);
extern void coro_restore(coro_t *in);
extern void enter_user_mode(struct regs *frame);

extern char _binary_nocproc_bin_start[];
extern char _binary_nocproc_bin_end[];

static task_t tasks[TASK_MAX];
static task_t *current;
static u32 next_pid = 1;
static u32 sched_last; /* last picked user task index (round-robin) */
static void (*sched_idle_hook)(void) = NULL;

task_t *sched_current(void)
{
    return current;
}

static task_t *sched_pick_user(void)
{
    for (u32 i = 0; i < TASK_MAX; i++) {
        u32 idx = (sched_last + 1 + i) % TASK_MAX;
        if (idx == 0)
            continue; /* task 0 (REPL) is scheduled via the coroutine */
        task_t *t = &tasks[idx];
        if (t->user && t->state == TASK_RUNNABLE) {
            sched_last = idx;
            return t;
        }
    }
    return NULL;
}

/* Build a fresh ring-3 entry frame (never-run task). */
static void sched_entry_frame(struct regs *f)
{
    memset(f, 0, sizeof(*f));
    f->rip    = USER_IMAGE_BASE; /* user blob entry */
    f->cs     = GDT_UCODE | 3;
    f->rflags = 0x202;      /* IF set */
    f->rsp    = USER_STACK_TOP;
    f->ss     = GDT_UDATA | 3;
}

/* Called from an ISR whose frame is on the current user task's kernel
   stack. Saves it, picks the next user task, and overwrites the frame in
   place; the ISR epilogue iretq's it. Falls back to resuming the REPL
   coroutine when no user task is runnable. */
static void sched_switch_to_next(struct regs *r)
{
    task_t *next = sched_pick_user();
    if (!next) {
        current = &tasks[0];
        tasks[0].state = TASK_RUNNABLE;
        tss_set_rsp0((u64)tasks[0].kstack + tasks[0].kstack_size);
        vmm_load_cr3(vmm_kernel_cr3());
        coro_restore(&tasks[0].coro); /* never returns */
    }
    current = next;
    current->state = TASK_RUNNING;
    tss_set_rsp0((u64)next->kstack + next->kstack_size);
    vmm_load_cr3(next->cr3);
    if (!next->started) {
        sched_entry_frame(&next->ctx);
        next->started = 1;
    }
    *r = next->ctx;
}

void sched_yield_to_user(void)
{
    task_t *next = sched_pick_user();
    if (!next)
        return;
    if (coro_save(&tasks[0].coro))
        return; /* resumed: REPL continues */

    current = next;
    current->state = TASK_RUNNING;
    tss_set_rsp0((u64)next->kstack + next->kstack_size);
    vmm_load_cr3(next->cr3);

    if (!next->started) {
        sched_entry_frame(&next->ctx);
        next->started = 1;
    }

    struct regs frame = next->ctx;
    enter_user_mode(&frame);   /* never returns */
}

void sched_preempt(struct regs *r)
{
    current->ctx = *r;
    current->state = TASK_RUNNABLE;
    sched_switch_to_next(r);
}

void sched_block_user(struct regs *r, u64 wake_ticks, u8 on_key)
{
    current->ctx = *r;
    current->state = TASK_BLOCKED;
    current->wake_ticks = wake_ticks;
    current->on_key = on_key;
    sched_switch_to_next(r);
}

void sched_exit_user(struct regs *r, i64 code)
{
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
    il_event_exit(current->pid, code);
    printk("process %u (%s) exited: %d\n",
           (unsigned)current->pid, current->name, (int)code);
    if (current->cr3)
        vmm_free_address_space(current->cr3);
    current->cr3 = 0;
    sched_switch_to_next(r);
}

void sched_on_tick(struct regs *r)
{
    u64 now = pit_ticks();
    for (u32 i = 1; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->wake_ticks &&
            t->wake_ticks <= now) {
            t->state = TASK_RUNNABLE;
            t->wake_ticks = 0;
        }
    }
    if (current && current->user)
        sched_preempt(r);
}

void sched_on_key(void)
{
    for (u32 i = 1; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->on_key) {
            t->state = TASK_RUNNABLE;
            t->on_key = 0;
        }
    }
}

void sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));

    tasks[0].pid         = 0;
    tasks[0].state       = TASK_RUNNABLE;
    tasks[0].name        = "repl";
    tasks[0].user        = 0;
    tasks[0].kstack      = kmalloc(TASK_KSTACK_SIZE);
    tasks[0].kstack_size = TASK_KSTACK_SIZE;
    tasks[0].cr3         = 0;
    tasks[0].model_budget_kb = 8192;

    current = &tasks[0];
    tss_set_rsp0((u64)tasks[0].kstack + TASK_KSTACK_SIZE);
}

i64 sched_spawn(const char *code, const char *name)
{
    task_t *t = NULL;
    for (u32 i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_ZOMBIE || tasks[i].pid == 0) {
            t = &tasks[i];
            break;
        }
    }
    if (!t)
        return -1;

    if (t->kstack)
        kfree(t->kstack);
    memset(t, 0, sizeof(task_t));
    t->model_budget_kb = 8192;

    u64 cr3 = vmm_new_address_space();
    if (!cr3)
        return -1;

    u8 *kstack = kmalloc(TASK_KSTACK_SIZE);
    if (!kstack) {
        vmm_free_address_space(cr3);
        return -1;
    }

    /* Copy the embedded user runtime blob into fresh frames at the user
       image base (PML4[2]). The blob is not page-aligned in the kernel
       image (_start is linked at USER_IMAGE_BASE), so copy unaligned from
       blob_addr to keep blob[0] == USER_IMAGE_BASE[0]. (Aliasing the
       kernel's blob pages would free kernel image memory when the address
       space is torn down.) */
    u64 blob_addr  = (u64)_binary_nocproc_bin_start;
    usize blob_pages =
        ((u64)_binary_nocproc_bin_end - blob_addr + 4095) / 4096;
    for (usize i = 0; i < blob_pages; i++) {
        u64 fr = pmm_alloc_frame();
        if (!fr)
            goto fail;
        memcpy((void *)fr, (void *)(blob_addr + i * 4096), 4096);
        vmm_map(cr3, USER_IMAGE_BASE + i * 4096, fr,
                VMM_USER | VMM_WRITE);
    }

    /* Map the script scratch (32 KiB) and copy the source in. */
    u64 scratch_frame = 0;
    for (usize i = 0; i < 8; i++) {
        u64 fr = pmm_alloc_frame();
        if (!fr)
            goto fail;
        if (i == 0)
            scratch_frame = fr;
        vmm_map(cr3, USER_SCRIPT_BASE + i * 4096, fr,
                VMM_USER | VMM_WRITE);
    }
    usize slen = strlen(code);
    if (slen > 32768)
        slen = 32768;
    memcpy((void *)scratch_frame, code, slen);
    ((char *)scratch_frame)[slen] = '\0';

    /* Map the user stack (64 KiB at USER_STACK_BASE). */
    for (usize i = 0; i < 16; i++) {
        u64 fr = pmm_alloc_frame();
        if (!fr)
            goto fail;
        vmm_map(cr3, USER_STACK_BASE + i * 4096, fr,
                VMM_USER | VMM_WRITE);
    }

    t->pid         = next_pid++;
    t->state       = TASK_RUNNABLE;
    t->name        = name;
    t->kstack      = kstack;
    t->kstack_size = TASK_KSTACK_SIZE;
    t->cr3         = cr3;
    t->user        = 1;
    t->heap_used   = 0;
    t->heap_pages  = 0;
    il_event_spawn((u32)t->pid, name);
    return (i64)t->pid;

fail:
    kfree(kstack);
    vmm_free_address_space(cr3);
    return -1;
}

static const char *state_name(task_state_t s)
{
    switch (s) {
    case TASK_RUNNABLE: return "ready";
    case TASK_RUNNING:  return "run";
    case TASK_BLOCKED:  return "blocked";
    case TASK_ZOMBIE:   return "zombie";
    }
    return "?";
}

void sched_ps(void)
{
    printk("pid  state   kind  name\n");
    for (u32 i = 0; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        if (!t->kstack)
            continue; /* unused slot */
        printk("%u  %s  %s  %s\n",
               (unsigned)t->pid, state_name(t->state),
               t->user ? "user" : "kern", t->name);
    }
}

void sched_register_idle_hook(void (*fn)(void))
{
    sched_idle_hook = fn;
}

void sched_idle(void)
{
    for (;;) {
        __asm__ volatile("sti");
        if (sched_idle_hook)
            sched_idle_hook();
        __asm__ volatile("hlt");
    }
}