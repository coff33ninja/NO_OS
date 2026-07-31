#ifndef NOOS_PIT_H
#define NOOS_PIT_H

#include "types.h"

void pit_init(void);
usize pit_ticks(void);
void pit_sleep(usize ms);

#endif
