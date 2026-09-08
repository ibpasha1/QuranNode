// arabic_text.h — on-device Arabic mushaf rendering.
//
// All the hard parts of Arabic (RTL ordering, contextual letter shaping,
// tashkeel positioning) happen OFFLINE in tools/shape_quran.py using HarfBuzz +
// a mushaf font. That tool bakes each ayah into an 8-bit alpha coverage bitmap
// plus per-word bounding boxes, packed into one "glyph pack" file per font size.
//
// On the device there is NO shaping and NO RTL logic: rendering an ayah is a
// tinted alpha blit, and highlighting the word being recited is a rectangle over
// that word's pre-computed box. This keeps the MCU side trivial and fast.
//
// Pack file format (little-endian), see tools/shape_quran.py:
//   header  { magic "QNGP", u16 version, u16 flags, u16 line_h, u16 ascent,
//             u32 n_entries, u32 index_off }
//   index[n]{ u16 surah, u16 ayah, u32 blob_off, u16 w, u16 h, u16 n_words, u16 pad }
//   blob    { u8 alpha[w*h] (row-major, already laid out visually),
//             box[n_words]{ u16 x,y,w,h } }   // word boxes in READING order (word 0 first)
#pragma once

#include "canvas.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Streaming pack: the file is kept OPEN and only the header + index live in RAM.
// Each looked-up ayah's blob is read on demand into a small LRU cache, so a long
// surah's pack never has to fit in memory (the reader only needs a few ayat).
struct HalFile;
#define GP_CACHE_N 4        // ayah blobs kept resident (reader shows prev+cur+next)

typedef struct {
    int      surah, ayah;   // -1 = empty slot
    uint8_t *blob;          // owned: one ayah's alpha[+colidx]+word-boxes
    size_t   cap;
    uint32_t used_at;       // LRU stamp
} GpCacheSlot;

typedef struct {
    struct HalFile *file;   // open pack file (streamed)
    uint16_t version;
    uint16_t flags;
    uint16_t line_h;
    uint16_t ascent;
    uint32_t n_entries;
    uint8_t *index;         // owned: n_entries * 16-byte entries
    GpCacheSlot cache[GP_CACHE_N];
    uint32_t clock;
} GlyphPack;

typedef struct { uint16_t x, y, w, h; } AtWordBox;

typedef struct {
    int surah, ayah;
    const uint8_t *alpha;   // w*h coverage, 0=transparent 255=opaque
    const uint8_t *colidx;  // w*h tajweed palette indices, or NULL (mono pack)
    int w, h;
    int n_words;
    const uint8_t *words;   // raw AtWordBox array (4x u16 each) within data
} AyahGlyphs;

// Load a pack from the SD root (via the HAL). Keeps the file buffer resident.
bool glyphpack_open(GlyphPack *gp, const char *rel_path);
void glyphpack_close(GlyphPack *gp);

// Look up one ayah (reads its blob into the cache — hence non-const). Returns
// false if not present. The returned pointers stay valid until GP_CACHE_N other
// ayat are fetched, so prev+cur+next coexist.
bool glyphpack_get(GlyphPack *gp, int surah, int ayah, AyahGlyphs *out);

// Sequential access over the pack's ayat (in file order = mushaf order).
int  glyphpack_count(const GlyphPack *gp);
bool glyphpack_at(GlyphPack *gp, int i, AyahGlyphs *out);

// Read word box i (reading order) into *out.
bool ayah_word_box(const AyahGlyphs *g, int i, AtWordBox *out);

// Read an ayah's pixel dimensions from the resident index WITHOUT streaming its
// blob — for layout/measurement (e.g. deciding how to scroll a tall segment).
// Returns false if the ayah isn't in the pack.
bool glyphpack_dims(GlyphPack *gp, int surah, int ayah, int *w, int *h);

// Blit an ayah's alpha bitmap into the canvas with its top-left at (x, y),
// tinting the glyph coverage with `color` (alpha-blended over the background).
// If highlight_word >= 0, that word's box is filled with `hl_color` first so the
// recited word appears highlit under the text.
void arabic_draw_ayah(Canvas *c, int x, int y, const AyahGlyphs *g,
                      color_t color, int highlight_word, color_t hl_color);

// Convenience: horizontally center the ayah in the canvas at vertical `y`.
// Returns the x it was drawn at.
int arabic_draw_ayah_centered(Canvas *c, int y, const AyahGlyphs *g,
                              color_t color, int highlight_word, color_t hl_color);

// Like arabic_draw_ayah, but colors each pixel by its tajweed rule using
// `palette[colidx]` (index 0 = `deflt`). Requires g->colidx != NULL.
void arabic_draw_ayah_colored(Canvas *c, int x, int y, const AyahGlyphs *g,
                              const color_t *palette, int n_palette, color_t deflt,
                              int highlight_word, color_t hl_color);

// --- Progressive veil (hifz drill) ----------------------------------------
// The veil is a POST-PASS, not a draw parameter. Threading a mask through the
// blit would need a per-pixel "which word owns this?" test, but word boxes
// overlap and don't tile — that's ~1.3M tests/frame. Instead the caller draws
// the ayah normally, then paints a curtain over the hidden words' boxes: it's
// O(n_hidden) rects and works with the plain AND tajweed blit for free, since
// it never touches the glyph coverage.
typedef enum {
    VEIL_NONE = 0,   // hide nothing
    VEIL_TAIL,       // hide the last pct% of [w0..w1] — keep the opening as the
                     // retrieval cue, hide the ending that must fire from memory
    VEIL_KEEP_FIRST, // keep only the first word of [w0..w1], hide the rest
    VEIL_STRIDE,     // hide every `stride`-th word (a lighter, interleaved veil)
    VEIL_ALL,        // hide all of [w0..w1]
} VeilMode;

// Fill `mask` (n_words bytes; 1 = hidden) for the words in [w0..w1] of an ayah.
// DETERMINISTIC — never random: random hiding makes rep 2 easier than rep 1, is
// unrepeatable across sessions, and turns "shaky" into noise. Words outside
// [w0..w1] are never hidden. `pct` applies to VEIL_TAIL, `stride` to VEIL_STRIDE.
void ayah_veil_mask(uint8_t *mask, int n_words, int w0, int w1,
                    VeilMode mode, int pct, int stride);

typedef struct {
    const uint8_t *mask;   // per-word: 1 = hide (n_words entries)
    int      n_words;
    bool     outline;      // FADE: leave a box outline (word shape is a legit
                           // scaffold). RECALL: false — a blank curtain.
    color_t  curtain;      // fill painted over a hidden word
    color_t  edge;         // outline color, used only when `outline`
} AyahVeil;

// Paint the curtain over an already-drawn ayah's hidden words. `pad = 2` to
// match arabic_draw_ayah's highlight pad, so veil and highlight line up.
void arabic_veil_ayah(Canvas *c, int x, int y, const AyahGlyphs *g,
                      const AyahVeil *v);

// Union bounding box of words [w0..w1] (reading order), for framing a segment.
// Clamps the range to the ayah; returns false if it covers no drawable word.
bool ayah_word_span_box(const AyahGlyphs *g, int w0, int w1, AtWordBox *out);
