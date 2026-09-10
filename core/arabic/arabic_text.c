#include "arabic_text.h"
#include "plat.h"
#include "hal.h"
#include <string.h>
#include <limits.h>

static const char *TAG = "ARABIC";

// Index entry layout (16 bytes, little-endian). Must match tools/shape_quran.py.
#define IDX_STRIDE 16

static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

// Composite sort key for one index entry: surah in the high half, ayah in the
// low half, so ascending numeric order == mushaf order.
static inline uint32_t entry_key(const uint8_t *e) {
    return ((uint32_t)rd_u16(e) << 16) | rd_u16(e + 2);
}

bool glyphpack_open(GlyphPack *gp, const char *rel_path)
{
    memset(gp, 0, sizeof(*gp));
    for (int i = 0; i < GP_CACHE_N; i++) gp->cache[i].surah = -1;

    gp->file = hal_fs_open(rel_path);
    if (!gp->file) { QN_LOGE(TAG, "pack not found: %s", rel_path); return false; }

    uint8_t hdr[20];
    if (hal_fs_pread(gp->file, hdr, 20, 0) != 20 || memcmp(hdr, "QNGP", 4) != 0) {
        QN_LOGE(TAG, "bad pack header: %s", rel_path);
        hal_fs_close(gp->file); gp->file = NULL; return false;
    }
    gp->version   = rd_u16(hdr + 4);
    gp->flags     = rd_u16(hdr + 6);
    gp->line_h    = rd_u16(hdr + 8);
    gp->ascent    = rd_u16(hdr + 10);
    gp->n_entries = rd_u32(hdr + 12);
    uint32_t index_off = rd_u32(hdr + 16);

    // Only the index lives in RAM (n_entries * 16 bytes); blobs stream on demand.
    size_t idx_bytes = (size_t)gp->n_entries * IDX_STRIDE;
    gp->index = malloc(idx_bytes ? idx_bytes : 1);
    if (!gp->index ||
        (size_t)hal_fs_pread(gp->file, gp->index, idx_bytes, index_off) != idx_bytes) {
        QN_LOGE(TAG, "pack index read failed: %s", rel_path);
        free(gp->index); gp->index = NULL;
        hal_fs_close(gp->file); gp->file = NULL; return false;
    }
    // The generator emits ayat in mushaf order, so the index is normally sorted
    // by (surah,ayah) — verify once here so find_entry can binary-search. Any
    // out-of-order entry (unexpected) just drops it back to the linear scan.
    gp->index_sorted = true;
    for (uint32_t i = 1; i < gp->n_entries; i++)
        if (entry_key(gp->index + (size_t)i * IDX_STRIDE) <
            entry_key(gp->index + (size_t)(i - 1) * IDX_STRIDE)) {
            gp->index_sorted = false;
            QN_LOGE(TAG, "pack index not sorted at %u; using linear lookup", (unsigned)i);
            break;
        }

    QN_LOGI(TAG, "pack %s: %u ayat, line_h=%u (streamed)", rel_path,
            (unsigned)gp->n_entries, gp->line_h);
    return true;
}

void glyphpack_close(GlyphPack *gp)
{
    if (gp->file) hal_fs_close(gp->file);
    if (gp->index) free(gp->index);
    for (int i = 0; i < GP_CACHE_N; i++)
        if (gp->cache[i].blob) free(gp->cache[i].blob);
    memset(gp, 0, sizeof(*gp));
}

static const uint8_t *find_entry(const GlyphPack *gp, int surah, int ayah)
{
    uint32_t target = ((uint32_t)(uint16_t)surah << 16) | (uint16_t)ayah;
    if (gp->index_sorted) {   // binary search the mushaf-ordered index
        uint32_t lo = 0, hi = gp->n_entries;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const uint8_t *e = gp->index + (size_t)mid * IDX_STRIDE;
            uint32_t k = entry_key(e);
            if (k == target) return e;
            if (k < target) lo = mid + 1; else hi = mid;
        }
        return NULL;
    }
    for (uint32_t i = 0; i < gp->n_entries; i++) {   // fallback: linear scan
        const uint8_t *e = gp->index + (size_t)i * IDX_STRIDE;
        if (rd_u16(e) == surah && rd_u16(e + 2) == ayah) return e;
    }
    return NULL;
}

bool glyphpack_get(GlyphPack *gp, int surah, int ayah, AyahGlyphs *out)
{
    if (!gp->file || !gp->index) return false;
    const uint8_t *e = find_entry(gp, surah, ayah);
    if (!e) return false;

    uint32_t blob = rd_u32(e + 4);
    int w  = rd_u16(e + 8);
    int h  = rd_u16(e + 10);
    int nw = rd_u16(e + 12);
    int colored = gp->flags & 1;   // per-pixel tajweed color plane present
    size_t planes = (size_t)w * h * (colored ? 2 : 1);
    size_t need   = planes + (size_t)nw * 8;

    // Cache hit? (prev/cur/next stay resident across frames)
    GpCacheSlot *slot = NULL;
    for (int i = 0; i < GP_CACHE_N; i++)
        if (gp->cache[i].surah == surah && gp->cache[i].ayah == ayah) { slot = &gp->cache[i]; break; }

    if (!slot) {
        // Miss: evict the least-recently-used slot and stream this ayah's blob in.
        slot = &gp->cache[0];
        for (int i = 1; i < GP_CACHE_N; i++)
            if (gp->cache[i].used_at < slot->used_at) slot = &gp->cache[i];
        if (slot->cap < need) {
            uint8_t *nb = realloc(slot->blob, need);
            if (!nb) return false;
            slot->blob = nb; slot->cap = need;
        }
        if ((size_t)hal_fs_pread(gp->file, slot->blob, need, blob) != need) {
            QN_LOGE(TAG, "blob read failed %d:%d", surah, ayah);
            slot->surah = -1;
            return false;
        }
        slot->surah = surah; slot->ayah = ayah;
    }
    slot->used_at = ++gp->clock;

    out->surah = surah;
    out->ayah = ayah;
    out->w = w;
    out->h = h;
    out->n_words = nw;
    out->alpha = slot->blob;
    out->colidx = colored ? slot->blob + (size_t)w * h : NULL;
    out->words = slot->blob + planes;
    return true;
}

int glyphpack_count(const GlyphPack *gp)
{
    return gp->file ? (int)gp->n_entries : 0;
}

bool glyphpack_at(GlyphPack *gp, int i, AyahGlyphs *out)
{
    if (!gp->file || i < 0 || (uint32_t)i >= gp->n_entries) return false;
    const uint8_t *e = gp->index + (size_t)i * IDX_STRIDE;
    return glyphpack_get(gp, rd_u16(e), rd_u16(e + 2), out);
}

bool glyphpack_dims(GlyphPack *gp, int surah, int ayah, int *w, int *h)
{
    if (!gp->index) return false;
    const uint8_t *e = find_entry(gp, surah, ayah);
    if (!e) return false;
    if (w) *w = rd_u16(e + 8);
    if (h) *h = rd_u16(e + 10);
    return true;
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

// --- Progressive veil -----------------------------------------------------
void ayah_veil_mask(uint8_t *mask, int n_words, int w0, int w1,
                    VeilMode mode, int pct, int stride)
{
    if (!mask || n_words <= 0) return;
    memset(mask, 0, (size_t)n_words);
    if (mode == VEIL_NONE) return;

    if (w0 < 0) w0 = 0;
    if (w1 >= n_words) w1 = n_words - 1;
    if (w0 > w1) return;
    int count = w1 - w0 + 1;

    switch (mode) {
    case VEIL_TAIL: {
        // Hide the last `pct`% of the span (rounded), keeping the head visible.
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int hidden = (count * pct + 50) / 100;
        if (hidden > count) hidden = count;
        for (int i = w1 - hidden + 1; i <= w1; i++) mask[i] = 1;
        break;
    }
    case VEIL_KEEP_FIRST:
        for (int i = w0 + 1; i <= w1; i++) mask[i] = 1;
        break;
    case VEIL_STRIDE:
        if (stride < 2) stride = 2;
        for (int i = w0; i <= w1; i++)
            if ((i - w0) % stride == stride - 1) mask[i] = 1;
        break;
    case VEIL_ALL:
        for (int i = w0; i <= w1; i++) mask[i] = 1;
        break;
    default:
        break;
    }
}

void arabic_veil_ayah(Canvas *c, int x, int y, const AyahGlyphs *g,
                      const AyahVeil *v)
{
    if (!v || !v->mask) return;
    int n = v->n_words < g->n_words ? v->n_words : g->n_words;
    const int pad = 2;   // align with arabic_draw_ayah's highlight pad
    for (int i = 0; i < n; i++) {
        if (!v->mask[i]) continue;
        AtWordBox b;
        if (!ayah_word_box(g, i, &b)) continue;
        int rx = x + b.x - pad, ry = y + b.y - pad;
        int rw = b.w + 2 * pad, rh = b.h + 2 * pad;
        canvas_rect_rounded_fill(c, rx, ry, rw, rh, 3, v->curtain);
        if (v->outline)   // FADE: leave the word's footprint as a scaffold
            canvas_rect_rounded(c, rx, ry, rw, rh, 3, v->edge);
    }
}

bool ayah_word_span_box(const AyahGlyphs *g, int w0, int w1, AtWordBox *out)
{
    if (w0 < 0) w0 = 0;
    if (w1 >= g->n_words) w1 = g->n_words - 1;
    if (w0 > w1) return false;

    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    bool any = false;
    for (int i = w0; i <= w1; i++) {
        AtWordBox b;
        if (!ayah_word_box(g, i, &b)) continue;
        if (b.x < x0) x0 = b.x;
        if (b.y < y0) y0 = b.y;
        if (b.x + b.w > x1) x1 = b.x + b.w;
        if (b.y + b.h > y1) y1 = b.y + b.h;
        any = true;
    }
    if (!any) return false;
    out->x = (uint16_t)x0;
    out->y = (uint16_t)y0;
    out->w = (uint16_t)(x1 - x0);
    out->h = (uint16_t)(y1 - y0);
    return true;
}
