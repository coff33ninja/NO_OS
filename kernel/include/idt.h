#ifndef NOOS_IDT_H
#define NOOS_IDT_H

#include "types.h"

void idt_init(void);
void idt_set_gate(u8 vector, u64 handler, u8 dpl);

#endif
