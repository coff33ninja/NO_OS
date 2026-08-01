#ifndef NOOS_SCHED_H
#define NOOS_SCHED_H

#include "types.h"
#include "isr.h"
#include "vmm.h"

#define TASK_MAX          16
#define TASK_KSTACK_SIZE  (16 * 1024)

typedef enum {
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef struct {
    u64 rsp;
    u64 rbx, rbp, r12, r13, r14, r15;
} coro_t;

/* One swapped-out page for a task: the swap-region slot holding the page
   whose virtual address is (vkey << 12). Looked up by linear scan on a page
   fault; the array is capped so no dynamic allocation is needed per task. */
#define SWAP_MAX_PER_TASK 128
typedef struct {
    u64  vkey; /* vaddr >> 12 */
    u32  slot; /* swap region slot index */
} swap_ent_t;

typedef struct {
    u32         pid;
    task_state_t state;
    const char *name;
    u8         *kstack;
    usize       kstack_size;
    struct regs ctx;
    u64         cr3;
    u64         wake_ticks;
    u8          user;       /* 1 = ring 3 task */
    u8          started;    /* 1 = has been entered at least once */
    u8          on_key;     /* blocked waiting for a key */
    usize       heap_used;  /* user heap bump cursor (bytes) */
    usize       heap_pages; /* user heap pages mapped */
    u32         model_budget_kb;  /* model weight RAM cap, default 8192 */
    u32         model_weights_kb; /* resident weight pages (KB) */
    u32         model_faults;     /* demand faults serviced, lifetime */
    usize       model_frames_n;   /* allocated length of frames/map (pages) */
    u64        *model_frames;     /* backing frame per page (lazy, kmalloc) */
    u32        *model_map;        /* resident page bitmap (lazy, kmalloc) */
    swap_ent_t  swap_map[SWAP_MAX_PER_TASK]; /* pages currently swapped out */
    u16         swap_count;       /* live entries in swap_map */
    i64         exit_code;
    coro_t      coro;       /* kernel task (task 0) coroutine context */
} task_t;

void sched_init(void);
task_t *sched_current(void);

/* REPL (task 0, kernel) gives the CPU to a runnable user task, if any.
   Returns immediately (possibly resumed later) if none is runnable. */
void sched_yield_to_user(void);

/* ISR-context switches for user tasks (frame-swap). */
void sched_preempt(struct regs *r);
void sched_block_user(struct regs *r, u64 wake_ticks, u8 on_key);
void sched_exit_user(struct regs *r, i64 code);

/* Driver hooks. */
void sched_on_tick(struct regs *r);
void sched_on_key(void);

/* Spawn a user process running the NOC script `code`. Returns PID. */
i64 sched_spawn(const char *code, const char *name);

/* Serialize one line of task list for Ps. */
void sched_ps(void);

/* Idle hook for opportunistic work */
void sched_register_idle_hook(void (*fn)(void));
void sched_idle(void);

/* Call `cb` for every live user task (task 0, the kernel REPL, is skipped).
   Used by model_invalidate_all() to reset per-task model state after the
   weight blob is rebuilt. */
void sched_foreach_user(void (*cb)(task_t *t));

#endif