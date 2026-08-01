#ifndef NOOS_NOC_OS_H
#define NOOS_NOC_OS_H

#include "types.h"

/* Platform abstraction for the shared NOC compiler/VM.
   Kernel build (kern/noc_os.c): kmalloc/printk/pit/kbd.
   User build   (user/noc_os.c): int 0x80 syscalls. */

void *noc_os_alloc(usize n);
void  noc_os_free(void *p);
void  noc_os_putc(char c);
void  noc_os_puts(const char *s);
u64   noc_os_ticks(void);
void  noc_os_sleep(u64 ms);
int   noc_os_kbd_poll(void); /* next key or -1, consuming */
int   noc_os_kbd_peek(void); /* next key or -1, non-consuming */
int   noc_os_kbd_wait(void); /* blocking */
void  noc_os_exit(int code);

/* M5: model weight RAM budget. Kernel side: direct task state. User side:
   int 0x80 syscalls. */
u64   noc_os_model_budget(u64 kb);
u64   noc_os_model_commit(u64 pages);

#endif
