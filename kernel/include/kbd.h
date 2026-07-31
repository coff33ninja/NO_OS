#ifndef NOOS_KBD_H
#define NOOS_KBD_H

#include "types.h"

/* Extended key codes (values above the ASCII range, unique in the buffer). */
#define KBD_UP    0x80
#define KBD_DOWN  0x81
#define KBD_LEFT  0x82
#define KBD_RIGHT 0x83
#define KBD_HOME  0x84
#define KBD_END   0x85
#define KBD_DEL   0x86

void kbd_init(void);
bool kbd_avail(void);
int  kbd_getc(void);     /* non-blocking; -1 if empty */
int  kbd_peekc(void);    /* non-blocking; next char without consuming */
int  kbd_readc(void);    /* blocking */

#endif
