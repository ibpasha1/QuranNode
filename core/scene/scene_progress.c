// scene_progress.c — the khatm screen: how much of the Quran you have read,
// how you're pacing, and what to read today.
//
// The centrepiece is the coverage map: 604 cells, one per Madani page, each
// filling upward in proportion to how much of that page you've read. It doubles
// as a bar chart of partial pages, so a glance tells you both how far you've
// come and where the gaps are — which a single percentage never could.
//
// Everything here is a read-out of core/quran/khatm.c; this file owns no state
// beyond the cursor and a cache of cell heights.
#include "scene.h"
#include "khatm.h"
#include "progress.h"
#include "player.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

// --- Coverage map geometry ------------------------------------------------
// 31 x 20 = 620 slots for 604 pages (the last row is short). At an 8px cell on
// a 9px pitch that's 278px wide, which centres in 320 with even margins.
#define MAP_COLS  31
#define MAP_ROWS  20
#define MAP_CELL   8
#define MAP_PITCH  9
#define MAP_W     (MAP_COLS * MAP_PITCH - 1)     // 278
#define MAP_X     ((CANVAS_WIDTH - MAP_W) / 2)   // 21
#define MAP_Y     152
#define MAP_H     (MAP_ROWS * MAP_PITCH - 1)     // 179 -> ends at y=331

// Cards below the map.
#define TODAY_Y 338
#define GOAL_Y  386
#define CARD_H  44

typedef enum { VIEW = 0, EDIT_GOAL, CONFIRM_RESET, BACKFILL } Mode;

static Mode s_mode;
static int  s_sel;          // VIEW: 0 = today card, 1 = goal card
static int  s_edit_sel;     // EDIT_GOAL row cursor
static int  s_edit_days;    // duration being dialled in
static int  s_toast;        // frames left on the toast
static const char *s_toast_msg;

// Goal editor scope controls. The scope kind picks which target field is live;
// a whole-Quran goal has none (it's implicitly pages 1..604).
enum { SCOPE_WHOLE = 0, SCOPE_SURAH, SCOPE_JUZ, SCOPE_RANGE };
static int s_edit_scope;
static int s_edit_sur;      // SCOPE_SURAH: 1..114
static int s_edit_juz;      // SCOPE_JUZ:   1..30
static int s_edit_rp0, s_edit_rp1;   // SCOPE_RANGE: page endpoints

// Row layout of the goal editor. It's dynamic because SCOPE_RANGE needs two
// target rows (from/to), the single-index scopes need one, and whole needs none.
typedef struct { int scope, t0, ntarget, dur, save, clear, neww, max; } EditRows;
static EditRows edit_rows(void)
{
    EditRows r;
    r.scope   = 0;
    r.t0      = 1;
    r.ntarget = (s_edit_scope == SCOPE_WHOLE) ? 0
              : (s_edit_scope == SCOPE_RANGE) ? 2 : 1;
    r.dur     = r.t0 + r.ntarget;
    r.save    = r.dur + 1;
    r.clear   = r.save + 1;
    r.neww    = r.clear + 1;
    r.max     = r.neww;
    return r;
}

// Resolve the dialled-in scope to an inclusive page window.
static bool edit_pages(int *p0, int *p1)
{
    switch (s_edit_scope) {
    case SCOPE_SURAH: return khatm_surah_page_range(s_edit_sur, p0, p1);
    case SCOPE_JUZ:   return khatm_juz_page_range(s_edit_juz, p0, p1);
    case SCOPE_RANGE:
        *p0 = s_edit_rp0; *p1 = s_edit_rp1;
        if (*p0 > *p1) { int t = *p0; *p0 = *p1; *p1 = t; }
        return true;
    default: *p0 = 1; *p1 = QDB_PAGE_COUNT; return true;
    }
}

// Seed the editor from the current goal. A stored page window is matched back
// to a juz or surah when it lines up exactly, so re-opening a "Juz 30" goal
// shows "Juz 30" rather than a raw page range.
static void edit_load_scope(void)
{
    s_edit_scope = SCOPE_WHOLE;
    s_edit_sur = 1; s_edit_juz = 1;
    s_edit_rp0 = 1; s_edit_rp1 = QDB_PAGE_COUNT;
    uint32_t res = khatm_goal().reserved;
    if (res == 0) return;                       // whole Quran
    int p0 = (int)(res & 0xFFFFu), p1 = (int)(res >> 16);
    s_edit_rp0 = p0; s_edit_rp1 = p1; s_edit_scope = SCOPE_RANGE;
    for (int j = 1; j <= QDB_JUZ_COUNT; j++) {
        int a, b;
        if (khatm_juz_page_range(j, &a, &b) && a == p0 && b == p1) {
            s_edit_scope = SCOPE_JUZ; s_edit_juz = j; return;
        }
    }
    for (int s = 1; s <= QDB_SURAH_COUNT; s++) {
        int a, b;
        if (khatm_surah_page_range(s, &a, &b) && a == p0 && b == p1) {
            s_edit_scope = SCOPE_SURAH; s_edit_sur = s; return;
        }
    }
}

// Human label for the dialled-in scope, e.g. "Whole Quran" / "Al-Baqarah" /
// "Juz 30" / "Pages 3-9".
static void edit_scope_label(char *buf, int n)
{
    switch (s_edit_scope) {
    case SCOPE_SURAH:
        snprintf(buf, n, "%s", qdb_surah_name(s_edit_sur));
        break;
    case SCOPE_JUZ:
        snprintf(buf, n, "Juz %d", s_edit_juz);
        break;
    case SCOPE_RANGE: {
        int a = s_edit_rp0, b = s_edit_rp1;
        if (a > b) { int t = a; a = b; b = t; }
        if (a == b) snprintf(buf, n, "Page %d", a);
        else        snprintf(buf, n, "Pages %d-%d", a, b);
        break;
    }
    default:
        snprintf(buf, n, "Whole Quran");
        break;
    }
}

// Cell fill heights, recomputed only when coverage actually changes — the map
// is 604 cells and this runs every frame otherwise.
static uint8_t  s_cell_h[QDB_PAGE_COUNT];
static uint32_t s_cell_seq;

static void toast(const char *msg) { s_toast_msg = msg; s_toast = 90; }

static void rebuild_cells(void)
{
    uint32_t seq = khatm_coverage_seq();
    if (seq == s_cell_seq) return;
    s_cell_seq = seq;
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        float f = khatm_page_frac(p);
        int h = (int)(f * MAP_CELL + 0.5f);
        if (f > 0.f && h == 0) h = 1;   // never let a started page read as empty
        s_cell_h[p - 1] = (uint8_t)h;
    }
}

// Should we offer to mark everything before the resume point as already read?
static bool backfill_available(void)
{
    if (khatm_backfill_offered()) return false;
    if (khatm_stats()->ayat_read > 0) return false;
    if (!progress_has_resume()) return false;
    ResumePoint r = progress_resume();
    return qdb_page_of(r.surah, r.ayah) > 1;
}

static int backfill_upto(void)
{
    ResumePoint r = progress_resume();
    int p = qdb_page_of(r.surah, r.ayah);
    return p > 1 ? p - 1 : 0;
}

// "3.5" / "12" — pages with one decimal only when it matters.
static void fmt_pages(uint32_t mpages, char *b, int n)
{
    if (mpages % 1000 == 0) snprintf(b, n, "%u", (unsigned)(mpages / 1000));
    else snprintf(b, n, "%.1f", (double)mpages / 1000.0);
}

static const char *pages_word(uint32_t mpages)
{
    return mpages == 1000 ? "page" : "pages";
}

// -------------------------------------------------------------------------
static void on_enter(void)
{
    s_mode = backfill_available() ? BACKFILL : VIEW;
    s_sel = 0;
    s_toast = 0;
    KhatmGoal g = khatm_goal();
    s_edit_days = g.last_days ? g.last_days : 30;
    s_cell_seq = 0;   // force a rebuild; coverage may have moved since we left
}

static void on_leave(void) { khatm_flush(); }

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (s_toast > 0) s_toast--;
}

// -------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------
static void draw_map(Canvas *c, const KhatmPlan *pl)
{
    rebuild_cells();

    int resume_page = 0;
    if (progress_has_resume()) {
        ResumePoint r = progress_resume();
        resume_page = qdb_page_of(r.surah, r.ayah);
    }

    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        int i = p - 1;
        int x = MAP_X + (i % MAP_COLS) * MAP_PITCH;
        int y = MAP_Y + (i / MAP_COLS) * MAP_PITCH;

        // Pages in today's plan get a lit base so the map answers "where am I
        // heading next" as well as "where have I been".
        bool planned = pl->from_page && p >= pl->from_page && p <= pl->to_page;
        canvas_rect_fill(c, x, y, MAP_CELL, MAP_CELL,
                         planned ? THEME_PLAYHEAD : THEME_GRID);

        int h = s_cell_h[i];
        if (h > 0)
            canvas_rect_fill(c, x, y + MAP_CELL - h, MAP_CELL, h,
                             h >= MAP_CELL ? THEME_ACTIVE : THEME_BAR);
        if (p == resume_page)
            canvas_rect(c, x - 1, y - 1, MAP_CELL + 2, MAP_CELL + 2, THEME_ACCENT);
    }

    // A 1px tick at the left edge of each juz's opening page.
    for (int j = 1; j <= QDB_JUZ_COUNT; j++) {
        int p = qdb_juz_page(j);
        if (p < 1 || p > QDB_PAGE_COUNT) continue;
        int i = p - 1;
        int x = MAP_X + (i % MAP_COLS) * MAP_PITCH;
        int y = MAP_Y + (i / MAP_COLS) * MAP_PITCH;
        canvas_vline(c, x - 1, y, MAP_CELL, THEME_TITLE);
    }
}

static void draw_card(Canvas *c, int y, bool sel)
{
    canvas_rect_fill(c, 10, y, CANVAS_WIDTH - 20, CARD_H,
                     sel ? THEME_ROW : THEME_PANEL);
    canvas_rect(c, 10, y, CANVAS_WIDTH - 20, CARD_H,
                sel ? THEME_ACCENT : THEME_GRID);
}

// Card title for the active goal: "KHATM GOAL" for the whole mushaf, else the
// scope's name (matched back to a juz/surah, or a raw page range).
static void goal_scope_title(const KhatmStats *k, char *buf, int n)
{
    // Whole-mushaf goals keep the generic card label; scoped goals name the
    // window (Juz 30 / Al-Baqarah / Pages 3-9).
    if (k->scope_from_page <= 1 && k->scope_to_page >= QDB_PAGE_COUNT)
        snprintf(buf, n, "KHATM GOAL");
    else
        khatm_scope_name(k->scope_from_page, k->scope_to_page, buf, n);
}

static void render_view(Canvas *c, const KhatmStats *k, const KhatmPlan *pl)
{
    char buf[48], buf2[32];

    // --- header -----------------------------------------------------------
    if (k->have_goal) {
        snprintf(buf, sizeof buf, "DAY %d/%d", k->days_elapsed,
                 k->days_elapsed + k->days_left);
    } else if (!k->have_day) {
        snprintf(buf, sizeof buf, "NO CLOCK");
    } else {
        snprintf(buf, sizeof buf, "NO GOAL");
    }
    theme_header(c, "KHATM", THEME_TITLE, buf,
                 k->have_goal ? THEME_TEXT : THEME_DIM);

    // --- headline: pages read --------------------------------------------
    font_draw_string(c, 12, 20, &font_tiny, "MUSHAF PAGES READ", THEME_LABEL);
    snprintf(buf, sizeof buf, "%d%%", (int)(k->percent + 0.5f));
    font_draw_string_right(c, CANVAS_WIDTH - 12, 20, &font_tiny, buf, THEME_TITLE);

    fmt_pages(k->read_mpages, buf, sizeof buf);
    font_draw_string(c, 12, 30, &font_large, buf, THEME_TEXT);
    int bw = font_string_width(&font_large, buf);
    font_draw_string(c, 12 + bw + 8, 44, &font_small, "/ 604", THEME_DIM);

    if (k->juz_full > 0) {
        snprintf(buf2, sizeof buf2, "%d juz complete", k->juz_full);
        font_draw_string_right(c, CANVAS_WIDTH - 12, 46, &font_tiny, buf2,
                               THEME_LABEL);
    }
    canvas_progress_bar(c, 12, 64, CANVAS_WIDTH - 24, 7, k->percent / 100.f,
                        THEME_ACCENT, THEME_GRID);

    // --- stat rows --------------------------------------------------------
    // With no clock the coverage numbers above are still true, but everything
    // per-day is unknowable — say so rather than printing a confident zero.
    if (!k->have_day) {
        theme_row(c, 80, "Today",  "--", false, true);
        theme_row(c, 100, "Pace",  "--", false, true);
        theme_row(c, 120, "Streak", "--", false, true);
        font_draw_string(c, 12, 136, &font_tiny,
                         "CONNECT WI-FI TO TRACK DAILY GOALS", THEME_DIM);
    } else {
        fmt_pages(k->today_mpages, buf2, sizeof buf2);
        if (k->have_goal) {
            char q[16];
            fmt_pages(k->quota_mpages, q, sizeof q);
            snprintf(buf, sizeof buf, "%s / %s pages", buf2, q);
        } else {
            snprintf(buf, sizeof buf, "%s %s", buf2, pages_word(k->today_mpages));
        }
        theme_row(c, 80, "Today", buf, false, false);

        if (k->eta_day > 0) {
            char eta[16];
            khatm_format_day(k->eta_day, eta, sizeof eta);
            snprintf(buf, sizeof buf, "%.1f/day  ends %s", (double)k->avg, eta);
        } else {
            snprintf(buf, sizeof buf, "%.1f/day", (double)k->avg);
        }
        theme_row(c, 100, "Pace", buf, false, k->avg <= 0.f);

        const char *dw = k->streak == 1 ? "day" : "days";
        if (k->best_streak > k->streak)
            snprintf(buf, sizeof buf, "%d %s  (best %d)", k->streak, dw,
                     k->best_streak);
        else
            snprintf(buf, sizeof buf, "%d %s", k->streak, dw);
        theme_row(c, 120, "Streak", buf, false, k->streak == 0);
    }

    // --- coverage map -----------------------------------------------------
    font_draw_string(c, 12, 142, &font_tiny, "COVERAGE", THEME_LABEL);
    if (k->pages_full > 0) {
        snprintf(buf, sizeof buf, "%d full pages", k->pages_full);
        font_draw_string_right(c, CANVAS_WIDTH - 12, 142, &font_tiny, buf,
                               THEME_DIM);
    }
    draw_map(c, pl);

    // --- today's reading --------------------------------------------------
    draw_card(c, TODAY_Y, s_sel == 0);
    if (k->complete) {
        font_draw_string(c, 22, TODAY_Y + 8, &font_tiny, "KHATM COMPLETE",
                         THEME_ACTIVE);
        snprintf(buf, sizeof buf, "%d completed - press to start another",
                 k->khatms_done);
        font_draw_string(c, 22, TODAY_Y + 24, &font_tiny, buf, THEME_DIM);
    } else if (pl->from_page == 0) {
        font_draw_string(c, 22, TODAY_Y + 8, &font_tiny, "TODAY'S READING",
                         THEME_LABEL);
        font_draw_string(c, 22, TODAY_Y + 22, &font_small, "Nothing queued",
                         THEME_DIM);
    } else {
        font_draw_string(c, 22, TODAY_Y + 8, &font_tiny, "TODAY'S READING",
                         THEME_LABEL);
        fmt_pages(pl->mpages, buf2, sizeof buf2);
        snprintf(buf, sizeof buf, "%s %s", buf2, pages_word(pl->mpages));
        font_draw_string_right(c, CANVAS_WIDTH - 22, TODAY_Y + 8, &font_tiny,
                               buf, THEME_TITLE);
        if (pl->from_page == pl->to_page)
            snprintf(buf, sizeof buf, "Page %d", pl->from_page);
        else
            snprintf(buf, sizeof buf, "Pages %d-%d", pl->from_page, pl->to_page);
        font_draw_string(c, 22, TODAY_Y + 20, &font_small, buf, THEME_TEXT);
        snprintf(buf, sizeof buf, "%s %d:%d", qdb_surah_name(pl->from_surah),
                 pl->from_surah, pl->from_ayah);
        font_draw_string(c, 22, TODAY_Y + 34, &font_tiny, buf, THEME_DIM);
    }

    // --- goal -------------------------------------------------------------
    draw_card(c, GOAL_Y, s_sel == 1);
    if (!k->have_day) {
        font_draw_string(c, 22, GOAL_Y + 8, &font_tiny, "KHATM GOAL", THEME_LABEL);
        font_draw_string(c, 22, GOAL_Y + 22, &font_small, "Needs the clock",
                         THEME_DIM);
    } else if (!k->have_goal) {
        font_draw_string(c, 22, GOAL_Y + 8, &font_tiny, "KHATM GOAL", THEME_LABEL);
        font_draw_string(c, 22, GOAL_Y + 22, &font_small, "Set a finish date",
                         THEME_TEXT);
        font_draw_string_right(c, CANVAS_WIDTH - 22, GOAL_Y + 24, &font_tiny,
                               "PRESS", THEME_ACCENT);
    } else {
        char title[24], when[16];
        goal_scope_title(k, title, sizeof title);
        khatm_format_day(khatm_goal().target_day, when, sizeof when);
        font_draw_string(c, 22, GOAL_Y + 6, &font_tiny, title, THEME_LABEL);
        if (k->overdue) {
            snprintf(buf, sizeof buf, "Due %s - overdue", when);
            font_draw_string(c, 22, GOAL_Y + 20, &font_small, buf, THEME_BADGE);
            font_draw_string(c, 22, GOAL_Y + 34, &font_tiny,
                             "Press to extend or re-plan", THEME_DIM);
        } else {
            // Scope progress: percent by the title, then a thin bar — the second
            // "both" bar, scoped to the goal rather than the whole mushaf above.
            snprintf(buf, sizeof buf, "%d%%", (int)(k->scope_percent + 0.5f));
            font_draw_string_right(c, CANVAS_WIDTH - 22, GOAL_Y + 6, &font_tiny,
                                   buf, THEME_TITLE);
            theme_bar(c, 22, GOAL_Y + 18, CANVAS_WIDTH - 44, 4,
                      k->scope_percent / 100.f, false);

            snprintf(buf, sizeof buf, "%s - %d left", when, k->days_left);
            font_draw_string(c, 22, GOAL_Y + 27, &font_tiny, buf, THEME_TEXT);
            // Ahead/behind against the original straight-line plan (within scope).
            int d = k->delta_mpages;
            if (d >= 1000 || d <= -1000) {
                fmt_pages((uint32_t)(d < 0 ? -d : d), buf2, sizeof buf2);
                snprintf(buf, sizeof buf, "%s %s", buf2, d > 0 ? "ahead" : "behind");
                font_draw_string_right(c, CANVAS_WIDTH - 22, GOAL_Y + 27,
                                       &font_tiny, buf,
                                       d > 0 ? THEME_ACTIVE : THEME_BADGE);
            } else {
                font_draw_string_right(c, CANVAS_WIDTH - 22, GOAL_Y + 27,
                                       &font_tiny, "on track", THEME_ACTIVE);
            }
        }
    }
}

static void render_edit(Canvas *c, const KhatmStats *k)
{
    (void)k;
    char buf[48], lbl[24];
    theme_header(c, "KHATM GOAL", THEME_TITLE, NULL, THEME_DIM);

    font_draw_string(c, 12, 26, &font_tiny, "WHAT TO READ, AND BY WHEN", THEME_LABEL);

    EditRows r = edit_rows();
    const int y0 = 40, dy = 22;

    // Scope kind.
    edit_scope_label(lbl, sizeof lbl);
    theme_row(c, y0 + r.scope * dy, "Scope", lbl, s_edit_sel == r.scope, false);

    // Target field(s) for the chosen kind.
    if (s_edit_scope == SCOPE_SURAH) {
        snprintf(buf, sizeof buf, "%d. %s", s_edit_sur, qdb_surah_name(s_edit_sur));
        theme_row(c, y0 + r.t0 * dy, "Surah", buf, s_edit_sel == r.t0, false);
    } else if (s_edit_scope == SCOPE_JUZ) {
        snprintf(buf, sizeof buf, "%d", s_edit_juz);
        theme_row(c, y0 + r.t0 * dy, "Juz", buf, s_edit_sel == r.t0, false);
    } else if (s_edit_scope == SCOPE_RANGE) {
        snprintf(buf, sizeof buf, "%d", s_edit_rp0);
        theme_row(c, y0 + r.t0 * dy, "From page", buf, s_edit_sel == r.t0, false);
        snprintf(buf, sizeof buf, "%d", s_edit_rp1);
        theme_row(c, y0 + (r.t0 + 1) * dy, "To page", buf, s_edit_sel == r.t0 + 1,
                  false);
    }

    // Duration.
    snprintf(buf, sizeof buf, "%d days", s_edit_days);
    theme_row(c, y0 + r.dur * dy, "Duration", buf, s_edit_sel == r.dur, false);

    // Actions.
    theme_row(c, y0 + r.save * dy, "Save goal", "", s_edit_sel == r.save, false);
    theme_row(c, y0 + r.clear * dy, "Clear goal", "", s_edit_sel == r.clear, false);
    theme_row(c, y0 + r.neww * dy, "Start a new khatm", "",
              s_edit_sel == r.neww, false);

    // The daily cost of the dialled-in scope + deadline — the number people
    // actually feel is pages/day, computed over the chosen window's remainder.
    int p0, p1;
    bool ok = edit_pages(&p0, &p1);
    uint32_t span = ok ? (uint32_t)(p1 - p0 + 1) * 1000u : 0;
    uint32_t already = ok ? khatm_scope_read_mpages(p0, p1) : 0;
    uint32_t rem = span > already ? span - already : 0;
    int yr = y0 + (r.max + 1) * dy + 6;
    snprintf(buf, sizeof buf, "%.1f pages/day",
             (double)rem / 1000.0 / (double)(s_edit_days > 0 ? s_edit_days : 1));
    font_draw_string(c, 12, yr, &font_tiny, buf, THEME_LABEL);
    char when[16];
    khatm_format_day(khatm_today() + s_edit_days - 1, when, sizeof when);
    snprintf(buf, sizeof buf, "Finish %s", when);
    font_draw_string_right(c, CANVAS_WIDTH - 12, yr, &font_tiny, buf, THEME_DIM);

    font_draw_string(c, 12, yr + 14, &font_tiny,
                     "A new khatm keeps your streak, clears pages.", THEME_DIM);

    KeyChip chips[3] = {
        { "^v", "MOVE", 2, { INPUT_NAV_UP, INPUT_NAV_DOWN } },
        { "<>", "ADJUST", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT,
                               INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "APPLY", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
    };
    theme_keybar(c, chips, 3);
}

static void render_confirm(Canvas *c, const char *title, const char *line1,
                           const char *line2, const char *yes)
{
    theme_header(c, title, THEME_TITLE, NULL, THEME_DIM);
    int y = CANVAS_HEIGHT / 2 - 40;
    canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 80, THEME_PANEL);
    canvas_rect(c, 20, y, CANVAS_WIDTH - 40, 80, THEME_ACCENT);
    font_draw_string_centered(c, y + 16, &font_small, line1, THEME_TEXT);
    font_draw_string_centered(c, y + 38, &font_tiny, line2, THEME_DIM);
    font_draw_string_centered(c, y + 58, &font_tiny, yes, THEME_ACCENT);

    KeyChip chips[2] = {
        { "OK", "CONFIRM", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "BK", "CANCEL", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, chips, 2);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    const KhatmStats *k = khatm_stats();
    const KhatmPlan  *pl = khatm_today_plan();

    if (s_mode == EDIT_GOAL) { render_edit(c, k); return; }
    if (s_mode == CONFIRM_RESET) {
        render_confirm(c, "NEW KHATM", "Start a new khatm?",
                       "Pages read resets to zero.",
                       "Streak and history are kept.");
        return;
    }
    if (s_mode == BACKFILL) {
        char l1[48], l2[48];
        int upto = backfill_upto();
        ResumePoint r = progress_resume();
        snprintf(l1, sizeof l1, "You're on page %d.",
                 qdb_page_of(r.surah, r.ayah));
        snprintf(l2, sizeof l2, "Mark pages 1-%d as already read?", upto);
        render_confirm(c, "KHATM", l1, l2, "BACK to start from zero");
        return;
    }

    render_view(c, k, pl);

    if (s_toast > 0 && s_toast_msg) {
        int tw = font_string_width(&font_tiny, s_toast_msg) + 16;
        int tx = (CANVAS_WIDTH - tw) / 2, ty = CANVAS_HEIGHT - THEME_KEYBAR_H - 20;
        canvas_rect_fill(c, tx, ty, tw, 14, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 14, THEME_ACCENT);
        font_draw_string_centered(c, ty + 4, &font_tiny, s_toast_msg, THEME_ACCENT);
    }

    KeyChip chips[3] = {
        { "^v", "MOVE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                             INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", s_sel == 0 ? "READ" : "GOAL", 2,
          { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "BK", "HOME", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, chips, 3);
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void start_today(void)
{
    const KhatmPlan *pl = khatm_today_plan();
    if (khatm_stats()->complete) { s_mode = CONFIRM_RESET; return; }
    if (!pl->from_page) { toast("Nothing to read"); return; }
    player_load(&g_player, pl->from_surah, pl->from_ayah);
    scene_switch(SCENE_READER);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Left/right on the selected editor row.
static void edit_adjust(int dir)
{
    EditRows r = edit_rows();
    int sel = s_edit_sel;
    if (sel == r.scope) {
        s_edit_scope += dir;                 // cycle the four scope kinds
        if (s_edit_scope < SCOPE_WHOLE) s_edit_scope = SCOPE_RANGE;
        if (s_edit_scope > SCOPE_RANGE) s_edit_scope = SCOPE_WHOLE;
        EditRows nr = edit_rows();           // row count may have shrunk
        if (s_edit_sel > nr.max) s_edit_sel = nr.max;
    } else if (r.ntarget >= 1 && sel == r.t0) {
        if (s_edit_scope == SCOPE_SURAH)
            s_edit_sur = clampi(s_edit_sur + dir, 1, QDB_SURAH_COUNT);
        else if (s_edit_scope == SCOPE_JUZ)
            s_edit_juz = clampi(s_edit_juz + dir, 1, QDB_JUZ_COUNT);
        else if (s_edit_scope == SCOPE_RANGE)
            s_edit_rp0 = clampi(s_edit_rp0 + dir, 1, QDB_PAGE_COUNT);
    } else if (r.ntarget == 2 && sel == r.t0 + 1) {
        s_edit_rp1 = clampi(s_edit_rp1 + dir, 1, QDB_PAGE_COUNT);
    } else if (sel == r.dur) {
        s_edit_days = clampi(s_edit_days + dir, 1, 365);
    } else {
        return;   // action rows have nothing to dial
    }
    hal_audio_click(false);
}

static void edit_activate(void)
{
    EditRows r = edit_rows();
    if (s_edit_sel == r.clear) {
        khatm_clear_goal();
        s_mode = VIEW;
        toast("Goal cleared");
    } else if (s_edit_sel == r.neww) {
        s_mode = CONFIRM_RESET;
    } else {
        // Every other row — scope, target, duration, Save — commits the goal;
        // OK on an adjustable row doubling as Save is the obvious action.
        int p0, p1;
        if (!edit_pages(&p0, &p1)) { toast("Invalid scope"); return; }
        khatm_set_goal_pages(p0, p1, s_edit_days);
        s_mode = VIEW;
        toast("Goal set");
    }
}

static void on_input(InputEvent e)
{
    // --- confirmation overlays -------------------------------------------
    if (s_mode == CONFIRM_RESET) {
        if (e.type == INPUT_NAV_SELECT || e.type == INPUT_ENC_PUSH) {
            hal_audio_click(true);
            khatm_reset_coverage();
            s_mode = VIEW;
            toast("New khatm started");
        } else if (e.type == INPUT_BTN_BACK) {
            s_mode = VIEW;
        }
        return;
    }
    if (s_mode == BACKFILL) {
        if (e.type == INPUT_NAV_SELECT || e.type == INPUT_ENC_PUSH) {
            hal_audio_click(true);
            khatm_mark_pages(1, backfill_upto());
            khatm_set_backfill_offered();
            s_mode = VIEW;
            toast("Earlier pages marked read");
        } else if (e.type == INPUT_BTN_BACK) {
            khatm_set_backfill_offered();   // asked once, never again
            s_mode = VIEW;
        }
        return;
    }

    // --- goal editor ------------------------------------------------------
    if (s_mode == EDIT_GOAL) {
        EditRows r = edit_rows();
        switch (e.type) {
        case INPUT_NAV_UP:
            if (s_edit_sel > 0) { s_edit_sel--; hal_audio_click(false); }
            break;
        case INPUT_NAV_DOWN:
            if (s_edit_sel < r.max) { s_edit_sel++; hal_audio_click(false); }
            break;
        case INPUT_NAV_LEFT:
        case INPUT_ENC_CCW:
            edit_adjust(-1);
            break;
        case INPUT_NAV_RIGHT:
        case INPUT_ENC_CW:
            edit_adjust(+1);
            break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
            hal_audio_click(true);
            edit_activate();
            break;
        case INPUT_BTN_BACK:
            s_mode = VIEW;
            break;
        default: break;
        }
        return;
    }

    // --- main view --------------------------------------------------------
    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:
        if (s_sel > 0) { s_sel--; hal_audio_click(false); }
        break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:
        if (s_sel < 1) { s_sel++; hal_audio_click(false); }
        break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        hal_audio_click(true);
        if (s_sel == 0) {
            start_today();
        } else if (!khatm_stats()->have_day) {
            toast("Clock not set yet");
        } else {
            KhatmGoal g = khatm_goal();
            s_edit_days = g.last_days ? g.last_days : 30;
            edit_load_scope();
            s_edit_sel = 0;
            s_mode = EDIT_GOAL;
        }
        break;
    case INPUT_BTN_PLAY:
        start_today();
        break;
    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        khatm_flush();
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

void scene_progress_register(void) { scene_register(SCENE_PROGRESS, &CB); }
