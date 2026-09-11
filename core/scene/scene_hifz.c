// scene_hifz.c — Lessons: today's memorization plan.
//
// Three streams, the classical way: sabaq (new), sabqi (recent review), manzil
// (older review on a cycle). The screen's job is to say what to do today, in
// what order, and to refuse politely when new material would be a mistake.
//
// All the judgement lives in core/quran/hifz.c; this file only draws it and
// takes grades. Grading from the list is deliberately a first-class path, not
// a fallback — the drill (H3) is a better way to earn a grade, but a hafiz
// reviewing from memory on a walk should be able to just tap.
#include "scene.h"
#include "hifz.h"
#include "qday.h"
#include "player.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>

// The drill is a separate scene; it takes a portion and grades it on return.
void scene_hifz_drill_set_portion(int portion);

typedef enum { VIEW = 0, PICK_TARGET, GRADE, MAP } Mode;

// Memorization heat map: the same 604-cell mushaf grid the khatm screen uses,
// keyed on strength instead of reading coverage. Geometry mirrors scene_progress.
#define MAP_COLS  31
#define MAP_CELL   8
#define MAP_PITCH  9
#define MAP_W     (MAP_COLS * MAP_PITCH - 1)
#define MAP_X     ((CANVAS_WIDTH - MAP_W) / 2)
#define MAP_Y     40
#define MAP_WEAK_MAX 8

static Mode s_mode;
static int  s_sel;          // index into the flattened task list
static int  s_grade_sel;    // 0 = got it, 1 = shaky, 2 = no
static int  s_grade_task;   // task being graded
static int  s_toast;
static const char *s_toast_msg;

// Target picker
static int s_pick_kind;     // 0 = juz, 1 = surah
static int s_pick_arg = 30;
static int s_pick_rev = 1;
static int s_pick_row;      // 0 kind, 1 which, 2 direction, 3 confirm
static Mode s_pick_ret = VIEW;  // where BACK returns to (VIEW or MAP)

#define LIST_TOP 132
#define LIST_BOT 384
#define ROW_H    30
#define ROW_GAP   4

// A flattened view of the plan so one cursor walks all three tiers.
typedef struct { int8_t tier; int16_t portion; uint8_t overdue; } Row;
static Row s_rows[1 + HIFZ_PLAN_MAX * 2];
static int s_nrows;

// Heat-map cell heights + per-page weak flag + weak list, rebuilt only when
// hifz state changes. s_map_weak mirrors hifz_page_has_weak() so render_map
// doesn't re-scan every ayah on all 604 pages every frame (~6k byte checks).
static uint8_t  s_map_h[QDB_PAGE_COUNT];
static uint8_t  s_map_weak[QDB_PAGE_COUNT];
static uint32_t s_map_seq = 0;
static HifzWeakAyah s_weak[MAP_WEAK_MAX];
static int s_nweak;
static int s_weak_sel;

static void toast(const char *m) { s_toast_msg = m; s_toast = 90; }

static void rebuild_map(void)
{
    uint32_t seq = hifz_state_seq();
    if (seq == s_map_seq) return;
    s_map_seq = seq;
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        float f = hifz_page_frac(p);
        int h = (int)(f * MAP_CELL + 0.5f);
        if (f > 0.f && h == 0) h = 1;
        s_map_h[p - 1] = (uint8_t)h;
        s_map_weak[p - 1] = hifz_page_has_weak(p) ? 1 : 0;
    }
    s_nweak = hifz_weak_ayat(s_weak, MAP_WEAK_MAX);
    if (s_weak_sel >= s_nweak) s_weak_sel = s_nweak > 0 ? s_nweak - 1 : 0;
}

static void rebuild_rows(void)
{
    const HifzPlan *pl = hifz_plan();
    s_nrows = 0;
    if (pl->n_sabaq) {
        s_rows[s_nrows].tier = HZ_SABAQ;
        s_rows[s_nrows].portion = pl->sabaq.portion;
        s_rows[s_nrows].overdue = pl->sabaq.overdue;
        s_nrows++;
    }
    for (int i = 0; i < pl->n_sabqi; i++) {
        s_rows[s_nrows].tier = HZ_SABQI;
        s_rows[s_nrows].portion = pl->sabqi[i].portion;
        s_rows[s_nrows].overdue = pl->sabqi[i].overdue;
        s_nrows++;
    }
    for (int i = 0; i < pl->n_manzil; i++) {
        s_rows[s_nrows].tier = HZ_MANZIL;
        s_rows[s_nrows].portion = pl->manzil[i].portion;
        s_rows[s_nrows].overdue = pl->manzil[i].overdue;
        s_nrows++;
    }
    if (s_sel >= s_nrows) s_sel = s_nrows > 0 ? s_nrows - 1 : 0;
}

static const char *tier_name(int t)
{
    switch (t) {
    case HZ_SABAQ:  return "SABAQ";
    case HZ_SABQI:  return "SABQI";
    case HZ_MANZIL: return "MANZIL";
    default:        return "";
    }
}

static color_t tier_color(int t)
{
    switch (t) {
    case HZ_SABAQ:  return THEME_ACCENT;
    case HZ_SABQI:  return THEME_ACTIVE;
    default:        return THEME_LABEL;
    }
}

static void on_enter(void)
{
    s_mode = hifz_scope().kind == HZ_SCOPE_NONE ? PICK_TARGET : VIEW;
    s_sel = 0;
    s_toast = 0;
    rebuild_rows();
}

static void on_leave(void) { hifz_flush(); }

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (s_toast > 0) s_toast--;
}

// -------------------------------------------------------------------------
// Views
// -------------------------------------------------------------------------
static void render_hero(Canvas *c, const HifzStats *k)
{
    const int hx = 10, hy = 20, hw = CANVAS_WIDTH - 20, hh = 76;
    canvas_rect_fill(c, hx, hy, hw, hh, THEME_PANEL);
    canvas_rect(c, hx, hy, hw, hh, THEME_GRID);

    char label[32];
    hifz_scope_label(label, sizeof label);
    font_draw_string(c, hx + 10, hy + 8, &font_medium, label, THEME_TITLE);

    HifzScope sc = hifz_scope();
    if (sc.reverse)
        font_draw_string_right(c, hx + hw - 10, hy + 12, &font_tiny,
                               "REVERSE", THEME_LABEL);

    canvas_progress_bar(c, hx + 10, hy + 36, hw - 20, 6, k->scope_frac,
                        THEME_ACCENT, THEME_GRID);

    char sub[44];
    snprintf(sub, sizeof sub, "%d / %d ayat  %d%%", k->scope_done, k->scope_total,
             (int)(k->scope_frac * 100.f + 0.5f));
    font_draw_string(c, hx + 10, hy + 50, &font_tiny, sub, THEME_TEXT);

    char right[32];
    if (k->have_day && k->streak > 0)
        snprintf(right, sizeof right, "%d portions  %d day%s", k->portions,
                 k->streak, k->streak == 1 ? "" : "s");
    else
        snprintf(right, sizeof right, "%d portions", k->portions);
    font_draw_string_right(c, hx + hw - 10, hy + 50, &font_tiny, right, THEME_DIM);
}

static void render_tier_strip(Canvas *c, const HifzPlan *pl)
{
    const int y = 102, h = 22, w = (CANVAS_WIDTH - 20) / 3;
    struct { const char *n; int v; int tier; } cells[3] = {
        { "SABAQ",  pl->n_sabaq,  HZ_SABAQ  },
        { "SABQI",  pl->n_sabqi,  HZ_SABQI  },
        { "MANZIL", pl->n_manzil, HZ_MANZIL },
    };
    for (int i = 0; i < 3; i++) {
        int x = 10 + i * w;
        canvas_rect_fill(c, x, y, w - 3, h, THEME_PANEL);
        canvas_rect(c, x, y, w - 3, h, THEME_GRID);
        char s[20];
        snprintf(s, sizeof s, "%s %d", cells[i].n, cells[i].v);
        int tx = x + (w - 3 - font_string_width(&font_tiny, s)) / 2;
        font_draw_string(c, tx, y + 8, &font_tiny, s,
                         cells[i].v ? tier_color(cells[i].tier) : THEME_DIM);
    }
}

static void render_list(Canvas *c)
{
    if (s_nrows == 0) {
        const char *msg = hifz_plan()->sabaq_blocked
                        ? "Review is caught up tomorrow"
                        : "Nothing due — well done";
        font_draw_string_centered(c, LIST_TOP + 40, &font_small, msg, THEME_DIM);
        return;
    }

    // Scroll so the cursor stays on screen. Budget for up to three section
    // headers, since they eat height the row pitch doesn't account for.
    int vis = (LIST_BOT - LIST_TOP - 3 * 11) / (ROW_H + ROW_GAP);
    if (vis < 1) vis = 1;
    int scroll = 0;
    if (s_sel >= vis) scroll = s_sel - vis + 1;

    int last_tier = -1;
    int y = LIST_TOP;
    for (int i = scroll; i < s_nrows && y + ROW_H <= LIST_BOT; i++) {
        const Row *r = &s_rows[i];
        const HifzPortion *p = hifz_portion(r->portion);
        if (!p) continue;
        bool sel = (i == s_sel);

        if (r->tier != last_tier) {
            // Only start a section if a row of it will actually fit — a header
            // stranded at the bottom with nothing under it reads as a bug.
            if (y + 11 + ROW_H > LIST_BOT) break;
            last_tier = r->tier;
            font_draw_string(c, 12, y, &font_tiny, tier_name(r->tier),
                             tier_color(r->tier));
            y += 11;
        }

        if (sel) theme_sel_block(c, 10, y, CANVAS_WIDTH - 20, ROW_H);
        else {
            canvas_rect_fill(c, 10, y, CANVAS_WIDTH - 20, ROW_H, THEME_PANEL);
            canvas_rect(c, 10, y, CANVAS_WIDTH - 20, ROW_H, THEME_GRID);
        }
        color_t fg = sel ? THEME_SEL_TEXT : THEME_TEXT;

        char ref[24];
        hifz_portion_label(r->portion, ref, sizeof ref);
        font_draw_string(c, 20, y + (ROW_H - 14) / 2, &font_small, ref, fg);

        // Right side: how solid it is, and how late.
        char det[24];
        if (r->overdue > 0)
            snprintf(det, sizeof det, "%dd late", r->overdue);
        else if (r->tier == HZ_SABAQ)
            snprintf(det, sizeof det, "new");
        else
            snprintf(det, sizeof det, "box %d", p->box);
        color_t dc = sel ? THEME_SEL_TEXT : (r->overdue ? THEME_BADGE : THEME_DIM);
        font_draw_string_right(c, CANVAS_WIDTH - 20, y + (ROW_H - 7) / 2,
                               &font_tiny, det, dc);
        y += ROW_H + ROW_GAP;
    }
}

static void render_view(Canvas *c)
{
    const HifzStats *k = hifz_stats();
    const HifzPlan  *pl = hifz_plan();

    char right[16];
    if (pl->have_day) qday_format(pl->today, right, sizeof right);
    else              snprintf(right, sizeof right, "--");
    theme_header(c, "LESSONS", THEME_TITLE, right,
                 pl->have_day ? THEME_TEXT : THEME_DIM);

    render_hero(c, k);
    render_tier_strip(c, pl);
    render_list(c);

    // --- footer: today's progress + why new material may be withheld ------
    const int fy = 392;
    canvas_rect_fill(c, 10, fy, CANVAS_WIDTH - 20, 30, THEME_PANEL);
    canvas_rect(c, 10, fy, CANVAS_WIDTH - 20, 30, THEME_GRID);
    char done[32];
    snprintf(done, sizeof done, "%d of %d done today", pl->done_today,
             pl->total_today);
    font_draw_string(c, 20, fy + 5, &font_tiny, done, THEME_LABEL);
    if (pl->total_today > 0) {
        float f = (float)pl->done_today / (float)pl->total_today;
        canvas_progress_bar(c, 20, fy + 18, CANVAS_WIDTH - 40, 5, f,
                            THEME_ACTIVE, THEME_GRID);
    }

    const int sy = 428;
    if (pl->sabaq_blocked) {
        // The single most important message this screen can show.
        font_draw_string_centered(c, sy, &font_tiny,
                                  "Finish review before starting new material",
                                  THEME_BADGE);
    } else if (!pl->have_day) {
        font_draw_string_centered(c, sy, &font_tiny,
                                  "No date - plan is in cycle order", THEME_BADGE);
    } else if (pl->manzil_backlog > 0) {
        char b[40];
        snprintf(b, sizeof b, "%d more manzil waiting after today",
                 pl->manzil_backlog);
        font_draw_string_centered(c, sy, &font_tiny, b, THEME_DIM);
    } else if (pl->new_available && pl->n_sabaq == 0) {
        font_draw_string_centered(c, sy, &font_tiny,
                                  "Press NEW to start today's portion", THEME_ACCENT);
    } else {
        font_draw_string_centered(c, sy, &font_tiny,
                                  "Press > for your memorization map", THEME_DIM);
    }

    if (s_toast > 0 && s_toast_msg) {
        int tw = font_string_width(&font_tiny, s_toast_msg) + 16;
        int tx = (CANVAS_WIDTH - tw) / 2, ty = CANVAS_HEIGHT - THEME_KEYBAR_H - 20;
        canvas_rect_fill(c, tx, ty, tw, 14, THEME_PANEL);
        canvas_rect(c, tx, ty, tw, 14, THEME_ACCENT);
        font_draw_string_centered(c, ty + 4, &font_tiny, s_toast_msg, THEME_ACCENT);
    }

    KeyChip chips[6] = {
        { "^v", "PICK", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                             INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", s_nrows ? "DRILL" : "NEW", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "<",  s_nrows ? "GRADE" : "", s_nrows ? 1 : 0, { INPUT_NAV_LEFT } },
        { ">",  "MAP", 1, { INPUT_NAV_RIGHT } },
        { "MD", "MEANING", 1, { INPUT_BTN_MODE } },
        { "BK", "HOME", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, chips, 6);
}

// --- grading overlay ------------------------------------------------------
static void render_grade(Canvas *c)
{
    theme_header(c, "HOW DID IT GO?", THEME_TITLE, NULL, THEME_DIM);

    char ref[24];
    hifz_portion_label(s_rows[s_grade_task].portion, ref, sizeof ref);
    font_draw_string_centered(c, 60, &font_medium, ref, THEME_TEXT);
    font_draw_string_centered(c, 92, &font_tiny,
                              "Grade it from memory, not from the page",
                              THEME_DIM);

    static const char *L[3] = { "GOT IT", "SHAKY", "NO" };
    static const char *H[3] = { "clean, no hesitation",
                                "got there, but rough",
                                "blanked or needed the text" };
    for (int i = 0; i < 3; i++) {
        int y = 140 + i * 62;
        bool sel = (i == s_grade_sel);
        if (sel) theme_sel_block(c, 20, y, CANVAS_WIDTH - 40, 44);
        else {
            canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 44, THEME_PANEL);
            canvas_rect(c, 20, y, CANVAS_WIDTH - 40, 44, THEME_GRID);
        }
        color_t fg = sel ? THEME_SEL_TEXT : THEME_TEXT;
        font_draw_string(c, 34, y + 8, &font_small, L[i], fg);
        font_draw_string(c, 34, y + 28, &font_tiny, H[i],
                         sel ? THEME_SEL_TEXT : THEME_DIM);
    }

    KeyChip chips[2] = {
        { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                               INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "CONFIRM", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
    };
    theme_keybar(c, chips, 2);
}

// --- target picker --------------------------------------------------------
static void render_pick(Canvas *c)
{
    theme_header(c, "WHAT TO MEMORIZE", THEME_TITLE, NULL, THEME_DIM);

    char v[32];
    theme_row(c, 40, "Type", s_pick_kind == 0 ? "Juz" : "Surah",
              s_pick_row == 0, false);

    if (s_pick_kind == 0) snprintf(v, sizeof v, "Juz %d", s_pick_arg);
    else snprintf(v, sizeof v, "%s", qdb_surah_name(s_pick_arg));
    theme_row(c, 68, "Which", v, s_pick_row == 1, false);

    theme_row(c, 96, "Order", s_pick_rev ? "Last to first" : "First to last",
              s_pick_row == 2, false);

    theme_row(c, 130, "Start", "", s_pick_row == 3, false);

    // Explain the default rather than leaving it mysterious: nearly everyone
    // memorizing Juz Amma starts at An-Nas because the short surahs are there.
    font_draw_string(c, 12, 170, &font_tiny,
                     "Last to first is the usual way through Juz 30 --",
                     THEME_DIM);
    font_draw_string(c, 12, 182, &font_tiny,
                     "the short surahs come first that way.", THEME_DIM);

    int total = 0;
    if (s_pick_kind == 0 && s_pick_arg >= 1 && s_pick_arg <= QDB_JUZ_COUNT) {
        QRef a = qdb_juz_start(s_pick_arg);
        int g0 = qdb_global_index(a.surah, a.ayah), g1;
        if (s_pick_arg == QDB_JUZ_COUNT) g1 = QDB_AYAH_TOTAL;
        else { QRef b = qdb_juz_start(s_pick_arg + 1);
               g1 = qdb_global_index(b.surah, b.ayah) - 1; }
        total = g1 - g0 + 1;
    } else if (s_pick_kind == 1) {
        total = qdb_ayah_count(s_pick_arg);
    }
    if (total > 0) {
        char t[48];
        int per = hifz_cfg_new_words();
        snprintf(t, sizeof t, "%d ayat  ~%d words a day", total, per);
        font_draw_string(c, 12, 206, &font_tiny, t, THEME_LABEL);
    }

    KeyChip chips[3] = {
        { "^v", "ROW", 2, { INPUT_NAV_UP, INPUT_NAV_DOWN } },
        { "<>", "CHANGE", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT,
                               INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "SET", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
    };
    theme_keybar(c, chips, 3);
}

// --- memorization heat map + weak spots -----------------------------------
static void render_map(Canvas *c)
{
    theme_header(c, "MEMORIZED", THEME_TITLE, NULL, THEME_DIM);
    rebuild_map();

    // 604-cell grid, each page filling upward by memorized fraction; a page
    // holding a lapsed ayah gets a coral cap so weak spots jump out.
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        int i = p - 1;
        int x = MAP_X + (i % MAP_COLS) * MAP_PITCH;
        int y = MAP_Y + (i / MAP_COLS) * MAP_PITCH;
        canvas_rect_fill(c, x, y, MAP_CELL, MAP_CELL, THEME_GRID);
        int h = s_map_h[i];
        if (h > 0)
            canvas_rect_fill(c, x, y + MAP_CELL - h, MAP_CELL, h,
                             h >= MAP_CELL ? THEME_ACTIVE : THEME_BAR);
        if (s_map_weak[i])
            canvas_hline(c, x, y, MAP_CELL, THEME_BADGE);
    }

    const HifzStats *k = hifz_stats();
    int gy = MAP_Y + 20 * MAP_PITCH + 6;
    char s[40];
    snprintf(s, sizeof s, "%d ayat memorized", k->memorized_ayat);
    font_draw_string(c, 12, gy, &font_tiny, s, THEME_LABEL);
    font_draw_string_right(c, CANVAS_WIDTH - 12, gy, &font_tiny,
                           "coral = review", THEME_BADGE);

    int ly = gy + 14;
    font_draw_string(c, 12, ly, &font_tiny, "WEAK SPOTS", THEME_LABEL);
    ly += 12;
    if (s_nweak == 0) {
        font_draw_string_centered(c, ly + 24, &font_small,
            hifz_scope().kind == HZ_SCOPE_NONE ? "Set a target first"
                                               : "Nothing weak - masha'Allah",
            THEME_DIM);
    } else {
        for (int i = 0; i < s_nweak; i++) {
            int ry = ly + i * 22;
            bool sel = (i == s_weak_sel);
            if (sel) theme_sel_block(c, 10, ry, CANVAS_WIDTH - 20, 20);
            else {
                canvas_rect_fill(c, 10, ry, CANVAS_WIDTH - 20, 20, THEME_PANEL);
                canvas_rect(c, 10, ry, CANVAS_WIDTH - 20, 20, THEME_GRID);
            }
            color_t fg = sel ? THEME_SEL_TEXT : THEME_TEXT;
            char ref[16];
            snprintf(ref, sizeof ref, "%d:%d", s_weak[i].surah, s_weak[i].ayah);
            font_draw_string(c, 20, ry + 3, &font_small, ref, fg);
            font_draw_string_right(c, CANVAS_WIDTH - 20, ry + 5, &font_tiny,
                                   s_weak[i].lapsed ? "lapsed" : "shaky",
                                   sel ? THEME_SEL_TEXT
                                       : (s_weak[i].lapsed ? THEME_BADGE : THEME_DIM));
        }
    }

    KeyChip chips[4] = {
        { "^v", "SPOT", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                             INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "DRILL", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { ">",  "TARGET", 1, { INPUT_NAV_RIGHT } },
        { "BK", "BACK", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, chips, 4);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    if (s_mode != MAP) rebuild_rows();
    switch (s_mode) {
    case PICK_TARGET: render_pick(c); break;
    case GRADE:       render_grade(c); break;
    case MAP:         render_map(c); break;
    default:          render_view(c); break;
    }
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void commit_grade(void)
{
    static const HifzGrade G[3] = { HZ_GOT, HZ_SHAKY, HZ_NO };
    int portion = s_rows[s_grade_task].portion;
    hifz_grade(portion, G[s_grade_sel], 0);
    s_mode = VIEW;
    rebuild_rows();
    toast(s_grade_sel == 0 ? "Well done" :
          s_grade_sel == 1 ? "Marked shaky - back tomorrow"
                           : "Back in today's review");
}

static void pick_adjust(int dir)
{
    switch (s_pick_row) {
    case 0:
        s_pick_kind = (s_pick_kind + (dir > 0 ? 1 : 1)) % 2;
        s_pick_arg = s_pick_kind == 0 ? 30 : 114;
        break;
    case 1: {
        int max = s_pick_kind == 0 ? QDB_JUZ_COUNT : QDB_SURAH_COUNT;
        s_pick_arg += dir;
        if (s_pick_arg < 1) s_pick_arg = max;
        if (s_pick_arg > max) s_pick_arg = 1;
        break;
    }
    case 2: s_pick_rev = !s_pick_rev; break;
    default: break;
    }
}

static void on_input(InputEvent e)
{
    if (s_mode == MAP) {
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_weak_sel > 0) { s_weak_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_weak_sel < s_nweak - 1) { s_weak_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
            // Drill the portion that covers the selected weak ayah.
            if (s_nweak > 0) {
                int g = qdb_global_index(s_weak[s_weak_sel].surah, s_weak[s_weak_sel].ayah);
                int pi = hifz_portion_at(g);
                if (pi >= 0) {
                    hal_audio_click(true);
                    scene_hifz_drill_set_portion(pi);
                    scene_switch(SCENE_HIFZ_DRILL);
                } else {
                    toast("No portion covers that ayah");
                }
            }
            break;
        case INPUT_NAV_RIGHT:
            // Change what you're memorizing — BACK returns here, not the plan.
            hal_audio_click(true);
            s_pick_row = 0;
            s_pick_ret = MAP;
            s_mode = PICK_TARGET;
            break;
        case INPUT_BTN_BACK: s_mode = VIEW; break;
        default: break;
        }
        return;
    }

    if (s_mode == GRADE) {
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_grade_sel > 0) { s_grade_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_grade_sel < 2) { s_grade_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: hal_audio_click(true); commit_grade(); break;
        case INPUT_BTN_BACK: s_mode = VIEW; break;
        default: break;
        }
        return;
    }

    if (s_mode == PICK_TARGET) {
        switch (e.type) {
        case INPUT_NAV_UP:    if (s_pick_row > 0) s_pick_row--; break;
        case INPUT_NAV_DOWN:  if (s_pick_row < 3) s_pick_row++; break;
        case INPUT_NAV_LEFT:
        case INPUT_ENC_CCW:   pick_adjust(-1); break;
        case INPUT_NAV_RIGHT:
        case INPUT_ENC_CW:    pick_adjust(+1); break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
            hal_audio_click(true);
            if (s_pick_row == 3) {
                hifz_set_scope(s_pick_kind == 0 ? HZ_SCOPE_JUZ : HZ_SCOPE_SURAH,
                               s_pick_arg, s_pick_rev != 0);
                s_mode = VIEW;
                s_sel = 0;
                toast("Target set");
            } else {
                s_pick_row++;
            }
            break;
        case INPUT_BTN_BACK:
            // Refuse to leave with no target — there'd be nothing to show.
            if (hifz_scope().kind == HZ_SCOPE_NONE) scene_switch(SCENE_HOME);
            else s_mode = s_pick_ret;
            break;
        default: break;
        }
        return;
    }

    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:
        if (s_sel > 0) { s_sel--; hal_audio_click(false); }
        break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:
        if (s_sel < s_nrows - 1) { s_sel++; hal_audio_click(false); }
        break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        hal_audio_click(true);
        if (s_nrows > 0) {
            // OK drills the selected portion — the drill is the better way to
            // earn a grade. Hand-grading stays on LEFT for review-on-a-walk.
            scene_hifz_drill_set_portion(s_rows[s_sel].portion);
            scene_switch(SCENE_HIFZ_DRILL);
        } else if (hifz_plan()->sabaq_blocked) {
            toast("Clear the overdue review first");
        } else if (hifz_start_new_portion() >= 0) {
            rebuild_rows();
            toast("New portion ready");
        } else {
            toast("Nothing left in this target");
        }
        break;
    case INPUT_NAV_LEFT:
        // Hand-grade from memory, no drill — a hafiz reviewing on a walk.
        if (s_nrows > 0) {
            hal_audio_click(true);
            s_grade_task = s_sel;
            s_grade_sel = 0;
            s_mode = GRADE;
        }
        break;
    case INPUT_NAV_RIGHT:
        // Flip to the memorization heat map + weak spots (TARGET picker lives
        // one level in, on the map's own '>').
        hal_audio_click(true);
        s_weak_sel = 0;
        s_map_seq = 0;   // force a rebuild
        s_mode = MAP;
        break;
    case INPUT_BTN_MODE:
        // Cross over to the meaning game — its own lesson, its own target.
        hal_audio_click(true);
        hifz_flush();
        scene_switch(SCENE_TAFSIR);
        break;
    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        hifz_flush();
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

void scene_hifz_register(void) { scene_register(SCENE_HIFZ, &CB); }
