// scene_loop.c — Ayah Loop editor (the memorization machine).
//
// Set a range (start->end ayah), how many times to repeat each ayah and the whole
// section, the pause between clips, and the speed — then Start. The loop runs on
// the shared Player and hands control to the reader, which shows loop progress and
// keeps the word-highlight going. Reached from the reader via the MODE key.
#include "scene.h"
#include "player.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include <stdio.h>

// Editable fields.
enum { F_START, F_END, F_EACH, F_SECTION, F_PAUSE, F_SPEED, F_GO, F_COUNT };

static int   s_sel = F_START;
static LoopConfig s_cfg;
static bool  s_init = false;

static void reset_from_player(void)
{
    s_cfg.start_ayah = g_player.ayah;
    s_cfg.end_ayah = g_player.ayah;
    if (s_cfg.end_ayah < s_cfg.start_ayah) s_cfg.end_ayah = s_cfg.start_ayah;
    s_cfg.each_reps = 3;
    s_cfg.section_reps = 5;
    s_cfg.pause_ms = 1500;
    s_cfg.rate = g_player.rate > 0 ? g_player.rate : 1.0f;
}

static void on_enter(void)
{
    if (!s_init) { reset_from_player(); s_init = true; }
    // Re-anchor the range on the ayah the user was reading.
    if (s_cfg.start_ayah != g_player.ayah && !g_player.loop.active)
        reset_from_player();
    s_sel = F_START;
}

static int amin(void) { return g_player.ayah_min ? g_player.ayah_min : 1; }
static int amax(void) { return g_player.ayah_max ? g_player.ayah_max : 1; }

static void clampi(int *v, int lo, int hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }

static void adjust(int dir)
{
    switch (s_sel) {
    case F_START:
        s_cfg.start_ayah += dir; clampi(&s_cfg.start_ayah, amin(), amax());
        if (s_cfg.end_ayah < s_cfg.start_ayah) s_cfg.end_ayah = s_cfg.start_ayah;
        break;
    case F_END:
        s_cfg.end_ayah += dir; clampi(&s_cfg.end_ayah, s_cfg.start_ayah, amax());
        break;
    case F_EACH:
        s_cfg.each_reps += dir; clampi(&s_cfg.each_reps, 1, 20);
        break;
    case F_SECTION:
        s_cfg.section_reps += dir; clampi(&s_cfg.section_reps, 0, 20);  // 0 = infinite
        break;
    case F_PAUSE:
        s_cfg.pause_ms += dir * 250;
        if ((int)s_cfg.pause_ms < 0) s_cfg.pause_ms = 0;
        if (s_cfg.pause_ms > 10000) s_cfg.pause_ms = 10000;
        break;
    case F_SPEED: {
        float r = s_cfg.rate + dir * 0.05f;
        if (r < 0.5f) r = 0.5f;
        if (r > 2.0f) r = 2.0f;
        s_cfg.rate = r;
        break;
    }
    default: break;
    }
}

static void field_value(int f, char *buf, int n)
{
    switch (f) {
    case F_START:   snprintf(buf, n, "%d:%d", g_player.surah, s_cfg.start_ayah); break;
    case F_END:     snprintf(buf, n, "%d:%d", g_player.surah, s_cfg.end_ayah); break;
    case F_EACH:    snprintf(buf, n, "x%d", s_cfg.each_reps); break;
    case F_SECTION: if (s_cfg.section_reps == 0) snprintf(buf, n, "loop");
                    else snprintf(buf, n, "x%d", s_cfg.section_reps); break;
    case F_PAUSE:   snprintf(buf, n, "%.2fs", s_cfg.pause_ms / 1000.0f); break;
    case F_SPEED:   snprintf(buf, n, "%.2fx", s_cfg.rate); break;
    default:        buf[0] = 0; break;
    }
}

static const char *field_label(int f)
{
    switch (f) {
    case F_START:   return "From ayah";
    case F_END:     return "To ayah";
    case F_EACH:    return "Each ayah";
    case F_SECTION: return "Whole range";
    case F_PAUSE:   return "Pause between";
    case F_SPEED:   return "Speed";
    default:        return "";
    }
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "Ayah Loop", THEME_TITLE, "MODE", THEME_DIM);

    int y = 44;
    for (int f = F_START; f <= F_SPEED; f++) {
        char v[24]; field_value(f, v, sizeof(v));
        theme_row(c, y, field_label(f), v, f == s_sel, false);
        y += 30;
    }

    // Start button row.
    bool go = (s_sel == F_GO);
    int gy = y + 6;
    if (go) theme_sel_block(c, 40, gy, CANVAS_WIDTH - 80, 20);
    font_draw_string_centered(c, gy + 5, &font_small, "START LOOP",
                              go ? THEME_SEL_TEXT : THEME_ACTIVE);

    theme_hint(c, "UP/DN field   ENC/<> adjust   SEL start   BACK reader");
}

static void on_input(InputEvent e)
{
    switch (e.type) {
    case INPUT_NAV_UP:   s_sel = (s_sel + F_COUNT - 1) % F_COUNT; break;
    case INPUT_NAV_DOWN: s_sel = (s_sel + 1) % F_COUNT; break;

    case INPUT_ENC_CW:
    case INPUT_NAV_RIGHT: adjust(+1); break;
    case INPUT_ENC_CCW:
    case INPUT_NAV_LEFT:  adjust(-1); break;

    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        if (s_sel == F_GO) {
            player_start_loop(&g_player, &s_cfg);
            scene_switch(SCENE_READER);
        } else {
            s_sel = (s_sel + 1) % F_COUNT;   // Enter on a field advances to the next
        }
        break;

    case INPUT_BTN_BACK:
    case INPUT_BTN_MODE:
        scene_switch(SCENE_READER);
        break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_render = on_render,
    .on_input = on_input,
};

void scene_loop_register(void) { scene_register(SCENE_LOOP, &CB); }
