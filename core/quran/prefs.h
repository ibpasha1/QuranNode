// prefs.h — user preferences (persisted via the HAL state store).
//
// Small, durable settings that shape the reading experience: recitation speed,
// Arabic font size, screen brightness, and whether tajweed rule colors are shown.
// prefs_apply() pushes the live ones (brightness -> panel, rate -> player).
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FONT_SMALL = 0, FONT_MEDIUM, FONT_LARGE, FONT_XLARGE, FONT_XXLARGE,
    FONT_SIZE_COUNT
} FontSize;

typedef struct {
    float   rate;        // recitation speed, 0.5 .. 2.0
    uint8_t font_size;   // FontSize
    uint8_t brightness;  // 10 .. 100 (%)
    uint8_t tajweed;     // 0/1 — color the text by tajweed rule
    uint8_t volume;      // 0 .. 100 (%) — software gain
    uint8_t output;      // 0 = headphone, 1 = speaker (amp enable)
    float   lat, lng;    // prayer-time location (degrees, +N/+E)
} Prefs;

extern Prefs g_prefs;

void prefs_init(void);    // load persisted prefs (or defaults) and apply them
void prefs_save(void);    // persist current prefs
void prefs_apply(void);   // push brightness -> HAL, rate -> shared player

// The reader glyph pack path for the current font size, e.g. "packs/reader_md.qgp".
const char *prefs_font_pack(int surah);   // per-surah pack, e.g. "packs/reader_lg/78.qgp"
const char *prefs_font_name(void);       // "Small" ... "Huge"
bool prefs_font_is_large(void);          // XL/XXL: reader drops prev/next context
