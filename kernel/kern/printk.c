#include "printk.h"
#include "vga.h"
#include "serial.h"

void printk(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    usize n = vsprintk(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (usize i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n')
            serial_putc('\r');
        serial_putc(c);
        vga_putc(c);
    }
}
