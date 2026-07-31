#ifndef NOOS_TSS_H
#define NOOS_TSS_H

#include "types.h"

#define TSS_SIZE 104

typedef struct __attribute__((packed)) {
    u32 rsvd0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 rsvd1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 rsvd2;
    u16 rsvd3;
    u16 iomap_base;
} tss_t;

void tss_init(void);
void tss_set_rsp0(u64 rsp0);

#endif
