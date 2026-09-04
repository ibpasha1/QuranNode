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
#include <stdio.h>

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
        hal_audio_set_volume(g_prefs.volume / 100.0f * 2.0f);
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

// Full-screen overlay while an over-the-air update is armed.
static void render_ota(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "Firmware Update", THEME_TITLE, NULL, THEME_DIM);

    // Primary path: pulling the latest release from GitHub. Show its progress.
    const char *status = hal_ota_status();
    font_draw_string_centered(c, 170, &font_small, status ? status : "Starting...", THEME_TITLE);

    // Fallback: a same-network push (tools/ota-push.sh or a browser) while Wi-Fi is up.
    const char *url = hal_ota_url();
    if (url) {
        font_draw_string_centered(c, 250, &font_tiny, "or push from a PC to", THEME_DIM);
        font_draw_string_centered(c, 272, &font_tiny, url, THEME_DIM);
    }
    theme_hint(c, "BACK cancel");
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
