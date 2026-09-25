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

enum { S_SPEED, S_VOLUME, S_OUTPUT, S_FONT, S_BRIGHT, S_TAJWEED,
       S_LOCATION, S_DST, S_SETCLOCK, S_WIFI, S_PUSH, S_UPDATE, S_COUNT };

// Overlays layered over the field list (same idea as the old s_ota bool).
enum { MODE_MAIN, MODE_OTA, MODE_CITY, MODE_CLOCK };
static int s_mode;

static int  s_sel;

// --- Location city table ---------------------------------------------------
// A short world list so location + time zone can be set without typing. `tz` is
// the STANDARD-time UTC offset in minutes; the separate Daylight-saving toggle
// adds an hour in summer. Picking a city sets lat/lng and tz_std_min at once.
typedef struct { const char *name; float lat, lng; int16_t tz; } City;
static const City CITIES[] = {
    { "Mecca",         21.4225f,  39.8262f,  180 },
    { "Medina",        24.4700f,  39.6100f,  180 },
    { "New York",      40.7100f, -74.0100f, -300 },
    { "Chicago",       41.8500f, -87.6500f, -360 },
    { "Denver",        39.7400f,-104.9900f, -420 },
    { "Los Angeles",   34.0500f,-118.2400f, -480 },
    { "Toronto",       43.6500f, -79.3800f, -300 },
    { "Mexico City",   19.4300f, -99.1300f, -360 },
    { "Sao Paulo",    -23.5500f, -46.6300f, -180 },
    { "London",        51.5100f,  -0.1300f,    0 },
    { "Paris",         48.8600f,   2.3500f,   60 },
    { "Berlin",        52.5200f,  13.4100f,   60 },
    { "Cairo",         30.0400f,  31.2400f,  120 },
    { "Istanbul",      41.0100f,  28.9800f,  180 },
    { "Riyadh",        24.7100f,  46.6800f,  180 },
    { "Dubai",         25.2000f,  55.2700f,  240 },
    { "Karachi",       24.8600f,  67.0100f,  300 },
    { "Delhi",         28.6100f,  77.2300f,  330 },
    { "Dhaka",         23.8100f,  90.4100f,  360 },
    { "Jakarta",       -6.2100f, 106.8500f,  420 },
    { "Kuala Lumpur",   3.1400f, 101.6900f,  480 },
    { "Singapore",      1.3500f, 103.8200f,  480 },
    { "Beijing",       39.9000f, 116.4100f,  480 },
    { "Tokyo",         35.6800f, 139.7700f,  540 },
    { "Sydney",       -33.8700f, 151.2100f,  600 },
    { "Auckland",     -36.8500f, 174.7600f,  720 },
};
#define N_CITIES ((int)(sizeof(CITIES) / sizeof(CITIES[0])))

static int s_city_sel;   // cursor in the city picker

// Nearest city to the current lat/lng, for showing a name on the Location row.
static int nearest_city(void)
{
    int best = 0; float bestd = 1e18f;
    for (int i = 0; i < N_CITIES; i++) {
        float dla = CITIES[i].lat - g_prefs.lat, dlo = CITIES[i].lng - g_prefs.lng;
        float d = dla * dla + dlo * dlo;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// --- Manual clock editor ---------------------------------------------------
static int s_clk[5];   // local Y, Mon(1-12), Day(1-31), Hour(0-23), Min(0-59)
static int s_clk_f;    // selected field 0..4

// Proleptic Gregorian civil<->days (Howard Hinnant), dependency-free so it
// works identically on device and in the sim without timegm/mktime tz quirks.
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = yoe + (int)era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int dd = doy - (153 * mp + 2) / 5 + 1;
    int mm = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (mm <= 2); *m = mm; *d = dd;
}

static int days_in_month(int y, int m)
{
    static const int dim[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dim[(m - 1) % 12];
}

// Seed the editor from the current local time, or a sane date if unset.
static void clock_editor_open(void)
{
    int64_t e = hal_wall_clock();
    int tz = prefs_tz_offset_min();
    if (e > 0) {
        int64_t local = e + (int64_t)tz * 60;
        int64_t days = local / 86400; int rem = (int)(local - days * 86400);
        if (rem < 0) { rem += 86400; days--; }
        civil_from_days(days, &s_clk[0], &s_clk[1], &s_clk[2]);
        s_clk[3] = rem / 3600; s_clk[4] = (rem % 3600) / 60;
    } else {
        s_clk[0] = 2026; s_clk[1] = 1; s_clk[2] = 1; s_clk[3] = 12; s_clk[4] = 0;
    }
    s_clk_f = 0;
}

// Apply the editor: local Y/M/D H:M -> UTC epoch -> hal_clock_set.
static void clock_editor_apply(void)
{
    int tz = prefs_tz_offset_min();
    int64_t local = days_from_civil(s_clk[0], s_clk[1], s_clk[2]) * 86400
                  + (int64_t)s_clk[3] * 3600 + (int64_t)s_clk[4] * 60;
    hal_clock_set(local - (int64_t)tz * 60);
}

static void clock_editor_adjust(int dir)
{
    switch (s_clk_f) {
    case 0: s_clk[0] += dir; if (s_clk[0] < 2020) s_clk[0] = 2020; if (s_clk[0] > 2099) s_clk[0] = 2099; break;
    case 1: s_clk[1] += dir; if (s_clk[1] < 1) s_clk[1] = 12; if (s_clk[1] > 12) s_clk[1] = 1; break;
    case 2: s_clk[2] += dir; { int dm = days_in_month(s_clk[0], s_clk[1]);
            if (s_clk[2] < 1) s_clk[2] = dm; if (s_clk[2] > dm) s_clk[2] = 1; } break;
    case 3: s_clk[3] += dir; if (s_clk[3] < 0) s_clk[3] = 23; if (s_clk[3] > 23) s_clk[3] = 0; break;
    case 4: s_clk[4] += dir; if (s_clk[4] < 0) s_clk[4] = 59; if (s_clk[4] > 59) s_clk[4] = 0; break;
    }
    int dm = days_in_month(s_clk[0], s_clk[1]);   // month/year change can shorten the day
    if (s_clk[2] > dm) s_clk[2] = dm;
}

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
    case S_OUTPUT:   // cycle 0 headphone -> 1 speaker -> 2 auto
        g_prefs.output = (uint8_t)((g_prefs.output + (dir < 0 ? 2 : 1)) % 3);
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
    case S_DST:   // no-op while tz is Auto (offset comes from the platform)
        if (g_prefs.tz_std_min != TZ_AUTO)
            g_prefs.dst = dir > 0 ? 1 : (dir < 0 ? 0 : !g_prefs.dst);
        break;
    }
}

static void field(int i, char *label, char *value, int n)
{
    switch (i) {
    case S_SPEED:   snprintf(label, n, "Recitation speed"); snprintf(value, n, "%.2fx", g_prefs.rate); break;
    case S_VOLUME:  snprintf(label, n, "Volume");           snprintf(value, n, "%d%%", g_prefs.volume); break;
    case S_OUTPUT:  snprintf(label, n, "Output");
        snprintf(value, n, "%s", g_prefs.output == 1 ? "Speaker" :
                                 g_prefs.output == 0 ? "Headphone" :
                                 hal_audio_headphone_present() ? "Auto (Headphone)" : "Auto (Speaker)");
        break;
    case S_FONT:    snprintf(label, n, "Font size");        snprintf(value, n, "%s", prefs_font_name()); break;
    case S_BRIGHT:  snprintf(label, n, "Brightness");       snprintf(value, n, "%d%%", g_prefs.brightness); break;
    case S_TAJWEED: snprintf(label, n, "Tajweed colors");   snprintf(value, n, "%s", g_prefs.tajweed ? "On" : "Off"); break;
    case S_LOCATION: snprintf(label, n, "Location");        snprintf(value, n, "%s >", CITIES[nearest_city()].name); break;
    case S_DST:     snprintf(label, n, "Daylight saving");
        if (g_prefs.tz_std_min == TZ_AUTO) snprintf(value, n, "%s", "Auto");
        else                               snprintf(value, n, "%s", g_prefs.dst ? "On" : "Off");
        break;
    case S_SETCLOCK: snprintf(label, n, "Set clock"); {
        int64_t e = hal_wall_clock();
        if (e <= 0) { snprintf(value, n, "%s", "not set >"); break; }
        int64_t local = e + (int64_t)prefs_tz_offset_min() * 60;
        int64_t days = local / 86400; int rem = (int)(local - days * 86400);
        if (rem < 0) { rem += 86400; days--; }
        int yy, mm, dd; civil_from_days(days, &yy, &mm, &dd);
        static const char *MON[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };
        snprintf(value, n, "%s %d %02d:%02d >", MON[(mm - 1) % 12], dd,
                 rem / 3600, (rem % 3600) / 60);
        } break;
    case S_WIFI:    snprintf(label, n, "Wi-Fi setup");
        snprintf(value, n, "%s", hal_wifi_have_creds() ? "Re-scan >" : "Scan QR >"); break;
    case S_PUSH:    snprintf(label, n, "Wi-Fi push (local)");
        snprintf(value, n, "%s", "Wi-Fi >"); break;
    case S_UPDATE:  snprintf(label, n, "Update firmware");
        snprintf(value, n, "%s", hal_ota_update_available() ? "Available >" : "Wi-Fi >"); break;
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
    ui_text_centered(c, 262, &ui_font_body, line, ph == OTA_FAIL ? COLOR_RED : THEME_TITLE);

    if (active)
        ota_scanner(c, 60, 300, c->width - 120, 8, t, accent);

    // Fallback: a same-network push (tools/ota-push.sh or a browser) while Wi-Fi is up.
    const char *url = hal_ota_url();
    if (url && ph != OTA_DONE) {
        ui_text_centered(c, 372, &ui_font_cap, "or push from a PC to", THEME_DIM);
        ui_text_centered(c, 394, &ui_font_cap, url, THEME_LABEL);
    }
    theme_hint(c, ph == OTA_DONE ? "rebooting..." : "BACK cancel");
}

// --- Location picker overlay: a scrolling city list ------------------------
static void render_city(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "Location", THEME_TITLE, NULL, THEME_DIM);

    const int top = 50, rowh = 30, visible = 11;   // rows that fit above the hint
    int first = s_city_sel - visible / 2;
    if (first < 0) first = 0;
    if (first > N_CITIES - visible) first = N_CITIES - visible;
    if (first < 0) first = 0;

    int y = top;
    for (int i = first; i < N_CITIES && i < first + visible; i++) {
        char val[16];
        int off = CITIES[i].tz;
        snprintf(val, sizeof val, "UTC%+d:%02d", off / 60, (off < 0 ? -off : off) % 60);
        theme_row(c, y, CITIES[i].name, val, i == s_city_sel, false);
        y += rowh;
    }
    theme_hint(c, "UP/DN pick   OK set   BACK cancel");
}

// --- Manual clock editor overlay -------------------------------------------
static void render_clock(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "Set clock", THEME_TITLE, NULL, THEME_DIM);

    static const char *MON[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };
    char vals[5][8];
    snprintf(vals[0], 8, "%d",   s_clk[0]);
    snprintf(vals[1], 8, "%s",   MON[(s_clk[1] - 1) % 12]);
    snprintf(vals[2], 8, "%d",   s_clk[2]);
    snprintf(vals[3], 8, "%02d", s_clk[3]);
    snprintf(vals[4], 8, "%02d", s_clk[4]);
    static const char *LBL[5] = { "Year", "Month", "Day", "Hour", "Minute" };

    int y = 70;
    for (int i = 0; i < 5; i++) {
        theme_row(c, y, LBL[i], vals[i], i == s_clk_f, false);
        y += 34;
    }
    ui_text_centered(c, y + 6, &ui_font_cap,
                              "enter your LOCAL time", THEME_DIM);
    theme_hint(c, "UP/DN field   ENC/<> change   OK set   BACK cancel");
}

static void on_render(Canvas *c)
{
    if (s_mode == MODE_OTA)   { render_ota(c);   return; }
    if (s_mode == MODE_CITY)  { render_city(c);  return; }
    if (s_mode == MODE_CLOCK) { render_clock(c); return; }

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
    if (s_mode == MODE_OTA) {   // overlay: only BACK closes it (Wi-Fi serves until reboot)
        if (e.type == INPUT_BTN_BACK || e.type == INPUT_BTN_MENU) s_mode = MODE_MAIN;
        return;
    }

    if (s_mode == MODE_CITY) {
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_city_sel > 0)            { s_city_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_city_sel < N_CITIES - 1) { s_city_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: {
            const City *ct = &CITIES[s_city_sel];
            g_prefs.lat = ct->lat; g_prefs.lng = ct->lng; g_prefs.tz_std_min = ct->tz;
            hal_audio_click(true);
            s_mode = MODE_MAIN;
            break;
        }
        case INPUT_BTN_BACK:
        case INPUT_BTN_MENU: s_mode = MODE_MAIN; break;
        default: break;
        }
        return;
    }

    if (s_mode == MODE_CLOCK) {
        switch (e.type) {
        case INPUT_NAV_UP:    s_clk_f = (s_clk_f + 4) % 5; hal_audio_click(false); break;
        case INPUT_NAV_DOWN:  s_clk_f = (s_clk_f + 1) % 5; hal_audio_click(false); break;
        case INPUT_NAV_RIGHT:
        case INPUT_ENC_CW:    clock_editor_adjust(+1); hal_audio_click(false); break;
        case INPUT_NAV_LEFT:
        case INPUT_ENC_CCW:   clock_editor_adjust(-1); hal_audio_click(false); break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:  clock_editor_apply(); hal_audio_click(true); s_mode = MODE_MAIN; break;
        case INPUT_BTN_BACK:
        case INPUT_BTN_MENU:  s_mode = MODE_MAIN; break;
        default: break;
        }
        return;
    }

    switch (e.type) {
    case INPUT_NAV_UP:   s_sel = (s_sel + S_COUNT - 1) % S_COUNT; break;
    case INPUT_NAV_DOWN: s_sel = (s_sel + 1) % S_COUNT; break;
    case INPUT_ENC_CW:
    case INPUT_NAV_RIGHT: if (s_sel != S_UPDATE && s_sel != S_PUSH && s_sel != S_WIFI && s_sel != S_LOCATION && s_sel != S_SETCLOCK) adjust(+1); break;
    case INPUT_ENC_CCW:
    case INPUT_NAV_LEFT:  if (s_sel != S_UPDATE && s_sel != S_PUSH && s_sel != S_WIFI && s_sel != S_LOCATION && s_sel != S_SETCLOCK) adjust(-1); break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        if (s_sel == S_UPDATE)        { s_mode = MODE_OTA; hal_ota_pull(); }   // check GitHub + self-flash
        else if (s_sel == S_WIFI)     { prefs_save(); scene_set_return(SCENE_SETTINGS); scene_switch(SCENE_WIFI_SETUP); }
        else if (s_sel == S_PUSH)     { s_mode = MODE_OTA; hal_ota_start(); }  // Wi-Fi + local /update server, NO GitHub pull
        else if (s_sel == S_LOCATION) { s_city_sel = nearest_city(); s_mode = MODE_CITY; }
        else if (s_sel == S_SETCLOCK) { clock_editor_open(); s_mode = MODE_CLOCK; }
        else if (s_sel == S_TAJWEED || s_sel == S_OUTPUT || s_sel == S_DST) adjust(0);  // toggle
        else s_sel = (s_sel + 1) % S_COUNT;                                    // advance
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
