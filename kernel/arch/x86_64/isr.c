#include "isr.h"
#include "printk.h"

#define EXC_COUNT 32

static void (*handlers[48])(struct regs *r);

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
    if (vector < 48)
        handlers[vector] = handler;
}

void isr_dispatch(u64 vector, u64 error, struct regs *r)
{
    if (vector < EXC_COUNT) {
        printk("\n*** EXCEPTION: %s (vector %u) ***\n",
               exc_names[vector], (unsigned)vector);
        printk("    error=0x%llx\n", error);
        if (vector == 14) { /* page fault */
            u64 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
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
    } else if (handlers[vector]) {
        handlers[vector](r);
    }
    /* Unhandled IRQ: drop it. */
}
