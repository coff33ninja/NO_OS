#ifndef NOOS_GDT_H
#define NOOS_GDT_H

#define GDT_KCODE 0x08
#define GDT_KDATA 0x10
#define GDT_UCODE 0x18
#define GDT_UDATA 0x20

void gdt_init(void);

#endif
