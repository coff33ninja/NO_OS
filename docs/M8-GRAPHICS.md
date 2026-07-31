# M3 — Graphics (TempleOS soul)

NO_OS M3 puts a 640x480 16-color VGA framebuffer under NOC control: the
kernel switches the VGA into planar graphics mode, exposes 2D primitives,
a bitmap font, and a sprite bank as NOC builtins, and the acceptance case
is a NOC program animating a moving sprite.

## 1. Display model

- Mode: VGA 640x480, 16 colors (mode 0x12), planar.
- Framebuffer base: `0xA0000`. Four bitplanes of 64 KiB each; one byte per
  horizontal run of 8 pixels, so a row is `640/8 = 80` bytes.
- Pixel `(x, y)`: plane byte offset `y * 80 + x/8`; bit `7 - (x & 7)`.
- A color is 4 bits, one bit per plane (`0..15`, standard VGA palette).
- Mode set is done entirely with VGA registers (Misc, Sequencer, CRTC,
  Graphics Controller, Attribute Controller) because the kernel runs in
  long mode where BIOS INT 10h is unavailable. The mode-set must run with
  interrupts disabled (the Attribute Controller index/data toggle is
  stateful and must not be interleaved with ISR port I/O).
- While graphics mode is active, the VGA text buffer is not displayed and
  `vga_putc` skips writing `0xB8000` (it would land in the graphics plane
  window). `printk` keeps emitting to serial either way, so the boot/REPL
  log remains visible in the test harness.

## 2. Pixel writes

Use the set/reset + bit mask path in Graphics Controller write mode 0:

```
GR 0x08 (bit mask)      = 0x80 >> (x & 7)
GR 0x00 (set/reset)     = color
GR 0x01 (enable set/res) = 0x0F
GR 0x05 (write mode)    = 0x00
VRAM[offset]            = 0xFF   (value ignored; set/reset supplies bits)
```

Fills write full bytes (mask `0xFF`) and apply partial masks only on the
left/right edges, so `FillRect` is ~4 port writes plus one write per byte.

## 3. Kernel API (`drivers/gfx.c`, `kernel/include/gfx.h`)

```c
void gfx_init(void);                  /* idempotent; no-op until first SetMode */
void gfx_setmode(void);               /* switch to 640x480x16 */
void gfx_textmode(void);              /* back to 80x25 text */
bool gfx_active(void);                /* 1 when graphics mode is on */
void gfx_pixel(i64 x, i64 y, u8 color);
void gfx_line(i64 x1, i64 y1, i64 x2, i64 y2, u8 color);   /* Bresenham */
void gfx_rect(i64 x, i64 y, i64 w, i64 h, u8 color);       /* outline */
void gfx_fillrect(i64 x, i64 y, i64 w, i64 h, u8 color);
void gfx_text(i64 x, i64 y, const char *s, u8 color);      /* 5x7 font */
void gfx_sprite(u32 n, i64 x, i64 y, u8 color);            /* sprite bank */
```

All draw functions clip to the 640x480 surface; out-of-range calls are
no-ops, so NOC programs cannot corrupt memory through coordinates.

## 4. Font

- Hand-crafted 5x7 bitmap font, ASCII 32..126, 8 bytes per glyph
  (7 used rows; pixels in bits 7..3, bit 7 = leftmost column).
- Unknown characters render as blank.

## 5. Sprite bank

- A small C-side bank of 16x16 pixel-art sprites (`gfx_sprites[]`), each a
  16-row bitmap of 2 bits-per-pixel (4 values: transparent / color /
  darker / lighter). `gfx_sprite(n, x, y, color)` draws sprite `n` tinted
  with `color`.
- Sprites are data in C; NOC programs move and draw them via `Sprite()`.
  This satisfies "sprites drawable from NOC source" for M3 (NOC arrays are
  a later milestone).

## 6. NOC builtins

Added to the registry in `kernel/noc/vm.c`:

| Signature | Effect |
|---|---|
| `U0 SetMode()`  | Enter 640x480x16 graphics mode |
| `U0 TextMode()` | Return to 80x25 text mode |
| `U0 Pixel(I64 x, I64 y, I64 color)`       | Set one pixel |
| `U0 Line(I64 x1, I64 y1, I64 x2, I64 y2, I64 color)` | Bresenham line |
| `U0 Rect(I64 x, I64 y, I64 w, I64 h, I64 color)`     | Outlined rect |
| `U0 FillRect(I64 x, I64 y, I64 w, I64 h, I64 color)` | Filled rect |
| `U0 Text(I64 x, I64 y, Str s, I64 color)` | Draw a string |
| `U0 Sprite(I64 n, I64 x, I64 y, I64 color)` | Draw sprite `n` |

## 7. Acceptance

Boot to the REPL, then run this NOC program from the harness:

```
SetMode();
for (I64 x = 0; x < 600; x += 40) {
    FillRect(0, 0, 640, 480, 0);
    Sprite(0, x, 200, 14);
    Text(8, 8, "NO_OS moving sprite", 7);
    Sleep(16);
}
Sprite(0, 560, 200, 14);
Print("gfx-done\n");
```

The harness waits for `gfx-done` on serial, takes a QEMU monitor
`screendump`, and asserts that the sprite color (14 = light yellow) appears
at the expected final position (sprite 0 around `x=560..575, y=200..215`)
and that it does not appear at the start position (`x=0..15`) — proving the
sprite actually moved.

## 8. Non-goals (M3)

- No VBE/linear framebuffer, no 256-color mode, no double buffering.
- No NOC-side arrays/structs yet (sprites are C-side data).
- No multi-line `.noc` files yet (REPL lines only; a program is one line).
- No per-pixel readback from NOC.
