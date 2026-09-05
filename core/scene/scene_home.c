// scene_home.c — the launcher home.
//
// Boot lands here: a hero "continue reading" widget (the real resume point,
// framed like an illuminated mushaf margin), a live next-prayer strip, and the
// app list — Quran, Lessons, Quran Teacher, Practice, Library, Settings.
// Lessons/Teacher are visible but marked SOON until those modes exist. The
// bottom keybar mirrors the physical buttons in real time.
#include "scene.h"
#include "player.h"
#include "progress.h"
#include "quran_db.h"
#include "prayer.h"
#include "prefs.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include <stdio.h>

typedef struct {
    const char *label;
    UiIcon      icon;
    SceneID     target;   // SCENE_COUNT = not built yet
} HomeItem;

static const HomeItem ITEMS[] = {
    { "Quran",         ICON_BOOK,   SCENE_NAV },
    { "Lessons",       ICON_NOTE,   SCENE_COUNT },
    { "Quran Teacher", ICON_WAVE,   SCENE_COUNT },
    { "Practice",      ICON_REPEAT, SCENE_LOOP },
    { "Library",       ICON_FOLDER, SCENE_LIBRARY },
    { "Settings",      ICON_GEAR,   SCENE_SETTINGS },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

static int s_sel = 0;     // 0 = hero card, 1..N_ITEMS = ITEMS[s_sel-1]
static int s_toast = 0;   // frames left on the "Coming soon" toast

static void resume_into_reader(void)
{
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    player_set_rate(&g_player, r.rate > 0 ? r.rate : 1.0f);
    player_load(&g_player, r.surah, r.ayah);
    scene_switch(SCENE_READER);
}

// "7:43 PM" from minutes-after-midnight.
static void fmt12(int min, char *b, int n)
{
    int h = (min / 60) % 24, m = min % 60;
    const char *ap = h >= 12 ? "PM" : "AM";
    h %= 12;
    if (h == 0) h = 12;
    snprintf(b, n, "%d:%02d %s", h, m, ap);
}

// Double-rule frame with corner ticks — the illuminated-margin nod.
static void hero_frame(Canvas *c, int x, int y, int w, int h, bool sel)
{
    color_t outer = sel ? THEME_ACCENT : THEME_GRID;
    color_t inner = sel ? THEME_BAR : THEME_GRID;
    canvas_rect(c, x, y, w, h, outer);
    canvas_rect(c, x + 3, y + 3, w - 6, h - 6, inner);
    // Corner ticks bridging the two rules.
    canvas_rect_fill(c, x + 1, y + 1, 2, 2, outer);
    canvas_rect_fill(c, x + w - 3, y + 1, 2, 2, outer);
    canvas_rect_fill(c, x + 1, y + h - 3, 2, 2, outer);
    canvas_rect_fill(c, x + w - 3, y + h - 3, 2, 2, outer);
}

static void on_render(Canvas *c)
{
    theme_clear(c);

    // Wall clock (may be unknown on-device before NTP).
    int64_t epoch = hal_wall_clock();
    int tz = hal_tz_offset_min();
    int now_min = -1;
    char clock[12] = "--:--";
    if (epoch > 0) {
        now_min = (int)(((epoch + (int64_t)tz * 60) % 86400) / 60);
        fmt12(now_min, clock, sizeof(clock));
    }
    theme_header(c, "QURANNODE", THEME_TITLE, clock,
                 epoch > 0 ? THEME_TEXT : THEME_DIM);

    // --- Hero: continue reading -------------------------------------------
    const int hx = 10, hy = 22, hw = CANVAS_WIDTH - 20, hh = 100;
    bool hero_sel = (s_sel == 0);
    canvas_rect_fill(c, hx, hy, hw, hh, hero_sel ? THEME_ROW : THEME_PANEL);
    hero_frame(c, hx, hy, hw, hh, hero_sel);

    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    int ayat = qdb_ayah_count(r.surah);
    int juz = qdb_juz_of(r.surah, r.ayah);

    font_draw_string(c, hx + 12, hy + 10, &font_tiny, "CONTINUE READING",
                     THEME_LABEL);
    char badge[12];
    snprintf(badge, sizeof(badge), "JUZ %d", juz);
    font_draw_string_right(c, hx + hw - 12, hy + 10, &font_tiny, badge,
                           THEME_TITLE);

    font_draw_string_centered(c, hy + 28, &font_medium, qdb_surah_name(r.surah),
                              THEME_TEXT);
    char sub[40];
    snprintf(sub, sizeof(sub), "Ayah %d of %d   ·   %.2fx", r.ayah, ayat,
             r.rate > 0 ? r.rate : 1.0f);
    font_draw_string_centered(c, hy + 58, &font_tiny, sub, THEME_DIM);

    float frac = ayat > 1 ? (float)(r.ayah - 1) / (float)(ayat - 1) : 0.f;
    canvas_progress_bar(c, hx + 14, hy + 78, hw - 28, 5, frac,
                        THEME_ACCENT, THEME_GRID);

    // --- Next prayer strip ---------------------------------------------------
    const int py = 132, ph = 22;
    canvas_rect_fill(c, 0, py, CANVAS_WIDTH, ph, THEME_PANEL);
    canvas_hline(c, 0, py, CANVAS_WIDTH, THEME_GRID);
    canvas_hline(c, 0, py + ph - 1, CANVAS_WIDTH, THEME_GRID);
    PrayerTimes pt;
    if (now_min >= 0 &&
        prayer_compute(epoch, tz, g_prefs.lat, g_prefs.lng, &pt)) {
        PrayerId next;
        int until = prayer_next(&pt, now_min, &next);
        char at[12], in[20];
        fmt12(pt.minutes[next], at, sizeof(at));
        if (until >= 60)
            snprintf(in, sizeof(in), "in %dh %02dm", until / 60, until % 60);
        else
            snprintf(in, sizeof(in), "in %dm", until);
        char name[12];
        snprintf(name, sizeof(name), "%s", prayer_name(next));
        for (char *p = name; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p += 'A' - 'a';
        font_draw_string(c, 12, py + 7, &font_tiny, name, THEME_TITLE);
        char rest[16];
        snprintf(rest, sizeof(rest), "  %s", at);
        font_draw_string(c, 12 + font_string_width(&font_tiny, name), py + 7,
                         &font_tiny, rest, THEME_TEXT);
        font_draw_string_right(c, CANVAS_WIDTH - 12, py + 7, &font_tiny, in,
                               THEME_DIM);
    } else {
        font_draw_string(c, 12, py + 7, &font_tiny, "PRAYER TIMES", THEME_LABEL);
        font_draw_string_right(c, CANVAS_WIDTH - 12, py + 7, &font_tiny,
                               "clock not set", THEME_DIM);
    }

    // --- App list ------------------------------------------------------------
    const int ly = 168, row_h = 38, gap = 5;
    for (int i = 0; i < N_ITEMS; i++) {
        int y = ly + i * (row_h + gap);
        bool sel = (s_sel == i + 1);
        bool soon = (ITEMS[i].target == SCENE_COUNT);
        if (sel) {
            theme_sel_block(c, 10, y, CANVAS_WIDTH - 20, row_h);
        } else {
            canvas_rect_fill(c, 10, y, CANVAS_WIDTH - 20, row_h, THEME_PANEL);
            canvas_rect(c, 10, y, CANVAS_WIDTH - 20, row_h, THEME_GRID);
        }
        color_t fg = sel ? THEME_SEL_TEXT : (soon ? THEME_DIM : THEME_TEXT);
        theme_icon(c, 24, y + (row_h - 9) / 2, ITEMS[i].icon,
                   sel ? THEME_SEL_TEXT : THEME_LABEL,
                   sel ? THEME_SEL_BG : THEME_PANEL);
        font_draw_string(c, 44, y + (row_h - 14) / 2, &font_small,
                         ITEMS[i].label, fg);
        if (soon)
            font_draw_string_right(c, CANVAS_WIDTH - 22, y + (row_h - 7) / 2,
                                   &font_tiny, "SOON",
                                   sel ? THEME_SEL_TEXT : THEME_BADGE);
        else
            font_draw_string_right(c, CANVAS_WIDTH - 22, y + (row_h - 7) / 2,
                                   &font_tiny, ">",
                                   sel ? THEME_SEL_TEXT : THEME_DIM);
    }

    // "Coming soon" toast above the keybar.
    if (s_toast > 0) {
        const char *msg = "Coming soon, insha'Allah";
        int tw = font_string_width(&font_tiny, msg) + 16;
        int tx = (CANVAS_WIDTH - tw) / 2, ty = CANVAS_HEIGHT - THEME_KEYBAR_H - 22;
        canvas_rect_fill(c, tx, ty, tw, 14, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 14, THEME_BADGE);
        font_draw_string_centered(c, ty + 4, &font_tiny, msg, THEME_BADGE);
    }

    // --- Live keybar ---------------------------------------------------------
    bool on_soon = s_sel > 0 && ITEMS[s_sel - 1].target == SCENE_COUNT;
    KeyChip chips[3] = {
        { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                               INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", s_sel == 0 ? "RESUME" : (on_soon ? "PEEK" : "OPEN"), 2,
          { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "PL", "RESUME", 1, { INPUT_BTN_PLAY } },
    };
    theme_keybar(c, chips, 3);
}

static void move(int dir)
{
    int next = s_sel + dir;
    if (next < 0 || next > N_ITEMS) return;
    s_sel = next;
    hal_audio_click(false);
}

static void activate(void)
{
    hal_audio_click(true);
    if (s_sel == 0) { resume_into_reader(); return; }
    const HomeItem *it = &ITEMS[s_sel - 1];
    if (it->target == SCENE_COUNT) { s_toast = 60; return; }
    scene_switch(it->target);
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (s_toast > 0) s_toast--;
}

static void on_input(InputEvent e)
{
    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:   move(-1); break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:    move(+1); break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:  activate(); break;
    case INPUT_BTN_PLAY:  resume_into_reader(); break;
    case INPUT_NAV_RIGHT:
    case INPUT_BTN_MENU:  scene_switch(SCENE_NAV); break;
    case INPUT_NAV_LEFT:  scene_switch(SCENE_LIBRARY); break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = on_tick,
};

void scene_home_register(void) { scene_register(SCENE_HOME, &CB); }
