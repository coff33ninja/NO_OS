#ifndef NOOS_PIC_H
#define NOOS_PIC_H

#include "types.h"

void pic_init(void);
void pic_eoi(u8 irq);
void pic_mask(u8 irq, bool masked);

#endif
