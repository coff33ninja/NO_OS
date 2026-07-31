#ifndef NOOS_FORMAT_H
#define NOOS_FORMAT_H

#include "types.h"
#include <stdarg.h>

usize floor_log2(usize v);
usize upow(usize base, usize exp);

/* Render v in the given base (2-36) into buf; returns length. */
usize aformat(char *buf, usize n, usize base, usize v, bool upper);

/* vsprintk family: bounded, always NUL-terminated when cap > 0. */
usize vsprintk(char *buf, usize cap, const char *fmt, va_list ap);
usize sprintk(char *buf, usize cap, const char *fmt, ...);

#endif
