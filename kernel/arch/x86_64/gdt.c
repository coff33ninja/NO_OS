#include "gdt.h"
#include "tss.h"
#include "types.h"

typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} gdt_ptr_t;

typedef struct {
    u16 limit0;
    u16 base0;
    u8  base1;
    u8  access;
    u8  gran;
    u8  base2;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    gdt_entry_t lo;
    gdt_entry_t hi;
} __attribute__((packed)) gdt_sys_entry_t;

static gdt_entry_t gdt[7];

static void gdt_set(int idx, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[idx].limit0 = limit & 0xFFFF;
    gdt[idx].base0  = base & 0xFFFF;
    gdt[idx].base1  = (base >> 16) & 0xFF;
    gdt[idx].access = access;
    gdt[idx].gran   = (gran & 0xF0) | ((limit >> 16) & 0x0F);
    gdt[idx].base2  = (base >> 24) & 0xFF;
}

void gdt_set_tss(u64 base)
{
    gdt_sys_entry_t *e = (gdt_sys_entry_t *)&gdt[GDT_TSS_IDX];
    u32 limit = TSS_SIZE;

    e->lo.limit0 = limit & 0xFFFF;
    e->lo.base0  = (u16)base;
    e->lo.base1  = (u8)(base >> 16);
    e->lo.access = 0x89;                 /* present, 64-bit available TSS */
    e->lo.gran   = 0x00;
    e->lo.base2  = (u8)(base >> 24);
    e->hi.base0  = (u16)(base >> 32);
    e->hi.base1  = (u8)(base >> 48);
    e->hi.access = 0;
    e->hi.gran   = 0;
    e->hi.base2  = 0;
}

void gdt_init(void)
{
    gdt_set(0, 0, 0, 0, 0);                /* null */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xA0);    /* kernel code, 64-bit */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);    /* kernel data */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xA0);    /* user code, 64-bit */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);    /* user data */
    gdt_set(5, 0, 0, 0, 0);                /* TSS, filled by gdt_set_tss */
    gdt_set(6, 0, 0, 0, 0);                /* reserved */

    gdt_ptr_t ptr;
    ptr.limit = sizeof(gdt) - 1;
    ptr.base  = (u64)&gdt;

    __asm__ volatile("lgdt %0" ::"m"(ptr));

    /* CS already selects a compatible 64-bit code descriptor (0x08). */
    __asm__ volatile(
        "mov %0, %%ds\n\t"
        "mov %0, %%es\n\t"
        "mov %0, %%fs\n\t"
        "mov %0, %%gs\n\t"
        "mov %0, %%ss\n\t" ::"r"((u16)GDT_KDATA));
}
