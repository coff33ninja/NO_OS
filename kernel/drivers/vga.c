#include "vga.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEM     ((volatile u16 *)0xB8000)

static u8 vga_color = 0x0F; /* white on black */
static u8 vga_row = 0;
static u8 vga_col = 0;

static inline u16 vga_entry(char c, u8 color)
{
    return (u16)(u8)c | ((u16)color << 8);
}

void vga_init(void)
{
    vga_clear();
}

void vga_clear(void)
{
    for (u32 i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = vga_entry(' ', 0x00);
    vga_row = 0;
    vga_col = 0;
}

void vga_set_color(u8 color)
{
    vga_color = color;
}

static void vga_scroll(void)
{
    for (u32 i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++)
        VGA_MEM[i] = VGA_MEM[i + VGA_WIDTH];
    for (u32 i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = vga_entry(' ', vga_color);
    vga_row = VGA_HEIGHT - 1;
}

void vga_putc(char c)
{
    if (c == '\n') {
        vga_row++;
        vga_col = 0;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        vga_col = (vga_col + 4) & ~3;
    } else if (c == '\b') {
        if (vga_col > 0)
            vga_col--;
    } else {
        VGA_MEM[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }
    if (vga_row >= VGA_HEIGHT)
        vga_scroll();
}

void vga_write(const char *s)
{
    while (*s)
        vga_putc(*s++);
}
