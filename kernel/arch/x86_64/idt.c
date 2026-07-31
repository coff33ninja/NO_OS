#include "idt.h"
#include "string.h"

typedef struct __attribute__((packed)) {
    u16 limit;
    u64 base;
} idt_ptr_t;

typedef struct __attribute__((packed)) {
    u16 offset_low;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} idt_entry_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

/* 48 stubs, one per vector 0..47 (see isr.s), plus the syscall gate. */
extern u64 isr_stub_table[48];
extern u64 isr80;

void idt_set_gate(u8 vector, u64 handler, u8 dpl)
{
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector    = 0x08;                 /* kernel code */
    idt[vector].ist         = 0;
    idt[vector].type_attr   = 0x8E | (dpl << 5);    /* 64-bit int gate */
    idt[vector].zero        = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));

    for (int i = 0; i < 48; i++)
        idt_set_gate((u8)i, isr_stub_table[i], 0);

    idt_set_gate(0x80, (u64)&isr80, 3);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (u64)&idt;

    __asm__ volatile("lidt %0" ::"m"(idt_ptr));
}
