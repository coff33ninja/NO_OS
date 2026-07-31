#ifndef NOOS_ISR_H
#define NOOS_ISR_H

#include "types.h"

/* Register frame pushed by isr_common_stub in isr.s. */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
};

void isr_dispatch(u64 vector, u64 error, struct regs *r);
void isr_register(u64 vector, void (*handler)(struct regs *r));

#endif
