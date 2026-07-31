#include "types.h"
#include "kbd.h"
#include "printk.h"

usize line_read(char *buf, usize cap)
{
    usize i = 0;
    if (!buf || cap == 0)
        return 0;

    for (;;) {
        int c = kbd_readc();
        if (c == '\n') {
            printk("\n");
            break;
        }
        if (c == '\b') {
            if (i > 0) {
                i--;
                printk("\b \b");
            }
            continue;
        }
        if (i < cap - 1) {
            buf[i++] = (char)c;
            printk("%c", c);
        }
    }
    buf[i] = '\0';
    return i;
}
