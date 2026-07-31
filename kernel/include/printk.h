#ifndef NOOS_PRINTK_H
#define NOOS_PRINTK_H

#include "format.h"

/* printk: formatted output to VGA + COM1. */
void printk(const char *fmt, ...);

#endif
