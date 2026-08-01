#include "isr.h"
#include "printk.h"
#include "sched.h"
#include "syscall.h"
#include "gdt.h"
#include "pgpred.h"
#include "model.h"

#define EXC_COUNT 32

static void (*handlers[256])(struct regs *r);

static const char *exc_names[EXC_COUNT] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FPU Error", "Alignment Check",
    "Machine Check", "SIMD Floating-Point", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved",
};

void isr_register(u64 vector, void (*handler)(struct regs *r))
{
    if (vector < 256)
        handlers[vector] = handler;
}

static void isr_panic(u64 vector, u64 error, struct regs *r)
{
    printk("\n*** EXCEPTION: %s (vector %u) ***\n",
           exc_names[vector], (unsigned)vector);
    printk("    error=0x%llx\n", error);
    if (vector == 14) { /* page fault */
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        pgpred_fault(cr2);
        printk("    CR2 (fault address)=0x%llx\n", cr2);
    }
    printk("    RIP=0x%llx CS=0x%x RFLAGS=0x%llx RSP=0x%llx\n",
           r->rip, (unsigned)r->cs, r->rflags, r->rsp);
    printk("    RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n",
           r->rax, r->rbx, r->rcx, r->rdx);
    printk("    RSI=0x%llx RDI=0x%llx RBP=0x%llx\n",
           r->rsi, r->rdi, r->rbp);
    printk("*** KERNEL PANIC: halted ***\n");
    __asm__ volatile("cli; hlt; 1: jmp 1b");
}

void isr_dispatch(u64 vector, u64 error, struct regs *r)
{
    if (vector == 0x80) {
        syscall_dispatch(r);
        return;
    }

    if (vector < EXC_COUNT) {
        if ((r->cs & ~3) == GDT_UCODE) {
            task_t *t = sched_current();
            if (vector == 14) { /* page fault */
                u64 cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                if (model_demand_fault(t, cr2, error)) {
                    /* Demand-paged a read-only model weight page in; retry
                       the faulting instruction with the page now mapped. */
                    return;
                }
                printk("process %u (%s) killed: %s (rip=0x%llx err=0x%llx)\n",
                       (unsigned)t->pid, t->name, exc_names[vector],
                       r->rip, error);
                pgpred_fault(cr2);
                printk("  cr2=0x%llx\n", cr2);
                sched_exit_user(r, -1);
                return;
            }
            printk("process %u (%s) killed: %s (rip=0x%llx err=0x%llx)\n",
                   (unsigned)t->pid, t->name, exc_names[vector], r->rip, error);
            sched_exit_user(r, -1);
            return;
        }
        isr_panic(vector, error, r);
        return;
    }

    if (handlers[vector])
        handlers[vector](r);
    /* Unhandled IRQ: drop it. */
}
