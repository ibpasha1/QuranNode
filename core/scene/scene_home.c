// scene_home.c — the calm home screen.
//
// The anti-phone home: next prayer, one "continue reading" line (the real resume
// point), and a whisper of today's progress. No badges, no feeds. SELECT/PLAY
// resumes the reader at exactly the saved ayah + speed. (Prayer time + the daily
// minutes are still placeholders; they arrive with the prayer-times feature.)
#include "scene.h"
#include "player.h"
#include "progress.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include <stdio.h>

static void resume_into_reader(void)
{
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    player_set_rate(&g_player, r.rate > 0 ? r.rate : 1.0f);
    player_load(&g_player, r.surah, r.ayah);
    scene_switch(SCENE_READER);
}

// A flat panel "card": THEME_PANEL fill with a grid border (no rounded corners,
// per the theme). Returns nothing; callers place content inside.
static void card(Canvas *c, int x, int y, int w, int h)
{
    canvas_rect_fill(c, x, y, w, h, THEME_PANEL);
    canvas_rect(c, x, y, w, h, THEME_GRID);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    theme_header(c, "QURANNODE", THEME_TITLE, "HOME", THEME_DIM);

    const int cx = 14, cw = CANVAS_WIDTH - 28;

    // Next prayer — quiet info card. (Times are placeholders until the
    // prayer-times feature lands.)
    card(c, cx, 34, cw, 118);
    font_draw_string(c, cx + 10, 42, &font_tiny, "NEXT PRAYER", THEME_LABEL);
    font_draw_string_centered(c, 62, &font_large, "MAGHRIB", THEME_TEXT);
    font_draw_string_centered(c, 100, &font_small, "7:43 PM", THEME_LABEL);
    font_draw_string_centered(c, 124, &font_tiny, "in 1h 28m", THEME_DIM);

    // Continue reading — the single call to action, from the real resume point.
    // Cyan cursor outline marks it as the thing SELECT acts on.
    card(c, cx, 190, cw, 96);
    theme_cursor_box(c, cx, 190, cw, 96);
    canvas_rect_fill(c, cx, 190, 3, 96, THEME_ACCENT);
    font_draw_string(c, cx + 12, 198, &font_tiny, "CONTINUE READING", THEME_LABEL);
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    font_draw_string_centered(c, 220, &font_medium, qdb_surah_name(r.surah), THEME_TITLE);
    char sub[40];
    snprintf(sub, sizeof(sub), "%d:%d  ·  %.2fx", r.surah, r.ayah,
             r.rate > 0 ? r.rate : 1.0f);
    font_draw_string_centered(c, 252, &font_tiny, sub, THEME_DIM);
    font_draw_string_right(c, cx + cw - 8, 272, &font_tiny, "SEL >", THEME_ACCENT);

    // Today's listening — placeholder minutes against a notional 30-min habit.
    font_draw_string(c, cx, 330, &font_tiny, "TODAY", THEME_LABEL);
    font_draw_string_right(c, cx + cw, 330, &font_tiny, "23 min", THEME_TEXT);
    theme_bar(c, cx, 342, cw, 4, 23.f / 30.f, false);

    // Where the side keys go — mirrors the hint rail, but visible at a glance.
    font_draw_string(c, cx, 420, &font_tiny, "< LIBRARY", THEME_DIM);
    font_draw_string_right(c, cx + cw, 420, &font_tiny, "QURAN >", THEME_DIM);

    theme_hint(c, "SEL resume   > Quran   < Library");
}

static void on_input(InputEvent e)
{
    switch (e.type) {
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
    case INPUT_BTN_PLAY:
        resume_into_reader();
        break;
    case INPUT_BTN_MENU:
    case INPUT_NAV_RIGHT:
        scene_switch(SCENE_NAV);
        break;
    case INPUT_NAV_LEFT:
        scene_switch(SCENE_LIBRARY);
        break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_render = on_render,
    .on_input = on_input,
};

void scene_home_register(void) { scene_register(SCENE_HOME, &CB); }
