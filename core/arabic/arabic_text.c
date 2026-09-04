#include "arabic_text.h"
#include "plat.h"
#include "hal.h"
#include <string.h>

static const char *TAG = "ARABIC";

// Index entry layout (16 bytes, little-endian). Must match tools/shape_quran.py.
#define IDX_STRIDE 16

static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool glyphpack_open(GlyphPack *gp, const char *rel_path)
{
    memset(gp, 0, sizeof(*gp));
    if (!hal_fs_slurp(rel_path, &gp->data, &gp->len)) {
        QN_LOGE(TAG, "pack not found: %s", rel_path);
        return false;
    }
    if (gp->len < 20 || memcmp(gp->data, "QNGP", 4) != 0) {
        QN_LOGE(TAG, "bad pack header: %s", rel_path);
        free(gp->data);
        gp->data = NULL;
        return false;
    }
    gp->version   = rd_u16(gp->data + 4);
    gp->flags     = rd_u16(gp->data + 6);
    gp->line_h    = rd_u16(gp->data + 8);
    gp->ascent    = rd_u16(gp->data + 10);
    gp->n_entries = rd_u32(gp->data + 12);
    uint32_t index_off = rd_u32(gp->data + 16);
    if (index_off + (size_t)gp->n_entries * IDX_STRIDE > gp->len) {
        QN_LOGE(TAG, "pack index out of range: %s", rel_path);
        free(gp->data);
        gp->data = NULL;
        return false;
    }
    gp->index = gp->data + index_off;
    QN_LOGI(TAG, "pack %s: %u ayat, line_h=%u", rel_path,
            (unsigned)gp->n_entries, gp->line_h);
    return true;
}

void glyphpack_close(GlyphPack *gp)
{
    if (gp->data) free(gp->data);
    memset(gp, 0, sizeof(*gp));
}

bool glyphpack_get(const GlyphPack *gp, int surah, int ayah, AyahGlyphs *out)
{
    if (!gp->data) return false;
    // Linear scan — packs are small (a few hundred ayat); binary search later.
    for (uint32_t i = 0; i < gp->n_entries; i++) {
        const uint8_t *e = gp->index + (size_t)i * IDX_STRIDE;
        if (rd_u16(e) == surah && rd_u16(e + 2) == ayah) {
            uint32_t blob = rd_u32(e + 4);
            int w = rd_u16(e + 8);
            int h = rd_u16(e + 10);
            int nw = rd_u16(e + 12);
            int colored = gp->flags & 1;   // per-pixel tajweed color plane present
            size_t planes = (size_t)w * h * (colored ? 2 : 1);
            size_t need = blob + planes + (size_t)nw * 8;
            if (need > gp->len) { QN_LOGE(TAG, "blob OOR %d:%d", surah, ayah); return false; }
            out->surah = surah;
            out->ayah = ayah;
            out->w = w;
            out->h = h;
            out->n_words = nw;
            out->alpha = gp->data + blob;
            out->colidx = colored ? gp->data + blob + (size_t)w * h : NULL;
            out->words = gp->data + blob + planes;
            return true;
        }
    }
    return false;
}

int glyphpack_count(const GlyphPack *gp)
{
    return gp->data ? (int)gp->n_entries : 0;
}

bool glyphpack_at(const GlyphPack *gp, int i, AyahGlyphs *out)
{
    if (!gp->data || i < 0 || (uint32_t)i >= gp->n_entries) return false;
    const uint8_t *e = gp->index + (size_t)i * IDX_STRIDE;
    return glyphpack_get(gp, rd_u16(e), rd_u16(e + 2), out);
}

bool ayah_word_box(const AyahGlyphs *g, int i, AtWordBox *out)
{
    if (i < 0 || i >= g->n_words) return false;
    const uint8_t *p = g->words + (size_t)i * 8;
    out->x = rd_u16(p);
    out->y = rd_u16(p + 2);
    out->w = rd_u16(p + 4);
    out->h = rd_u16(p + 6);
    return true;
}

void arabic_draw_ayah(Canvas *c, int x, int y, const AyahGlyphs *g,
                      color_t color, int highlight_word, color_t hl_color)
{
    // Highlight the recited word first, so the tinted glyphs land on top of it.
    if (highlight_word >= 0 && highlight_word < g->n_words) {
        AtWordBox b;
        if (ayah_word_box(g, highlight_word, &b)) {
            int pad = 2;
            canvas_rect_rounded_fill(c, x + b.x - pad, y + b.y - pad,
                                     b.w + 2 * pad, b.h + 2 * pad, 3, hl_color);
        }
    }

    const uint8_t *a = g->alpha;
    for (int yy = 0; yy < g->h; yy++) {
        int cy = y + yy;
        if (cy < 0 || cy >= c->height) { a += g->w; continue; }
        for (int xx = 0; xx < g->w; xx++) {
            uint8_t cov = a[xx];
            if (cov == 0) continue;
            int cx = x + xx;
            if (cx < 0 || cx >= c->width) continue;
            if (cov == 255) canvas_pixel(c, cx, cy, color);
            else            canvas_pixel_blend(c, cx, cy, color, cov);
        }
        a += g->w;
    }
}

int arabic_draw_ayah_centered(Canvas *c, int y, const AyahGlyphs *g,
                              color_t color, int highlight_word, color_t hl_color)
{
    int x = (c->width - g->w) / 2;
    arabic_draw_ayah(c, x, y, g, color, highlight_word, hl_color);
    return x;
}

void arabic_draw_ayah_colored(Canvas *c, int x, int y, const AyahGlyphs *g,
                              const color_t *palette, int n_palette, color_t deflt,
                              int highlight_word, color_t hl_color)
{
    if (!g->colidx) {   // mono pack — fall back to a flat tint
        arabic_draw_ayah(c, x, y, g, deflt, highlight_word, hl_color);
        return;
    }
    if (highlight_word >= 0 && highlight_word < g->n_words) {
        AtWordBox b;
        if (ayah_word_box(g, highlight_word, &b)) {
            int pad = 2;
            canvas_rect_rounded_fill(c, x + b.x - pad, y + b.y - pad,
                                     b.w + 2 * pad, b.h + 2 * pad, 3, hl_color);
        }
    }

    const uint8_t *a = g->alpha;
    const uint8_t *ci = g->colidx;
    for (int yy = 0; yy < g->h; yy++) {
        int cy = y + yy;
        if (cy < 0 || cy >= c->height) { a += g->w; ci += g->w; continue; }
        for (int xx = 0; xx < g->w; xx++) {
            uint8_t cov = a[xx];
            if (cov == 0) continue;
            int cx = x + xx;
            if (cx < 0 || cx >= c->width) continue;
            uint8_t idx = ci[xx];
            color_t col = (idx > 0 && idx < n_palette) ? palette[idx] : deflt;
            if (cov == 255) canvas_pixel(c, cx, cy, col);
            else            canvas_pixel_blend(c, cx, cy, col, cov);
        }
        a += g->w;
        ci += g->w;
    }
}
