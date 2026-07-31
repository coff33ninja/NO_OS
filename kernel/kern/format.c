#include "format.h"

usize floor_log2(usize v)
{
    usize r = 0;
    while (v > 1) {
        v >>= 1;
        r++;
    }
    return r;
}

usize upow(usize base, usize exp)
{
    usize r = 1;
    while (exp--) {
        if (base && r > (usize)-1 / base)
            return (usize)-1;
        r *= base;
    }
    return r;
}

usize aformat(char *buf, usize n, usize base, usize v, bool upper)
{
    static const char ldig[] = "0123456789abcdef";
    static const char udig[] = "0123456789ABCDEF";
    const char *dig = upper ? udig : ldig;
    char tmp[65];
    usize i = 0;

    if (base < 2 || base > 36)
        base = 10;

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v && i < sizeof(tmp)) {
            tmp[i++] = dig[v % base];
            v /= base;
        }
    }

    usize w = 0;
    while (i > 0 && w + 1 < n) {
        buf[w++] = tmp[--i];
    }
    if (n > 0)
        buf[w] = '\0';
    return w;
}

usize vsprintk(char *buf, usize cap, const char *fmt, va_list ap)
{
    usize w = 0;

    while (*fmt && (cap == 0 || w + 1 < cap)) {
        char c = *fmt++;

        if (c != '%') {
            buf[w++] = c;
            continue;
        }

        c = *fmt++;
        int len = 0;
        if (c == 'l') {
            len = 1;
            c = *fmt++;
            if (c == 'l') {
                len = 2;
                c = *fmt++;
            }
        }
        switch (c) {
        case '%':
            buf[w++] = '%';
            break;
        case 'c':
            buf[w++] = (char)va_arg(ap, int);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s && (cap == 0 || w + 1 < cap))
                buf[w++] = *s++;
            break;
        }
        case 'd': {
            i64 v = (len == 2) ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            char tmp[24];
            usize i = 0;
            bool neg = v < 0;
            u64 u = neg ? (u64)(-v) : (u64)v;
            if (u == 0) {
                tmp[i++] = '0';
            } else {
                while (u && i < sizeof(tmp)) {
                    tmp[i++] = (char)('0' + u % 10);
                    u /= 10;
                }
            }
            if (neg && (cap == 0 || w + 1 < cap))
                buf[w++] = '-';
            while (i > 0 && (cap == 0 || w + 1 < cap))
                buf[w++] = tmp[--i];
            break;
        }
        case 'u': {
            u64 v = (len == 2) ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            char tmp[24];
            usize i = 0;
            if (v == 0) {
                tmp[i++] = '0';
            } else {
                while (v && i < sizeof(tmp)) {
                    tmp[i++] = (char)('0' + v % 10);
                    v /= 10;
                }
            }
            while (i > 0 && (cap == 0 || w + 1 < cap))
                buf[w++] = tmp[--i];
            break;
        }
        case 'x': {
            u64 v = (len == 2) ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            usize n = aformat(buf + w, cap > w ? cap - w : 0, 16, v, false);
            w += n;
            break;
        }
        case 'p': {
            u64 v = (u64)va_arg(ap, void *);
            buf[w++] = '0';
            if (cap == 0 || w + 1 < cap)
                buf[w++] = 'x';
            usize n = aformat(buf + w, cap > w ? cap - w : 0, 16, v, false);
            w += n;
            break;
        }
        case '\0':
            fmt--;
            break;
        default:
            buf[w++] = '%';
            if (cap == 0 || w + 1 < cap)
                buf[w++] = c;
            break;
        }
    }

    if (cap > 0)
        buf[w] = '\0';
    return w;
}

usize sprintk(char *buf, usize cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    usize n = vsprintk(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
