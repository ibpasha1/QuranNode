#include "prefs.h"
#include "hal.h"
#include "player.h"
#include "plat.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "PREFS";
#define PREFS_MAGIC 0x51505231u   // "QPR1"

typedef struct { uint32_t magic; Prefs p; } PrefsBlob;

Prefs g_prefs;

static void set_defaults(void)
{
    g_prefs.rate = 1.0f;
    g_prefs.font_size = FONT_LARGE;
    g_prefs.brightness = 90;
    g_prefs.tajweed = 0;
    g_prefs.volume = 90;     // -> ~1.8x gain (line-out is quiet)
    g_prefs.output = 0;      // headphone
}

void prefs_init(void)
{
    PrefsBlob b;
    size_t got = 0;
    if (hal_state_load("prefs", &b, sizeof(b), &got) &&
        got == sizeof(b) && b.magic == PREFS_MAGIC) {
        g_prefs = b.p;
        QN_LOGI(TAG, "loaded: rate=%.2f font=%d bright=%d tajweed=%d",
                g_prefs.rate, g_prefs.font_size, g_prefs.brightness, g_prefs.tajweed);
    } else {
        set_defaults();
    }
    // Clamp anything out of range (guards against stale/garbage state).
    if (g_prefs.rate < 0.5f || g_prefs.rate > 2.0f) g_prefs.rate = 1.0f;
    if (g_prefs.font_size >= FONT_SIZE_COUNT) g_prefs.font_size = FONT_LARGE;
    if (g_prefs.brightness < 10) g_prefs.brightness = 10;
    if (g_prefs.brightness > 100) g_prefs.brightness = 100;
    if (g_prefs.volume > 100) g_prefs.volume = 100;
    if (g_prefs.output > 1) g_prefs.output = 0;
    prefs_apply();
}

void prefs_save(void)
{
    PrefsBlob b = { .magic = PREFS_MAGIC, .p = g_prefs };
    hal_state_save("prefs", &b, sizeof(b));
}

void prefs_apply(void)
{
    hal_set_brightness(g_prefs.brightness);
    player_set_rate(&g_player, g_prefs.rate);
    hal_audio_set_volume(g_prefs.volume / 100.0f * 2.0f);   // 100% -> 2.0x, 90% -> 1.8x
    hal_audio_set_output(g_prefs.output);
}

static const char *size_tag(void)
{
    switch (g_prefs.font_size) {
    case FONT_SMALL:   return "sm";
    case FONT_MEDIUM:  return "md";
    case FONT_LARGE:   return "lg";
    case FONT_XLARGE:  return "xl";
    default:           return "xxl";
    }
}

const char *prefs_font_pack(void)
{
    static char path[48];
    snprintf(path, sizeof(path), "packs/reader_%s%s.qgp",
             size_tag(), g_prefs.tajweed ? "_tj" : "");
    return path;
}

const char *prefs_font_name(void)
{
    switch (g_prefs.font_size) {
    case FONT_SMALL:   return "Small";
    case FONT_MEDIUM:  return "Medium";
    case FONT_LARGE:   return "Large";
    case FONT_XLARGE:  return "Extra Large";
    default:           return "Huge";
    }
}

// True for sizes where the focus layout drops the prev/next context ayat.
bool prefs_font_is_large(void) { return g_prefs.font_size >= FONT_XLARGE; }
