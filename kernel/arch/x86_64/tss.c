#include "tss.h"
#include "gdt.h"

static tss_t tss __attribute__((aligned(16)));

void tss_init(void)
{
    tss.rsvd0      = 0;
    tss.rsp0       = 0;
    tss.rsp1       = 0;
    tss.rsp2       = 0;
    tss.rsvd1      = 0;
    tss.ist1       = 0;
    tss.ist2       = 0;
    tss.ist3       = 0;
    tss.ist4       = 0;
    tss.ist5       = 0;
    tss.ist6       = 0;
    tss.ist7       = 0;
    tss.rsvd2      = 0;
    tss.rsvd3      = 0;
    tss.iomap_base = TSS_SIZE; /* no IO bitmap: base == limit end */

    gdt_set_tss((u64)&tss);

    __asm__ volatile("ltr %0" ::"r"((u16)GDT_TSS));
}

void tss_set_rsp0(u64 rsp0)
{
    tss.rsp0 = rsp0;
}
