#include "prefs.h"
#include "hal.h"
#include "player.h"
#include "plat.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

static const char *TAG = "PREFS";
#define PREFS_MAGIC 0x51505232u   // "QPR2" (v2 added lat/lng)

typedef struct { uint32_t magic; Prefs p; } PrefsBlob;

Prefs g_prefs;

static void set_defaults(void)
{
    g_prefs.rate = 1.0f;
    g_prefs.font_size = FONT_LARGE;
    g_prefs.brightness = 55;   // legible but easy on the battery (backlight is the top draw)
    g_prefs.tajweed = 0;
    g_prefs.volume = 90;     // -> ~1.8x gain (line-out is quiet)
    g_prefs.output = 2;      // 0 headphone, 1 speaker, 2 auto (follow jack-detect) — default
    // Prayer-time location, defaulting to New York; edit in Settings > Location.
    g_prefs.lat = 40.71f;
    g_prefs.lng = -74.01f;
    // Time zone: derive from the platform until the user picks a city/offset.
    g_prefs.tz_std_min = TZ_AUTO;
    g_prefs.dst = 0;
}

void prefs_init(void)
{
    PrefsBlob b;
    memset(&b, 0, sizeof b);
    size_t got = 0;
    // Accept any blob that carries at least the pre-v3 prefix (through lng); the
    // appended tz fields default to "auto" when a shorter v2 blob is loaded.
    size_t need = offsetof(PrefsBlob, p) + offsetof(Prefs, tz_std_min);
    bool loaded = hal_state_load("prefs", &b, sizeof(b), &got) &&
                  b.magic == PREFS_MAGIC && got >= need;
    if (loaded) {
        g_prefs = b.p;
        // Append-only migration: a v2 blob (before tz_std_min/dst) is shorter,
        // so those trailing fields read back as zero — treat that as "auto".
        if (got < sizeof(b)) { g_prefs.tz_std_min = TZ_AUTO; g_prefs.dst = 0; }
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
    if (g_prefs.output > 2) g_prefs.output = 2;   // 0 headphone, 1 speaker, 2 auto
    if (g_prefs.lat < -90.f || g_prefs.lat > 90.f ||
        g_prefs.lng < -180.f || g_prefs.lng > 180.f) {
        g_prefs.lat = 40.71f; g_prefs.lng = -74.01f;
    }
    if (g_prefs.tz_std_min != TZ_AUTO &&
        (g_prefs.tz_std_min < -720 || g_prefs.tz_std_min > 840))
        g_prefs.tz_std_min = TZ_AUTO;
    if (g_prefs.dst > 1) g_prefs.dst = 0;
    prefs_apply();
}

void prefs_save(void)
{
    PrefsBlob b = { .magic = PREFS_MAGIC, .p = g_prefs };
    hal_state_save("prefs", &b, sizeof(b));
}

int prefs_tz_offset_min(void)
{
    if (g_prefs.tz_std_min == TZ_AUTO) return hal_tz_offset_min();
    return g_prefs.tz_std_min + (g_prefs.dst ? 60 : 0);
}

float prefs_volume_gain(void)
{
    if (g_prefs.volume == 0) return 0.0f;
    float f = g_prefs.volume / 100.0f;
    // Cubic taper approximates perceived loudness: 100% -> 2.0x (line-out is
    // quiet), 70% -> 0.69x, 50% -> 0.25x, 20% -> 0.016x. Every slider step
    // is audible, unlike the old linear map where 100->50% was a barely
    // perceptible -6dB (field report: "it doesn't actually lower").
    return 2.0f * f * f * f;
}

void prefs_apply(void)
{
    hal_set_brightness(g_prefs.brightness);
    player_set_rate(&g_player, g_prefs.rate);
    hal_audio_set_volume(prefs_volume_gain());
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

const char *prefs_font_pack(int surah)
{
    static char path[48];
    snprintf(path, sizeof(path), "packs/reader_%s%s/%d.qgp",
             size_tag(), g_prefs.tajweed ? "_tj" : "", surah);
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
