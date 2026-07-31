#include "noc_os.h"
#include "heap.h"
#include "pit.h"
#include "kbd.h"
#include "serial.h"
#include "vga.h"

void *noc_os_alloc(usize n)
{
    return kmalloc(n);
}

void noc_os_free(void *p)
{
    kfree(p);
}

void noc_os_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
    vga_putc(c);
}

void noc_os_puts(const char *s)
{
    while (*s)
        noc_os_putc(*s++);
}

u64 noc_os_ticks(void)
{
    return (u64)pit_ticks();
}

void noc_os_sleep(u64 ms)
{
    pit_sleep((usize)ms);
}

int noc_os_kbd_poll(void)
{
    return kbd_getc();
}

int noc_os_kbd_peek(void)
{
    return kbd_peekc();
}

int noc_os_kbd_wait(void)
{
    return kbd_readc();
}

void noc_os_exit(int code)
{
    (void)code;
    for (;;)
        __asm__ volatile("hlt");
}
