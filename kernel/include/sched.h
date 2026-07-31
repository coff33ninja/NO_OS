#ifndef NOOS_SCHED_H
#define NOOS_SCHED_H

#include "types.h"
#include "isr.h"

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

#endif