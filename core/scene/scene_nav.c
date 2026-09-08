// scene_nav.c — jump navigation (Surah / Juz / Page / Bookmarks).
//
// Replaces endless scrolling: pick a way in (by surah, by juz, by mushaf page,
// or a saved bookmark) and land in the reader at that exact ayah. Surahs that
// aren't in the bundled sample yet are shown dimmed so it's clear what has
// content, and each entry carries how much of it you've already read.
#include "scene.h"
#include "player.h"
#include "progress.h"
#include "khatm.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "input_accel.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>

typedef enum { NAV_ROOT, NAV_SURAH, NAV_JUZ, NAV_PAGE, NAV_BOOKMARKS,
               NAV_MODE_COUNT } NavMode;

static NavMode s_mode = NAV_ROOT;
static int s_sel[NAV_MODE_COUNT];      // selection per mode
static int s_scroll[NAV_MODE_COUNT];   // scroll per mode
static InputAccel s_accel;   // hold-to-scroll ramp for the long lists

#define ROW_H 18
#define LIST_TOP 28
#define VIS ((CANVAS_HEIGHT - 16 - LIST_TOP) / ROW_H)

// Root rows are taller "launcher" entries with an icon and a right-hand detail.
#define ROOT_ROW_H 34

// The bundled sample only has content for these surahs (extend as data grows).
static bool content_available(int surah) { return surah == 1; }

// Root launcher rows. Keep in sync with ROOT_ROWS in render_root() and enter().
#define N_ROOT 6

static int list_count(void)
{
    switch (s_mode) {
    case NAV_ROOT:      return N_ROOT;
    case NAV_SURAH:     return QDB_SURAH_COUNT;
    case NAV_JUZ:       return QDB_JUZ_COUNT;
    case NAV_PAGE:      return QDB_PAGE_COUNT;
    case NAV_BOOKMARKS: return progress_bookmark_count();
    default:            return 0;
    }
}

// "100%" / "42%" / "" when untouched — the read fraction of a list entry.
static void pct_str(float frac, char *b, int n)
{
    int p = (int)(frac * 100.f + 0.5f);
    if (p <= 0) b[0] = 0;
    else snprintf(b, n, "%d%%", p);
}

// Fill the three columns of a list row: leading index (soft blue), main name,
// and a right-aligned detail (dim). Any column may come back empty.
static void row_cols(int i, char *idx, int ni, char *name, int nn,
                     char *detail, int nd, bool *dim)
{
    idx[0] = name[0] = detail[0] = 0;
    *dim = false;
    switch (s_mode) {
    case NAV_ROOT:
        break;   // root uses its own launcher rows
    case NAV_SURAH: {
        int s = i + 1;
        char pct[8];
        pct_str(khatm_surah_frac(s), pct, sizeof pct);
        snprintf(idx, ni, "%d", s);
        snprintf(name, nn, "%s", qdb_surah_name(s));
        if (pct[0]) snprintf(detail, nd, "%d ayat  %s", qdb_ayah_count(s), pct);
        else        snprintf(detail, nd, "%d ayat", qdb_ayah_count(s));
        *dim = !content_available(s);
        break;
    }
    case NAV_JUZ: {
        int j = i + 1;
        QRef r = qdb_juz_start(j);
        char pct[8];
        pct_str(khatm_juz_frac(j), pct, sizeof pct);
        snprintf(idx, ni, "%d", j);
        snprintf(name, nn, "%s", qdb_surah_name(r.surah));
        if (pct[0]) snprintf(detail, nd, "p%d  %s", qdb_juz_page(j), pct);
        else        snprintf(detail, nd, "p%d", qdb_juz_page(j));
        *dim = !content_available(r.surah);
        break;
    }
    case NAV_PAGE: {
        int p = i + 1;
        QRef r = qdb_page_start(p);
        char pct[8];
        pct_str(khatm_page_frac(p), pct, sizeof pct);
        snprintf(idx, ni, "%d", p);
        snprintf(name, nn, "%s", qdb_surah_name(r.surah));
        if (pct[0]) snprintf(detail, nd, "%d:%d  %s", r.surah, r.ayah, pct);
        else        snprintf(detail, nd, "%d:%d", r.surah, r.ayah);
        *dim = !content_available(r.surah);
        break;
    }
    case NAV_BOOKMARKS: {
        Bookmark b = progress_bookmark(i);
        snprintf(name, nn, "%s", qdb_surah_name(b.surah));
        snprintf(detail, nd, "%d:%d", b.surah, b.ayah);
        *dim = !content_available(b.surah);
        break;
    }
    default: break;
    }
}

static void jump_to(int surah, int ayah)
{
    player_set_rate(&g_player, g_player.rate > 0 ? g_player.rate : 1.0f);
    player_load(&g_player, surah, ayah);
    scene_switch(SCENE_READER);
}

static const char *mode_title(void)
{
    switch (s_mode) {
    case NAV_ROOT: return "NAVIGATE";
    case NAV_SURAH: return "NAVIGATE / SURAH";
    case NAV_JUZ: return "NAVIGATE / JUZ";
    case NAV_PAGE: return "NAVIGATE / PAGE";
    case NAV_BOOKMARKS: return "NAVIGATE / BOOKMARKS";
    default: return "";
    }
}

// The root menu as launcher rows: icon, name, and a live detail line.
static const struct { const char *label; UiIcon icon; } ROOT_ROWS[N_ROOT] = {
    { "By Surah",  ICON_NOTE },
    { "By Juz",    ICON_FOLDER },
    { "By Page",   ICON_BOOK },
    { "Bookmarks", ICON_INFO },
    { "Progress",  ICON_INFO },
    { "Settings",  ICON_GEAR },
};

static void render_root(Canvas *c)
{
    int sel = s_sel[NAV_ROOT];
    for (int i = 0; i < N_ROOT; i++) {
        int y = LIST_TOP + 6 + i * (ROOT_ROW_H + 6);
        bool is_sel = (i == sel);
        char detail[24];
        switch (i) {
        case 0:  snprintf(detail, sizeof(detail), "114"); break;
        case 1:  snprintf(detail, sizeof(detail), "30"); break;
        case 2:  snprintf(detail, sizeof(detail), "604"); break;
        case 3:  snprintf(detail, sizeof(detail), "%d", progress_bookmark_count()); break;
        case 4:  snprintf(detail, sizeof(detail), "%d%%",
                          (int)(khatm_stats()->percent + 0.5f)); break;
        default: detail[0] = 0; break;
        }
        bool dim = (i == 3 && progress_bookmark_count() == 0);

        if (is_sel) theme_sel_block(c, 6, y, CANVAS_WIDTH - 12, ROOT_ROW_H);
        else        canvas_rect(c, 6, y, CANVAS_WIDTH - 12, ROOT_ROW_H, THEME_GRID);

        color_t fg = is_sel ? THEME_SEL_TEXT : (dim ? THEME_DIM : THEME_TEXT);
        theme_icon(c, 18, y + (ROOT_ROW_H - 9) / 2, ROOT_ROWS[i].icon,
                   is_sel ? THEME_SEL_TEXT : THEME_LABEL,
                   is_sel ? THEME_SEL_BG : THEME_BG);
        font_draw_string(c, 38, y + (ROOT_ROW_H - 14) / 2, &font_small, ROOT_ROWS[i].label, fg);
        if (detail[0])
            font_draw_string_right(c, CANVAS_WIDTH - 18, y + (ROOT_ROW_H - 7) / 2,
                                   &font_tiny, detail,
                                   is_sel ? THEME_SEL_TEXT : THEME_DIM);
    }
}

static void on_render(Canvas *c)
{
    theme_clear(c);

    int count = list_count();
    int sel = s_sel[s_mode], scroll = s_scroll[s_mode];

    // Header with a breadcrumb title and a "position/total" readout on lists.
    char right[16] = "";
    if (s_mode != NAV_ROOT && count > 0)
        snprintf(right, sizeof(right), "%d/%d", sel + 1, count);
    theme_header(c, mode_title(), THEME_TITLE, right[0] ? right : NULL, THEME_DIM);

    KeyChip chips[3] = {
        { "^v", s_mode == NAV_ROOT ? "CHOOSE" : "SCROLL", 4,
          { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "OPEN", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "BK", s_mode == NAV_ROOT ? "HOME" : "BACK", 1, { INPUT_BTN_BACK } },
    };

    if (s_mode == NAV_ROOT) {
        render_root(c);
        theme_keybar(c, chips, 3);
        return;
    }

    if (count == 0) {
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 4, &font_small,
                                  "Nothing here yet", THEME_DIM);
        theme_hint(c, "BACK up");
        return;
    }

    for (int r = 0; r < VIS && scroll + r < count; r++) {
        int i = scroll + r;
        int y = LIST_TOP + r * ROW_H;
        bool is_sel = (i == sel), dim = false;
        char idx[8], name[28], detail[16];
        row_cols(i, idx, sizeof(idx), name, sizeof(name),
                 detail, sizeof(detail), &dim);
        if (is_sel) theme_sel_block(c, 0, y, CANVAS_WIDTH, ROW_H - 1);
        color_t nc = is_sel ? THEME_SEL_TEXT : (dim ? THEME_DIM : THEME_TEXT);
        color_t ic = is_sel ? THEME_SEL_TEXT : THEME_LABEL;
        color_t dc = is_sel ? THEME_SEL_TEXT : THEME_DIM;
        if (idx[0]) font_draw_string_right(c, 36, y + 3, &font_small, idx, ic);
        font_draw_string(c, 46, y + 3, &font_small, name, nc);
        if (detail[0])
            font_draw_string_right(c, CANVAS_WIDTH - 10, y + 6, &font_tiny, detail, dc);
    }

    // Scrollbar.
    if (count > VIS) {
        int track = VIS * ROW_H;
        int th = track * VIS / count; if (th < 8) th = 8;
        int ty = LIST_TOP + (track - th) * sel / (count - 1);
        canvas_rect_fill(c, CANVAS_WIDTH - 3, ty, 3, th, THEME_ACCENT);
    }

    theme_keybar(c, chips, 3);
}

static void move(int dir)
{
    int count = list_count();
    if (count == 0) return;
    // Hold-to-scroll: repeated events in one direction ramp the step size, so
    // holding a key sweeps the 114-surah list instead of crawling it.
    int step = input_accel_step(&s_accel, dir, plat_millis());
    int *sel = &s_sel[s_mode], *scroll = &s_scroll[s_mode];
    int was = *sel;
    *sel += dir * step;
    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;
    if (*sel < *scroll) *scroll = *sel;
    if (*sel >= *scroll + VIS) *scroll = *sel - VIS + 1;
    if (*sel != was) hal_audio_click(false);
}

static void enter(void)
{
    hal_audio_click(true);
    int sel = s_sel[s_mode];
    switch (s_mode) {
    case NAV_ROOT:
        switch (sel) {
        case 0: s_mode = NAV_SURAH; break;
        case 1: s_mode = NAV_JUZ; break;
        case 2: s_mode = NAV_PAGE; break;
        case 3: s_mode = NAV_BOOKMARKS; break;
        case 4: scene_switch(SCENE_PROGRESS); return;
        default: scene_switch(SCENE_SETTINGS); return;
        }
        break;
    case NAV_SURAH:
        jump_to(sel + 1, 1);
        break;
    case NAV_JUZ: {
        QRef r = qdb_juz_start(sel + 1);
        jump_to(r.surah, r.ayah);
        break;
    }
    case NAV_PAGE: {
        QRef r = qdb_page_start(sel + 1);
        jump_to(r.surah, r.ayah);
        break;
    }
    case NAV_BOOKMARKS: {
        Bookmark b = progress_bookmark(sel);
        jump_to(b.surah, b.ayah);
        break;
    }
    default: break;
    }
}

static void back(void)
{
    if (s_mode == NAV_ROOT) scene_switch(SCENE_HOME);
    else s_mode = NAV_ROOT;
}

static void on_enter(void) { s_mode = NAV_ROOT; }

static void on_input(InputEvent e)
{
    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:  move(-1); break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:   move(+1); break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH: enter(); break;
    case INPUT_BTN_BACK: back(); break;
    case INPUT_BTN_MENU: scene_switch(SCENE_HOME); break;
    default: break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_render = on_render,
    .on_input = on_input,
};

void scene_nav_register(void) { scene_register(SCENE_NAV, &CB); }
