// scene_settings.c — preferences editor.
//
// Recitation speed, volume, output routing, Arabic font size, screen brightness,
// tajweed colors, and an on-device firmware update. Changes apply live and persist
// on exit. Reached from Home via the menu, or Settings in nav.
#include "scene.h"
#include "prefs.h"
#include "player.h"
#include "hal.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { S_SPEED, S_VOLUME, S_OUTPUT, S_FONT, S_BRIGHT, S_TAJWEED, S_UPDATE, S_COUNT };

static int  s_sel;
static bool s_ota;   // firmware-update overlay active (Wi-Fi + upload server up)

static void adjust(int dir)
{
    switch (s_sel) {
    case S_SPEED: {
        float r = g_prefs.rate + dir * 0.05f;
        if (r < 0.5f) r = 0.5f;
        if (r > 2.0f) r = 2.0f;
        g_prefs.rate = r;
        player_set_rate(&g_player, r);
        break;
    }
    case S_VOLUME: {
        int v = (int)g_prefs.volume + dir * 5;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        g_prefs.volume = (uint8_t)v;
        hal_audio_set_volume(prefs_volume_gain());
        break;
    }
    case S_OUTPUT:
        g_prefs.output = dir > 0 ? 1 : (dir < 0 ? 0 : !g_prefs.output);
        hal_audio_set_output(g_prefs.output);
        break;
    case S_FONT: {
        int f = (int)g_prefs.font_size + dir;
        if (f < 0) f = 0;
        if (f >= FONT_SIZE_COUNT) f = FONT_SIZE_COUNT - 1;
        g_prefs.font_size = (uint8_t)f;   // reader reloads its pack on next render
        break;
    }
    case S_BRIGHT: {
        int b = (int)g_prefs.brightness + dir * 10;
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        g_prefs.brightness = (uint8_t)b;
        hal_set_brightness(g_prefs.brightness);
        break;
    }
    case S_TAJWEED:
        g_prefs.tajweed = dir > 0 ? 1 : (dir < 0 ? 0 : !g_prefs.tajweed);
        break;
    }
}

static void field(int i, char *label, char *value, int n)
{
    switch (i) {
    case S_SPEED:   snprintf(label, n, "Recitation speed"); snprintf(value, n, "%.2fx", g_prefs.rate); break;
    case S_VOLUME:  snprintf(label, n, "Volume");           snprintf(value, n, "%d%%", g_prefs.volume); break;
    case S_OUTPUT:  snprintf(label, n, "Output");           snprintf(value, n, "%s", g_prefs.output ? "Speaker" : "Headphone"); break;
    case S_FONT:    snprintf(label, n, "Font size");        snprintf(value, n, "%s", prefs_font_name()); break;
    case S_BRIGHT:  snprintf(label, n, "Brightness");       snprintf(value, n, "%d%%", g_prefs.brightness); break;
    case S_TAJWEED: snprintf(label, n, "Tajweed colors");   snprintf(value, n, "%s", g_prefs.tajweed ? "On" : "Off"); break;
    case S_UPDATE:  snprintf(label, n, "Update firmware");  snprintf(value, n, "%s", "Wi-Fi >"); break;
    }
}

// ---------------------------------------------------------------------------
// Animated firmware-update overlay.
//
// The update HAL only hands us a status *string*, so we infer the phase from it
// and drive every pixel off plat_millis() — the scene already redraws at 30fps,
// so a full repaint each frame gives smooth motion with no per-frame state.
// A gold comet orbits a ring, a phase glyph pulses at its centre, an
// indeterminate "scanner" bar sweeps, and the status ellipsis breathes.
// ---------------------------------------------------------------------------
typedef enum { OTA_INIT, OTA_CONNECT, OTA_DOWNLOAD, OTA_DONE, OTA_FAIL } OtaPhase;

static OtaPhase ota_phase(const char *s)
{
    if (!s) return OTA_INIT;
    if (strstr(s, "fail") || strstr(s, "Fail"))       return OTA_FAIL;   // "Wi-Fi failed" too
    if (strstr(s, "Updated") || strstr(s, "eboot"))   return OTA_DONE;
    if (strstr(s, "ownload"))                          return OTA_DOWNLOAD;
    if (strstr(s, "onnect") || strstr(s, "Wi-Fi") || strstr(s, "tart")) return OTA_CONNECT;
    return OTA_INIT;
}

// Sonar: faint rings that grow out of the centre and fade — a sense of "working".
static void ota_sonar(Canvas *c, int cx, int cy, uint32_t t, color_t col)
{
    for (int k = 0; k < 3; k++) {
        float f  = fmodf(t / 1500.0f + k / 3.0f, 1.0f);   // 0..1 expansion
        int   rr = (int)(50 + f * 92);
        uint8_t a = (uint8_t)((1.0f - f) * 85);
        canvas_circle(c, cx, cy, rr, color_blend(THEME_BG, col, a));
    }
}

// A bright arc whose tail fades out — the orbiting "comet" that reads as spin.
static void ota_comet(Canvas *c, int cx, int cy, int r, int th, float head, float len, color_t col)
{
    const int steps = 22;
    for (int i = 0; i < steps; i++) {
        float f = (float)i / (steps - 1);                 // 0 tail .. 1 head
        float a = head - len * (1.0f - f);
        uint8_t alpha = (uint8_t)(25 + f * 230);
        canvas_arc_thick(c, cx, cy, r, th, a, a + len / steps + 0.03f,
                         color_blend(THEME_BG, col, alpha));
    }
}

// Wi-Fi glyph: a dot with arcs above that light up in sequence (connecting).
static void ota_glyph_wifi(Canvas *c, int cx, int cy, uint32_t t)
{
    int dx = cx, dy = cy + 18;
    int lit = (t / 350) % 4;                              // 0..3 rings lit
    for (int k = 0; k < 3; k++) {
        color_t col = (k < lit) ? THEME_ACCENT : color_blend(THEME_BG, THEME_GRID, 230);
        canvas_arc_thick(c, dx, dy, 12 + k * 11, 3, (float)(M_PI * 1.25), (float)(M_PI * 1.75), col);
    }
    canvas_circle_fill(c, dx, dy, 3, THEME_ACCENT);
}

// Download glyph: arrow dropping into a tray, with particles falling down the shaft.
static void ota_glyph_dl(Canvas *c, int cx, int cy, uint32_t t)
{
    int top = cy - 22, trayW = 34, trayY = cy + 20;
    // inbox tray
    canvas_hline(c, cx - trayW / 2, trayY, trayW, THEME_LABEL);
    canvas_vline(c, cx - trayW / 2, trayY - 6, 6, THEME_LABEL);
    canvas_vline(c, cx + trayW / 2, trayY - 6, 6, THEME_LABEL);
    // shaft + arrowhead
    canvas_rect_fill(c, cx - 1, top, 3, 30, THEME_ACCENT);
    for (int i = 0; i <= 10; i++) {
        canvas_pixel(c, cx - 10 + i, top + 20 + i, THEME_ACCENT);
        canvas_pixel(c, cx - 10 + i, top + 21 + i, THEME_ACCENT);
        canvas_pixel(c, cx + 10 - i, top + 20 + i, THEME_ACCENT);
        canvas_pixel(c, cx + 10 - i, top + 21 + i, THEME_ACCENT);
    }
    // particles streaming down the shaft
    for (int k = 0; k < 3; k++) {
        float f = fmodf(t / 500.0f + k / 3.0f, 1.0f);
        int py = top - 8 + (int)(f * 26);
        canvas_circle_fill(c, cx, py, 2, color_blend(THEME_BG, THEME_TITLE, (uint8_t)((1.0f - f) * 255)));
    }
}

static void ota_glyph_check(Canvas *c, int cx, int cy, color_t col)
{
    for (int s = 0; s < 3; s++) {
        canvas_line(c, cx - 16, cy + s, cx - 4, cy + 12 + s, col);
        canvas_line(c, cx - 4, cy + 12 + s, cx + 18, cy - 10 + s, col);
    }
}

static void ota_glyph_x(Canvas *c, int cx, int cy, color_t col)
{
    for (int s = 0; s < 3; s++) {
        canvas_line(c, cx - 14, cy - 14 + s, cx + 14, cy + 14 + s, col);
        canvas_line(c, cx + 14, cy - 14 + s, cx - 14, cy + 14 + s, col);
    }
}

// Indeterminate scanner bar: a soft-edged highlight bounces across the track.
static void ota_scanner(Canvas *c, int x, int y, int w, int h, uint32_t t, color_t col)
{
    canvas_rect_rounded_fill(c, x, y, w, h, h / 2, THEME_PANEL);
    float p   = fmodf(t / 1400.0f, 1.0f);
    float tri = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;   // ping-pong 0..1..0
    int   seg = 54;
    int   hx  = x + (int)(tri * (w - seg));
    for (int i = 0; i < seg; i++) {
        uint8_t a = (uint8_t)(sinf((float)i / seg * (float)M_PI) * 255);  // bright middle
        for (int yy = 0; yy < h; yy++)
            canvas_pixel(c, hx + i, y + yy, color_blend(THEME_PANEL, col, a));
    }
}

// Full-screen overlay while an over-the-air update is armed.
static void render_ota(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "Firmware Update", THEME_TITLE, NULL, THEME_DIM);

    const char *status = hal_ota_status();
    OtaPhase    ph     = ota_phase(status);
    uint32_t    t      = plat_millis();
    int cx = c->width / 2, cy = 172, R = 54, TH = 6;
    bool active = (ph == OTA_INIT || ph == OTA_CONNECT || ph == OTA_DOWNLOAD);

    color_t accent = THEME_ACCENT;
    if (ph == OTA_DONE)  accent = THEME_ACTIVE;
    else if (ph == OTA_FAIL) accent = COLOR_RED;

    // --- centre badge: sonar + ring + orbiting comet + phase glyph ---
    if (active) ota_sonar(c, cx, cy, t, accent);
    canvas_arc_thick(c, cx, cy, R, TH, 0, (float)(2 * M_PI), color_blend(THEME_BG, THEME_GRID, 230));
    if (active)
        ota_comet(c, cx, cy, R, TH, fmodf(t / 260.0f, (float)(2 * M_PI)), 1.6f, accent);
    else
        canvas_arc_thick(c, cx, cy, R, TH, 0, (float)(2 * M_PI), accent);

    switch (ph) {
    case OTA_INIT:
    case OTA_CONNECT:  ota_glyph_wifi(c, cx, cy, t); break;
    case OTA_DOWNLOAD: ota_glyph_dl(c, cx, cy, t);   break;
    case OTA_DONE:     ota_glyph_check(c, cx, cy, accent); break;
    case OTA_FAIL:     ota_glyph_x(c, cx, cy, accent);     break;
    }

    // --- status line with a breathing ellipsis while working ---
    char base[64];
    snprintf(base, sizeof base, "%s", status ? status : "Starting");
    for (int i = (int)strlen(base) - 1; i >= 0 && (base[i] == '.' || base[i] == ' '); i--) base[i] = 0;
    char line[80];
    if (active) snprintf(line, sizeof line, "%s%.*s", base, (int)((t / 400) % 4), "...");
    else        snprintf(line, sizeof line, "%s", base);
    font_draw_string_centered(c, 262, &font_small, line, ph == OTA_FAIL ? COLOR_RED : THEME_TITLE);

    if (active)
        ota_scanner(c, 60, 300, c->width - 120, 8, t, accent);

    // Fallback: a same-network push (tools/ota-push.sh or a browser) while Wi-Fi is up.
    const char *url = hal_ota_url();
    if (url && ph != OTA_DONE) {
        font_draw_string_centered(c, 372, &font_tiny, "or push from a PC to", THEME_DIM);
        font_draw_string_centered(c, 394, &font_tiny, url, THEME_LABEL);
    }
    theme_hint(c, ph == OTA_DONE ? "rebooting..." : "BACK cancel");
}

static void on_render(Canvas *c)
{
    if (s_ota) { render_ota(c); return; }

    theme_clear(c);
    theme_header(c, "Settings", THEME_TITLE, NULL, THEME_DIM);

    int y = 50;
    for (int i = 0; i < S_COUNT; i++) {
        char label[24], value[24];
        field(i, label, value, sizeof(label));
        theme_row(c, y, label, value, i == s_sel, false);
        y += 30;
    }

    theme_hint(c, "UP/DN field   ENC/<> adjust   BACK save");
}

static void on_leave(void) { prefs_save(); }

static void on_input(InputEvent e)
{
    if (s_ota) {   // overlay: only BACK closes it (Wi-Fi keeps serving until reboot)
        if (e.type == INPUT_BTN_BACK || e.type == INPUT_BTN_MENU) s_ota = false;
        return;
    }

    switch (e.type) {
    case INPUT_NAV_UP:   s_sel = (s_sel + S_COUNT - 1) % S_COUNT; break;
    case INPUT_NAV_DOWN: s_sel = (s_sel + 1) % S_COUNT; break;
    case INPUT_ENC_CW:
    case INPUT_NAV_RIGHT: if (s_sel != S_UPDATE) adjust(+1); break;
    case INPUT_ENC_CCW:
    case INPUT_NAV_LEFT:  if (s_sel != S_UPDATE) adjust(-1); break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        if (s_sel == S_UPDATE)      { s_ota = true; hal_ota_pull(); }    // check GitHub + self-flash
        else if (s_sel == S_TAJWEED || s_sel == S_OUTPUT) adjust(0);     // toggle
        else s_sel = (s_sel + 1) % S_COUNT;                             // advance
        break;
    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        prefs_save();
        scene_switch(SCENE_HOME);
        break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_exit = on_leave,
    .on_render = on_render,
    .on_input = on_input,
};

void scene_settings_register(void) { scene_register(SCENE_SETTINGS, &CB); }
