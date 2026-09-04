#pragma once

#include <stdint.h>
#include <stdbool.h>

// QuranNode targets the ST7796S 3.5" panel in PORTRAIT (320 wide x 480 tall) —
// the reading device is held upright, mushaf-style.
#define CANVAS_WIDTH   320
#define CANVAS_HEIGHT  480
#define CANVAS_BPP     16
#define CANVAS_BUF_SIZE (CANVAS_WIDTH * CANVAS_HEIGHT * 2)  // 307200 bytes

// RGB565 color type
typedef uint16_t color_t;

// RGB565 packing macro. Standard RRRRRGGGGGGBBBBB, then the two bytes swapped
// for the panel's SPI byte order (confirmed on-hardware via the swatch test —
// the previous packing scrambled channels, e.g. cyan rendered brown).
#define RGB565(r, g, b) ((color_t)(                                         \
    ((((((g) & 0x1C) << 3) | ((b) >> 3))) << 8) |                          \
    ((r) & 0xF8) | (((g) >> 5) & 0x07)                                      \
))

// Color palette - grayscale
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_DARK3     RGB565(34, 34, 34)
#define COLOR_DARK2     RGB565(68, 68, 68)
#define COLOR_DARK1     RGB565(102, 102, 102)
#define COLOR_MID       RGB565(136, 136, 136)
#define COLOR_LIGHT1    RGB565(170, 170, 170)
#define COLOR_LIGHT2    RGB565(204, 204, 204)
#define COLOR_LIGHT3    RGB565(230, 230, 230)
#define COLOR_WHITE     RGB565(255, 255, 255)

// Accent colors (legacy - kept for compatibility)
#define COLOR_CYAN      RGB565(0, 200, 255)
#define COLOR_ORANGE    RGB565(255, 140, 0)
#define COLOR_GREEN     RGB565(0, 220, 80)
#define COLOR_RED       RGB565(255, 50, 50)
#define COLOR_BLUE      RGB565(60, 100, 255)
#define COLOR_YELLOW    RGB565(255, 220, 0)
#define COLOR_MAGENTA   RGB565(220, 50, 220)
#define COLOR_TEAL      RGB565(0, 180, 180)

// TE-inspired palette (black bg, orange accent, gray structure)
#define COLOR_OG          RGB565(255, 95, 0)      // TE orange - primary accent
#define COLOR_BROWN       RGB565(120, 120, 120)   // Mid gray - secondary/dimmed text
#define COLOR_GRID        RGB565(50, 50, 50)      // Dark gray - grid lines

// Backward compatibility shades -> colors
#define SHADE_BLACK    COLOR_BLACK
#define SHADE_DARK3    COLOR_DARK3
#define SHADE_DARK2    COLOR_DARK2
#define SHADE_DARK1    COLOR_DARK1
#define SHADE_MID      COLOR_MID
#define SHADE_LIGHT1   COLOR_LIGHT1
#define SHADE_LIGHT2   COLOR_LIGHT2
#define SHADE_LIGHT3   COLOR_LIGHT3
#define SHADE_WHITE    COLOR_WHITE

typedef struct {
    uint16_t *buf;
    uint16_t width;
    uint16_t height;
} Canvas;

bool canvas_init(Canvas *c, uint16_t width, uint16_t height);
void canvas_deinit(Canvas *c);
void canvas_clear(Canvas *c, color_t color);

// Color utilities
color_t color_blend(color_t c1, color_t c2, uint8_t alpha);
color_t color_from_gray(uint8_t gray);

// Primitives
void canvas_pixel(Canvas *c, int x, int y, color_t color);
color_t canvas_get_pixel(const Canvas *c, int x, int y);
void canvas_line(Canvas *c, int x0, int y0, int x1, int y1, color_t color);
void canvas_hline(Canvas *c, int x, int y, int w, color_t color);
void canvas_vline(Canvas *c, int x, int y, int h, color_t color);

// Shapes
void canvas_rect(Canvas *c, int x, int y, int w, int h, color_t color);
void canvas_rect_fill(Canvas *c, int x, int y, int w, int h, color_t color);
void canvas_rect_rounded(Canvas *c, int x, int y, int w, int h, int r, color_t color);
void canvas_rect_rounded_fill(Canvas *c, int x, int y, int w, int h, int r, color_t color);
void canvas_circle(Canvas *c, int cx, int cy, int r, color_t color);
void canvas_circle_fill(Canvas *c, int cx, int cy, int r, color_t color);

// Arc for round display knob indicators
void canvas_arc_thick(Canvas *c, int cx, int cy, int r, int thickness,
                      float start_angle, float end_angle, color_t color);

// Blending
void canvas_pixel_blend(Canvas *c, int x, int y, color_t color, uint8_t alpha);

// Progress / bars
void canvas_progress_bar(Canvas *c, int x, int y, int w, int h,
                         float progress, color_t fg, color_t bg);

// Round display mask - blacks out pixels outside inscribed circle
void canvas_apply_circle_mask(Canvas *c);

// Grid overlay for retro CRT / hardware display aesthetic
void canvas_draw_grid(Canvas *c, int spacing, color_t color);

// QR code rendering.
// `qrcode` is a buffer produced by qrcodegen_encodeText(). Each module is drawn
// as a `scale`x`scale` block with top-left at (ox, oy). Caller is responsible
// for any surrounding quiet zone (draw a `bg` rectangle behind it first).
void canvas_draw_qr(Canvas *c, const uint8_t *qrcode, int ox, int oy, int scale,
                    color_t fg, color_t bg);
