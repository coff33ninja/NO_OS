#ifndef NOOS_GDT_H
#define NOOS_GDT_H

#include "types.h"

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UCODE 0x18
#define GDT_UDATA 0x20
#define GDT_TSS   0x28
#define GDT_TSS_IDX 5

void gdt_init(void);
void gdt_set_tss(u64 base);

#endif
