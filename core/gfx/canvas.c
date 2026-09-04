#include "canvas.h"
#include "qrcodegen.h"
#include "plat.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "CANVAS";

// The ESP32 build overrides QN_CANVAS_ALLOC to place the framebuffer in PSRAM;
// the simulator (and any plain-C host) uses the standard allocator.
#ifndef QN_CANVAS_ALLOC
#define QN_CANVAS_ALLOC(sz) malloc(sz)
#define QN_CANVAS_FREE(p)   free(p)
#endif

bool canvas_init(Canvas *c, uint16_t width, uint16_t height)
{
    c->width = width;
    c->height = height;
    size_t buf_size = width * height * sizeof(uint16_t);
    c->buf = QN_CANVAS_ALLOC(buf_size);
    if (!c->buf) {
        QN_LOGE(TAG, "Failed to allocate canvas buffer (%u bytes)", (unsigned)buf_size);
        return false;
    }
    memset(c->buf, 0, buf_size);
    QN_LOGI(TAG, "Canvas allocated: %dx%d, %u bytes", width, height, (unsigned)buf_size);
    return true;
}

void canvas_deinit(Canvas *c)
{
    if (c->buf) {
        QN_CANVAS_FREE(c->buf);
        c->buf = NULL;
    }
}

void canvas_clear(Canvas *c, color_t color)
{
    if (color == 0) {
        memset(c->buf, 0, c->width * c->height * sizeof(uint16_t));
    } else {
        for (int i = 0; i < c->width * c->height; i++) {
            c->buf[i] = color;
        }
    }
}

color_t color_blend(color_t c1, color_t c2, uint8_t alpha)
{
    // Byte-swap to standard 5-6-5 layout for channel extraction
    uint16_t p1 = (c1 << 8) | (c1 >> 8);
    uint16_t p2 = (c2 << 8) | (c2 >> 8);

    uint8_t r1 = (p1 >> 11) & 0x1F;
    uint8_t g1 = (p1 >> 5) & 0x3F;
    uint8_t b1 = p1 & 0x1F;

    uint8_t r2 = (p2 >> 11) & 0x1F;
    uint8_t g2 = (p2 >> 5) & 0x3F;
    uint8_t b2 = p2 & 0x1F;

    uint8_t r = (r1 * (255 - alpha) + r2 * alpha) / 255;
    uint8_t g = (g1 * (255 - alpha) + g2 * alpha) / 255;
    uint8_t b = (b1 * (255 - alpha) + b2 * alpha) / 255;

    uint16_t result = (r << 11) | (g << 5) | b;
    return (color_t)((result << 8) | (result >> 8));
}

color_t color_from_gray(uint8_t gray)
{
    return RGB565(gray, gray, gray);
}

void canvas_pixel(Canvas *c, int x, int y, color_t color)
{
    if (x < 0 || x >= c->width || y < 0 || y >= c->height) return;
    c->buf[y * c->width + x] = color;
}

color_t canvas_get_pixel(const Canvas *c, int x, int y)
{
    if (x < 0 || x >= c->width || y < 0 || y >= c->height) return 0;
    return c->buf[y * c->width + x];
}

void canvas_hline(Canvas *c, int x, int y, int w, color_t color)
{
    if (y < 0 || y >= c->height) return;
    int x0 = x < 0 ? 0 : x;
    int x1 = (x + w) > c->width ? c->width : (x + w);
    for (int i = x0; i < x1; i++) {
        c->buf[y * c->width + i] = color;
    }
}

void canvas_vline(Canvas *c, int x, int y, int h, color_t color)
{
    if (x < 0 || x >= c->width) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = (y + h) > c->height ? c->height : (y + h);
    for (int i = y0; i < y1; i++) {
        c->buf[i * c->width + x] = color;
    }
}

void canvas_line(Canvas *c, int x0, int y0, int x1, int y1, color_t color)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        canvas_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void canvas_rect(Canvas *c, int x, int y, int w, int h, color_t color)
{
    canvas_hline(c, x, y, w, color);
    canvas_hline(c, x, y + h - 1, w, color);
    canvas_vline(c, x, y, h, color);
    canvas_vline(c, x + w - 1, y, h, color);
}

void canvas_rect_fill(Canvas *c, int x, int y, int w, int h, color_t color)
{
    for (int j = 0; j < h; j++) {
        canvas_hline(c, x, y + j, w, color);
    }
}

void canvas_rect_rounded(Canvas *c, int x, int y, int w, int h, int r, color_t color)
{
    canvas_hline(c, x + r, y, w - 2 * r, color);
    canvas_hline(c, x + r, y + h - 1, w - 2 * r, color);
    canvas_vline(c, x, y + r, h - 2 * r, color);
    canvas_vline(c, x + w - 1, y + r, h - 2 * r, color);

    int cx, cy, d;
    cx = r; cy = 0; d = 1 - r;
    while (cx >= cy) {
        canvas_pixel(c, x + r - cx, y + r - cy, color);
        canvas_pixel(c, x + r - cy, y + r - cx, color);
        canvas_pixel(c, x + w - 1 - r + cx, y + r - cy, color);
        canvas_pixel(c, x + w - 1 - r + cy, y + r - cx, color);
        canvas_pixel(c, x + r - cx, y + h - 1 - r + cy, color);
        canvas_pixel(c, x + r - cy, y + h - 1 - r + cx, color);
        canvas_pixel(c, x + w - 1 - r + cx, y + h - 1 - r + cy, color);
        canvas_pixel(c, x + w - 1 - r + cy, y + h - 1 - r + cx, color);
        cy++;
        if (d < 0) {
            d += 2 * cy + 1;
        } else {
            cx--;
            d += 2 * (cy - cx) + 1;
        }
    }
}

void canvas_rect_rounded_fill(Canvas *c, int x, int y, int w, int h, int r, color_t color)
{
    canvas_rect_fill(c, x + r, y, w - 2 * r, h, color);
    canvas_rect_fill(c, x, y + r, r, h - 2 * r, color);
    canvas_rect_fill(c, x + w - r, y + r, r, h - 2 * r, color);

    int cx_v, cy_v, d;
    cx_v = r; cy_v = 0; d = 1 - r;
    while (cx_v >= cy_v) {
        canvas_hline(c, x + r - cx_v, y + r - cy_v, w - 2 * r + 2 * cx_v, color);
        canvas_hline(c, x + r - cy_v, y + r - cx_v, w - 2 * r + 2 * cy_v, color);
        canvas_hline(c, x + r - cx_v, y + h - 1 - r + cy_v, w - 2 * r + 2 * cx_v, color);
        canvas_hline(c, x + r - cy_v, y + h - 1 - r + cx_v, w - 2 * r + 2 * cy_v, color);
        cy_v++;
        if (d < 0) {
            d += 2 * cy_v + 1;
        } else {
            cx_v--;
            d += 2 * (cy_v - cx_v) + 1;
        }
    }
}

void canvas_circle(Canvas *c, int cx, int cy, int r, color_t color)
{
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        canvas_pixel(c, cx + x, cy + y, color);
        canvas_pixel(c, cx - x, cy + y, color);
        canvas_pixel(c, cx + x, cy - y, color);
        canvas_pixel(c, cx - x, cy - y, color);
        canvas_pixel(c, cx + y, cy + x, color);
        canvas_pixel(c, cx - y, cy + x, color);
        canvas_pixel(c, cx + y, cy - x, color);
        canvas_pixel(c, cx - y, cy - x, color);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void canvas_circle_fill(Canvas *c, int cx, int cy, int r, color_t color)
{
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        canvas_hline(c, cx - x, cy + y, 2 * x + 1, color);
        canvas_hline(c, cx - x, cy - y, 2 * x + 1, color);
        canvas_hline(c, cx - y, cy + x, 2 * y + 1, color);
        canvas_hline(c, cx - y, cy - x, 2 * y + 1, color);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

void canvas_arc_thick(Canvas *c, int cx, int cy, int r, int thickness,
                      float start_angle, float end_angle, color_t color)
{
    // Draw a thick arc from start_angle to end_angle (radians)
    // Iterate through angles and for each, fill from r-thickness to r
    float step = 1.0f / (float)r;  // ~1 pixel per step
    for (float angle = start_angle; angle <= end_angle; angle += step) {
        float cos_a = cosf(angle);
        float sin_a = sinf(angle);
        for (int t = 0; t < thickness; t++) {
            int ri = r - t;
            int px = cx + (int)(cos_a * ri);
            int py = cy + (int)(sin_a * ri);
            canvas_pixel(c, px, py, color);
        }
    }
}

void canvas_pixel_blend(Canvas *c, int x, int y, color_t color, uint8_t alpha)
{
    if (x < 0 || x >= c->width || y < 0 || y >= c->height) return;
    color_t existing = canvas_get_pixel(c, x, y);
    color_t blended = color_blend(existing, color, alpha);
    canvas_pixel(c, x, y, blended);
}

void canvas_progress_bar(Canvas *c, int x, int y, int w, int h,
                         float progress, color_t fg, color_t bg)
{
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    int fill_w = (int)(w * progress);
    canvas_rect_fill(c, x, y, w, h, bg);
    if (fill_w > 0) {
        canvas_rect_fill(c, x, y, fill_w, h, fg);
    }
}

void canvas_draw_grid(Canvas *c, int spacing, color_t color)
{
    for (int x = spacing; x < c->width; x += spacing) {
        canvas_vline(c, x, 0, c->height, color);
    }
    for (int y = spacing; y < c->height; y += spacing) {
        canvas_hline(c, 0, y, c->width, color);
    }
}

void canvas_draw_qr(Canvas *c, const uint8_t *qrcode, int ox, int oy, int scale,
                    color_t fg, color_t bg)
{
    if (!qrcode || scale < 1) return;

    int size = qrcodegen_getSize(qrcode);
    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            color_t col = qrcodegen_getModule(qrcode, mx, my) ? fg : bg;
            canvas_rect_fill(c, ox + mx * scale, oy + my * scale, scale, scale, col);
        }
    }
}

void canvas_apply_circle_mask(Canvas *c)
{
    int cx = c->width / 2;
    int cy = c->height / 2;
    int r = (c->width < c->height ? c->width : c->height) / 2;
    int r_sq = r * r;

    for (int y = 0; y < c->height; y++) {
        int dy = y - cy;
        int dy_sq = dy * dy;
        for (int x = 0; x < c->width; x++) {
            int dx = x - cx;
            if (dx * dx + dy_sq > r_sq) {
                c->buf[y * c->width + x] = 0;
            }
        }
    }
}
