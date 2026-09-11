// scene_tafsir_play.c — the Tafsir Game, on screen.
//
// A meaning drill: it walks the ayat the learner owes today (tglearn's due
// queue, topped up with new ones from the target), and for each shows a card
// built by tafsirgame.c. This first cut plays QUIZ cards only — the ayah is
// drawn with one word highlighted and you pick its English meaning; cloze and
// assemble land in the next pass. The result of each ayah is graded back into
// the Leitner scheduler.
//
// It degrades the same way the talqeen drill does: the per-word glosses come
// from the optional .qwm packs, so with none installed every card politely
// says so rather than showing a broken quiz.
#include "scene.h"
#include "tglearn.h"
#include "tafsirgame.h"
#include "wordmeaning.h"
#include "arabic_text.h"
#include "quran_db.h"
#include "prefs.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

#define SESSION_MAX 24

static int  s_session[SESSION_MAX];   // global ayah indices for this sitting
static int  s_n, s_idx;               // count, current position
static int  s_done_ok, s_done_total;  // tally for the summary line

static WordMeaning s_wm;  static bool s_wm_ok;  static int s_wm_surah = -1;
static GlyphPack   s_pack; static bool s_pack_ok; static int s_pack_surah = -1;

static uint32_t s_seed_base;          // varied per card by index + ayah
static TafsirCard s_card;
static bool s_have_card;              // false => no gloss for this ayah
static int  s_sel;                    // highlighted choice
static bool s_answered;
static bool s_correct;

// -------------------------------------------------------------------------
static void open_pack(int surah)
{
    if (s_pack_ok && s_pack_surah == surah) return;
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; }
    s_pack_ok = glyphpack_open(&s_pack, prefs_font_pack(surah));
    s_pack_surah = surah;
}

static void open_wm(int surah)
{
    if (s_wm_ok && s_wm_surah == surah) return;
    if (s_wm_ok) { wordmeaning_close(&s_wm); s_wm_ok = false; }
    s_wm_ok = wordmeaning_open(&s_wm, surah);
    s_wm_surah = surah;
}

// Build the card for the session's current ayah (QUIZ only for now).
static void load_card(void)
{
    s_have_card = false;
    s_answered = false;
    s_correct = false;
    s_sel = 0;
    if (s_idx < 0 || s_idx >= s_n) return;

    QRef r = qdb_from_global(s_session[s_idx]);
    if (r.surah <= 0) return;
    open_pack(r.surah);
    open_wm(r.surah);
    if (!s_wm_ok) return;

    TafsirSource src = { .wm = &s_wm, .surah = r.surah };
    uint32_t rng;
    tg_seed(&rng, s_seed_base + (uint32_t)s_idx * 2654435761u + (uint32_t)r.ayah);
    s_have_card = tg_make_card(&src, r.ayah, TG_QUIZ, &rng, &s_card);
}

// Compose the sitting: everything due, then new ayat up to the daily cap.
static void build_session(void)
{
    s_n = tglearn_today(s_session, SESSION_MAX);
    int budget = tglearn_new_per_day();
    while (s_n < SESSION_MAX && budget-- > 0) {
        int g = tglearn_start_new();
        if (!g) break;
        s_session[s_n++] = g;
    }
    s_idx = 0;
    s_done_ok = s_done_total = 0;
}

static void leave_to_caller(void)
{
    tglearn_flush();
    SceneID ret = scene_take_return();
    scene_switch(ret == SCENE_COUNT ? SCENE_HOME : ret);
}

static void on_enter(void)
{
    s_seed_base = plat_millis() | 1u;
    build_session();
    load_card();
}

static void on_leave(void)
{
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; s_pack_surah = -1; }
    if (s_wm_ok)   { wordmeaning_close(&s_wm);  s_wm_ok = false;   s_wm_surah = -1; }
    tglearn_flush();
}

// -------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------
static int choice_count(void) { return s_have_card ? s_card.n_choices : 0; }

// Draw the ayah with the quizzed word washed in gold, top-aligned in the band.
static void render_ayah(Canvas *c, int surah, int ayah, int top, int bot, int hl)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, surah, ayah, &g)) return;
    int x = (CANVAS_WIDTH - g.w) / 2;
    int y = top;
    if (g.h < bot - top) y = top + (bot - top - g.h) / 2;   // center short ayat
    arabic_draw_ayah(c, x, y, &g, THEME_TEXT, hl, THEME_ACCENT);
}

static void render_choices(Canvas *c, int top)
{
    int n = choice_count();
    for (int i = 0; i < n; i++) {
        int y = top + i * 34;
        bool sel = (i == s_sel);
        color_t fg = THEME_TEXT;
        if (s_answered) {
            if (i == s_card.correct) {
                canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_ACTIVE);
                fg = THEME_SEL_TEXT;
            } else if (sel && !s_correct) {
                canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_BADGE);
                fg = THEME_SEL_TEXT;
            } else {
                canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_PANEL);
                canvas_rect(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_GRID);
                fg = THEME_DIM;
            }
        } else if (sel) {
            theme_sel_block(c, 20, y, CANVAS_WIDTH - 40, 30);
            fg = THEME_SEL_TEXT;
        } else {
            canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_PANEL);
            canvas_rect(c, 20, y, CANVAS_WIDTH - 40, 30, THEME_GRID);
        }
        char label[4] = { (char)('A' + i), 0, 0, 0 };
        font_draw_string(c, 30, y + 8, &font_small, label, fg);
        font_draw_string(c, 52, y + 8, &font_small, tg_choice(&s_card, i), fg);
    }
}

static void on_render(Canvas *c)
{
    theme_clear(c);

    // Empty sitting: nothing due and nothing left to learn in the target.
    if (s_n == 0) {
        theme_header(c, "TAFSIR", THEME_TITLE, "", THEME_LABEL);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 16, &font_small,
                                  "Nothing to study right now", THEME_TEXT);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 + 4, &font_tiny,
                                  "set a target in Lessons", THEME_DIM);
        KeyChip k[1] = { { "BK", "BACK", 1, { INPUT_BTN_BACK } } };
        theme_keybar(c, k, 1);
        return;
    }

    // Session complete.
    if (s_idx >= s_n) {
        theme_header(c, "TAFSIR", THEME_TITLE, "done", THEME_LABEL);
        char line[32];
        snprintf(line, sizeof line, "%d / %d correct", s_done_ok, s_done_total);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 8, &font_medium, line, THEME_ACTIVE);
        KeyChip k[1] = { { "OK", "FINISH", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } } };
        theme_keybar(c, k, 1);
        return;
    }

    QRef r = qdb_from_global(s_session[s_idx]);
    char ref[24];
    snprintf(ref, sizeof ref, "%d:%d  %d/%d", r.surah, r.ayah, s_idx + 1, s_n);
    int hdr_y = theme_header(c, "TAFSIR", THEME_TITLE, ref, THEME_LABEL);

    int kb_top = CANVAS_HEIGHT - THEME_KEYBAR_H;

    if (!s_have_card) {
        render_ayah(c, r.surah, r.ayah, hdr_y + 8, kb_top - 60, -1);
        font_draw_string_centered(c, kb_top - 44, &font_small,
                                  s_wm_ok ? "(no meaning for this ayah)"
                                          : "run tools/build_wordmeanings.py",
                                  THEME_DIM);
        KeyChip k[2] = {
            { "OK", "SKIP", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
        return;
    }

    int n = choice_count();
    int choices_top = kb_top - 8 - n * 34;
    int prompt_y = choices_top - 20;

    render_ayah(c, r.surah, r.ayah, hdr_y + 8, prompt_y - 6, s_card.word);

    const char *prompt = s_answered
        ? (s_correct ? "Correct" : "The gold word means:")
        : "What does the gold word mean?";
    font_draw_string_centered(c, prompt_y, &font_tiny, prompt,
                              s_answered ? (s_correct ? THEME_ACTIVE : THEME_BADGE)
                                         : THEME_LABEL);
    render_choices(c, choices_top);

    if (!s_answered) {
        KeyChip k[3] = {
            { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                                   INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "OK", "ANSWER", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 3);
    } else {
        KeyChip k[2] = {
            { "OK", "NEXT", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
    }
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void advance(void)
{
    s_idx++;
    if (s_idx < s_n) load_card();
}

static void answer(void)
{
    s_answered = true;
    s_correct = (s_sel == s_card.correct);
    hal_audio_click(true);
    QRef r = qdb_from_global(s_session[s_idx]);
    tglearn_grade(r.surah, r.ayah, s_correct ? TG_GOT : TG_WRONG);
    s_done_total++;
    if (s_correct) s_done_ok++;
}

static void on_input(InputEvent e)
{
    // Empty sitting or completed: any confirm/back leaves.
    if (s_n == 0 || s_idx >= s_n) {
        switch (e.type) {
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
        case INPUT_BTN_BACK:
        case INPUT_BTN_MENU: leave_to_caller(); break;
        default: break;
        }
        return;
    }

    if (e.type == INPUT_BTN_BACK || e.type == INPUT_BTN_MENU) {
        leave_to_caller();
        return;
    }

    if (!s_have_card) {                     // "no meaning" — OK just skips
        if (e.type == INPUT_NAV_SELECT || e.type == INPUT_ENC_PUSH) advance();
        return;
    }

    if (!s_answered) {
        int n = choice_count();
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_sel > 0)     { s_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_sel < n - 1) { s_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: answer(); break;
        default: break;
        }
    } else {
        switch (e.type) {
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: hal_audio_click(true); advance(); break;
        default: break;
        }
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_exit = on_leave,
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = NULL,
};

void scene_tafsir_play_register(void) { scene_register(SCENE_TAFSIR_PLAY, &CB); }
