#pragma once

#include <stdint.h>
#include "canvas.h"

typedef struct {
    const uint8_t *data;       // Bitmap data
    uint8_t char_width;        // Fixed width per character
    uint8_t char_height;       // Height per character
    uint8_t first_char;        // First ASCII char in set
    uint8_t last_char;         // Last ASCII char in set
    uint8_t spacing;           // Pixels between characters
    uint8_t scale;             // Render scale (1 = native, 2 = 2x, etc.)
} Font;

// Built-in fonts
extern const Font font_tiny;    // 5x7 @ scale 1 = 5x7 effective
extern const Font font_small;   // 5x7 @ scale 2 = 10x14 effective
extern const Font font_medium;  // 5x7 @ scale 3 = 15x21 effective
extern const Font font_large;   // 5x7 @ scale 4 = 20x28 effective

// Drawing
void font_draw_char(Canvas *c, int x, int y, const Font *f, char ch, color_t color);
void font_draw_string(Canvas *c, int x, int y, const Font *f, const char *str, color_t color);
int font_string_width(const Font *f, const char *str);

// Alignment helpers
void font_draw_string_centered(Canvas *c, int y, const Font *f, const char *str, color_t color);
void font_draw_string_right(Canvas *c, int x, int y, const Font *f, const char *str, color_t color);

// Pixel-fit helpers — compute character limits from available pixel width
int font_max_chars(const Font *f, int avail_px);
void font_truncate(char *dst, int dst_size, const char *src, const Font *f, int avail_px);
