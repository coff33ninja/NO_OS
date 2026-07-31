#ifndef NOOS_KBD_H
#define NOOS_KBD_H

#include "types.h"

void kbd_init(void);
bool kbd_avail(void);
int  kbd_getc(void);     /* non-blocking; -1 if empty */
int  kbd_readc(void);    /* blocking */

#endif
