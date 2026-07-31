#ifndef NOOS_VGA_H
#define NOOS_VGA_H

#include "types.h"

void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 color);
void vga_putc(char c);
void vga_write(const char *s);

#endif
