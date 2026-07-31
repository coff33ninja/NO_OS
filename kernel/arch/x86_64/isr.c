#include "isr.h"
#include "printk.h"
#include "sched.h"
#include "syscall.h"
#include "gdt.h"

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
            printk("process %u (%s) killed: %s (rip=0x%llx err=0x%llx)\n",
                   (unsigned)t->pid, t->name, exc_names[vector], r->rip, error);
            if (vector == 14) { /* page fault */
                u64 cr2;
                __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                printk("  cr2=0x%llx (P=%d U=%d I/D=%d)\n", cr2,
                       (int)(error & 1), (int)((error >> 2) & 1),
                       (int)((error >> 4) & 1));
                u8 *pc = (u8 *)r->rip;
                printk("  [%p]=%x %x %x %x\n", pc,
                       pc[0], pc[1], pc[2], pc[3]);
                u64 cr3v;
                __asm__ volatile("mov %%cr3, %0" : "=r"(cr3v));
                u64 *pl = (u64 *)cr3v;
                u64 pml4i = (r->rip >> 39) & 0x1FF;
                u64 pdpti = (r->rip >> 30) & 0x1FF;
                u64 pdi   = (r->rip >> 21) & 0x1FF;
                u64 pti   = (r->rip >> 12) & 0x1FF;
                printk("  cr3=0x%llx pml4[%u]=0x%llx\n", cr3v,
                       (unsigned)pml4i, pl[pml4i]);
                if (pl[pml4i] & 1) {
                    u64 *pdpt = (u64 *)(pl[pml4i] & ~0xFFFUL);
                    printk("  pdpt[%u]=0x%llx\n", (unsigned)pdpti, pdpt[pdpti]);
                    if (pdpt[pdpti] & 1) {
                        u64 *pd = (u64 *)(pdpt[pdpti] & ~0xFFFUL);
                        printk("  pd[%u]=0x%llx\n", (unsigned)pdi, pd[pdi]);
                        if (pd[pdi] & 1) {
                            u64 *pt = (u64 *)(pd[pdi] & ~0xFFFUL);
                            printk("  pt[%u]=0x%llx\n", (unsigned)pti, pt[pti]);
                            if (pt[pti] & 1) {
                                u64 pf = pt[pti] & ~0xFFFUL;
                                u8 *fr = (u8 *)pf + (r->rip & 0xFFF);
                                printk("  phys[0x%llx]=%x %x %x %x\n",
                                       pf, fr[0], fr[1], fr[2], fr[3]);
                            }
                        }
                    }
                }
            }
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
