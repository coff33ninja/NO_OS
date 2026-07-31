#ifndef NOOS_HEAP_H
#define NOOS_HEAP_H

#include "types.h"

void *kmalloc(usize size);
void kfree(void *ptr);

usize heap_used_bytes(void);
usize heap_blocks(void);

#endif
