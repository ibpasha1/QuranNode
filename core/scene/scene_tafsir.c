// scene_tafsir.c — the Tafsir Game dashboard.
//
// The home of the meaning game: it names your target, shows how much of it
// you've studied and what's due, and starts a sitting. A small scope picker
// (surah / juz / whole Quran, forward or reverse) sets the target, mirroring
// the memorization picker in scene_hifz so the two lessons feel the same.
//
// The scheduling + card logic live in tglearn.c and tafsirgame.c; this file is
// the front door and hands off to SCENE_TAFSIR_PLAY for the actual sitting.
#include "scene.h"
#include "tglearn.h"
#include "qday.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

typedef enum { DASH = 0, PICK } Mode;

static Mode s_mode;
static const char *s_toast;
static int  s_toast_ttl;

// Scope picker: 0 = Surah, 1 = Juz, 2 = Whole Quran.
static int s_kind = 0;
static int s_arg  = 1;
static int s_rev  = 0;
static int s_row;      // 0 type, 1 which, 2 order, 3 start

static void toast(const char *m) { s_toast = m; s_toast_ttl = 90; }

static void on_enter(void)
{
    s_mode = tglearn_scope().kind == TGL_SCOPE_NONE ? PICK : DASH;
    s_toast_ttl = 0;
    // Seed the picker from the current target so it opens where you left off.
    TgScope sc = tglearn_scope();
    if (sc.kind == TGL_SCOPE_JUZ)   { s_kind = 1; s_arg = sc.label_arg; }
    else if (sc.kind == TGL_SCOPE_QURAN) { s_kind = 2; }
    else                            { s_kind = 0; s_arg = sc.label_arg ? sc.label_arg : 1; }
    s_rev = sc.reverse;
    s_row = 0;
}

static void on_leave(void) { tglearn_flush(); }

static void on_tick(uint32_t dt_ms) { (void)dt_ms; if (s_toast_ttl > 0) s_toast_ttl--; }

// -------------------------------------------------------------------------
static void launch_play(void)
{
    hal_audio_click(true);
    scene_set_return(SCENE_TAFSIR);
    scene_switch(SCENE_TAFSIR_PLAY);
}

// -------------------------------------------------------------------------
// Dashboard
// -------------------------------------------------------------------------
static void render_dash(Canvas *c)
{
    const TgStats *st = tglearn_stats();
    float frac = st->scope_total > 0 ? (float)st->scope_done / (float)st->scope_total : 0.f;

    char right[16];
    if (st->have_day) qday_format(st->day, right, sizeof right);
    else              snprintf(right, sizeof right, "--");
    theme_header(c, "MEANINGS", THEME_TITLE, right, st->have_day ? THEME_TEXT : THEME_DIM);

    // Hero: target + progress.
    const int hx = 10, hy = 24, hw = CANVAS_WIDTH - 20, hh = 78;
    canvas_rect_fill(c, hx, hy, hw, hh, THEME_PANEL);
    canvas_rect(c, hx, hy, hw, hh, THEME_GRID);

    char label[32];
    tglearn_scope_label(label, sizeof label);
    font_draw_string(c, hx + 10, hy + 8, &font_medium, label, THEME_TITLE);
    if (tglearn_scope().reverse)
        font_draw_string_right(c, hx + hw - 10, hy + 12, &font_tiny, "REVERSE", THEME_LABEL);

    canvas_progress_bar(c, hx + 10, hy + 36, hw - 20, 6, frac, THEME_ACCENT, THEME_GRID);
    char sub[44];
    snprintf(sub, sizeof sub, "%d / %d ayat  %d%%", st->scope_done, st->scope_total,
             (int)(frac * 100.f + 0.5f));
    font_draw_string(c, hx + 10, hy + 50, &font_tiny, sub, THEME_TEXT);

    // Three little counters: studied / mastered / due.
    const int y = 116, h = 40, w = (CANVAS_WIDTH - 20) / 3;
    struct { const char *n; int v; color_t col; } cells[3] = {
        { "STUDIED",  st->started,  THEME_TEXT   },
        { "MASTERED", st->mastered, THEME_ACTIVE },
        { "DUE",      st->due,      st->due ? THEME_BADGE : THEME_DIM },
    };
    for (int i = 0; i < 3; i++) {
        int x = 10 + i * w;
        canvas_rect_fill(c, x, y, w - 3, h, THEME_PANEL);
        canvas_rect(c, x, y, w - 3, h, THEME_GRID);
        char num[12]; snprintf(num, sizeof num, "%d", cells[i].v);
        int nx = x + (w - 3 - font_string_width(&font_medium, num)) / 2;
        font_draw_string(c, nx, y + 6, &font_medium, num, cells[i].v ? cells[i].col : THEME_DIM);
        int tx = x + (w - 3 - font_string_width(&font_tiny, cells[i].n)) / 2;
        font_draw_string(c, tx, y + 26, &font_tiny, cells[i].n, THEME_LABEL);
    }

    // Call to action.
    const int cy = 190;
    int due = st->due, avail = tglearn_new_available();
    const char *big = due > 0 ? "Review meanings"
                    : avail > 0 ? "Learn meanings"
                                : "All caught up";
    font_draw_string_centered(c, cy, &font_medium, big,
                              due > 0 ? THEME_BADGE : avail > 0 ? THEME_ACCENT : THEME_ACTIVE);
    char line2[40];
    if (due > 0)        snprintf(line2, sizeof line2, "%d ayat waiting", due);
    else if (avail > 0) snprintf(line2, sizeof line2, "%d new in this target", avail);
    else                snprintf(line2, sizeof line2, "nothing due right now");
    font_draw_string_centered(c, cy + 24, &font_tiny, line2, THEME_DIM);

    if (st->answered > 0) {
        char acc[40];
        snprintf(acc, sizeof acc, "lifetime %u / %u correct", st->correct, st->answered);
        font_draw_string_centered(c, cy + 44, &font_tiny, acc, THEME_LABEL);
    }

    if (s_toast_ttl > 0 && s_toast) {
        int tw = font_string_width(&font_tiny, s_toast) + 16;
        int tx = (CANVAS_WIDTH - tw) / 2, ty = CANVAS_HEIGHT - THEME_KEYBAR_H - 22;
        canvas_rect_fill(c, tx, ty, tw, 14, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 14, THEME_ACCENT);
        font_draw_string_centered(c, ty + 4, &font_tiny, s_toast, THEME_ACCENT);
    }

    KeyChip chips[3] = {
        { "OK", "PLAY", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { ">",  "TARGET", 3, { INPUT_NAV_RIGHT, INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "BK", "LESSONS", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, chips, 3);
}

// -------------------------------------------------------------------------
// Scope picker
// -------------------------------------------------------------------------
static int pick_total(void)
{
    if (s_kind == 0) return qdb_ayah_count(s_arg);
    if (s_kind == 2) return QDB_AYAH_TOTAL;
    if (s_arg < 1 || s_arg > QDB_JUZ_COUNT) return 0;
    QRef a = qdb_juz_start(s_arg);
    int g0 = qdb_global_index(a.surah, a.ayah), g1;
    if (s_arg == QDB_JUZ_COUNT) g1 = QDB_AYAH_TOTAL;
    else { QRef b = qdb_juz_start(s_arg + 1); g1 = qdb_global_index(b.surah, b.ayah) - 1; }
    return g1 - g0 + 1;
}

static void render_pick(Canvas *c)
{
    theme_header(c, "WHICH MEANINGS", THEME_TITLE, NULL, THEME_DIM);

    const char *kn = s_kind == 0 ? "Surah" : s_kind == 1 ? "Juz" : "Whole Quran";
    theme_row(c, 40, "Type", kn, s_row == 0, false);

    char v[32];
    if (s_kind == 0)      snprintf(v, sizeof v, "%s", qdb_surah_name(s_arg));
    else if (s_kind == 1) snprintf(v, sizeof v, "Juz %d", s_arg);
    else                  snprintf(v, sizeof v, "--");
    theme_row(c, 68, "Which", v, s_row == 1, s_kind == 2);

    theme_row(c, 96, "Order", s_rev ? "Last to first" : "First to last",
              s_row == 2, s_kind == 2);

    theme_row(c, 130, "Start", "", s_row == 3, false);

    int total = pick_total();
    if (total > 0) {
        char t[48];
        snprintf(t, sizeof t, "%d ayat  ~%d new a day", total, tglearn_new_per_day());
        font_draw_string(c, 12, 172, &font_tiny, t, THEME_LABEL);
    }
    font_draw_string(c, 12, 190, &font_tiny,
                     "Learn what each ayah means, word by word.", THEME_DIM);

    KeyChip chips[3] = {
        { "^v", "ROW", 2, { INPUT_NAV_UP, INPUT_NAV_DOWN } },
        { "<>", "CHANGE", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT, INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "SET", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
    };
    theme_keybar(c, chips, 3);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    if (s_mode == PICK) render_pick(c);
    else                render_dash(c);
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void pick_adjust(int dir)
{
    switch (s_row) {
    case 0:
        s_kind = (s_kind + (dir > 0 ? 1 : 2)) % 3;      // wrap both ways over 3
        if (s_kind == 0) s_arg = 1;
        else if (s_kind == 1) s_arg = 30;
        break;
    case 1: {
        if (s_kind == 2) break;                          // whole Quran: no "which"
        int max = s_kind == 0 ? QDB_SURAH_COUNT : QDB_JUZ_COUNT;
        s_arg += dir;
        if (s_arg < 1) s_arg = max;
        if (s_arg > max) s_arg = 1;
        break;
    }
    case 2: if (s_kind != 2) s_rev = !s_rev; break;
    default: break;
    }
}

static void commit_scope(void)
{
    TgScopeKind k = s_kind == 0 ? TGL_SCOPE_SURAH
                  : s_kind == 1 ? TGL_SCOPE_JUZ
                                : TGL_SCOPE_QURAN;
    tglearn_set_scope(k, s_arg, s_rev != 0);
    s_mode = DASH;
    toast("Target set");
}

static void on_input(InputEvent e)
{
    if (s_mode == PICK) {
        switch (e.type) {
        case INPUT_NAV_UP:    if (s_row > 0) { s_row--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:  if (s_row < 3) { s_row++; hal_audio_click(false); } break;
        case INPUT_NAV_LEFT:
        case INPUT_ENC_CCW:   pick_adjust(-1); hal_audio_click(false); break;
        case INPUT_NAV_RIGHT:
        case INPUT_ENC_CW:    pick_adjust(+1); hal_audio_click(false); break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
            hal_audio_click(true);
            if (s_row == 3) commit_scope();
            else s_row++;
            break;
        case INPUT_BTN_BACK:
            // Refuse to leave with no target — the dashboard would be empty.
            if (tglearn_scope().kind == TGL_SCOPE_NONE) scene_switch(SCENE_HOME);
            else s_mode = DASH;
            break;
        default: break;
        }
        return;
    }

    switch (e.type) {
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        launch_play();
        break;
    case INPUT_NAV_RIGHT:
    case INPUT_ENC_CW:
    case INPUT_ENC_CCW:
        hal_audio_click(true);
        s_row = 0;
        s_mode = PICK;
        break;
    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        tglearn_flush();
        scene_switch(SCENE_HOME);
        break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_exit = on_leave,
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = on_tick,
};

void scene_tafsir_register(void) { scene_register(SCENE_TAFSIR, &CB); }
