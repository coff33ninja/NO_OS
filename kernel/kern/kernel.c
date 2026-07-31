#include "types.h"
#include "vga.h"
#include "serial.h"

static void serial_hex(u32 v)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9];
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
    serial_write(buf);
}

void kmain(u32 mb_info)
{
    serial_init();
    vga_init();

    vga_set_color(0x1F); /* white on blue */
    vga_write("NO_OS v0.1\n");
    vga_set_color(0x0F);

    serial_write("NO_OS v0.1 booted.\r\n");
    serial_write("multiboot info: 0x");
    serial_hex(mb_info);
    serial_write("\r\n");

    for (;;)
        __asm__ volatile("hlt");
}
