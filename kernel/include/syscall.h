#ifndef NOOS_SYSCALL_H
#define NOOS_SYSCALL_H

#include "isr.h"

#define SYS_EXIT     0
#define SYS_PUTC     1
#define SYS_PUTS     2
#define SYS_SLEEP    3
#define SYS_TICKS    4
#define SYS_KBD_POLL 5
#define SYS_KBD_WAIT 6
#define SYS_ALLOC    7
#define SYS_FREE     8
#define SYS_YIELD    9
#define SYS_KBD_PEEK 10
#define SYS_MODEL_BUDGET 11
#define SYS_MODEL_COMMIT 12
#define SYS_MODEL_TOUCH  13
#define SYS_MODEL_EVICT  14
#define SYS_MODEL_STATS  15

void syscall_dispatch(struct regs *r);

#endif
