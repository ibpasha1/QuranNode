// scene_tafsir_play.c — the Tafsir Game, on screen.
//
// A meaning drill. It walks the ayat the learner owes today (tglearn's due
// queue, topped up with new ones from the target), and turns each ayah into a
// small DECK of cards built by tafsirgame.c, rotating the three kinds for
// variety and retention:
//
//   QUIZ     — the ayah with one word washed gold; pick its English meaning.
//   CLOZE    — the ayah's full meaning with one word blanked; pick the gap.
//   ASSEMBLE — the word-meanings shown shuffled; place them in reading order.
//
// The ayah's grade comes from accuracy across its deck: all right -> GOT, a
// slip or two -> HARD, mostly wrong -> WRONG, fed straight into the Leitner
// scheduler. Glosses come from the optional .qwm packs, so with none installed
// each ayah politely says so rather than showing a broken card.
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

#define SESSION_MAX     24
#define CARDS_PER_AYAH  4
#define ASM_MAX_TILES   5     // assemble stays legible only for short ayat

static int  s_session[SESSION_MAX];   // global ayah indices for this sitting
static int  s_n, s_idx;               // count, current ayah
static int  s_done_ok, s_done_total;  // ayah-level tally for the summary

static WordMeaning s_wm;  static bool s_wm_ok;  static int s_wm_surah = -1;
static GlyphPack   s_pack; static bool s_pack_ok; static int s_pack_surah = -1;

static uint32_t s_seed_base;          // varied per card
static unsigned s_spin;               // rotates the leading kind across cards

static TafsirCard s_deck[CARDS_PER_AYAH];
static int  s_deck_n, s_card_i;       // cards this ayah, current card
static int  s_ayah_ok, s_ayah_total;  // accuracy accumulator for the ayah

// Per-card interaction state.
static bool s_answered, s_correct;
static int  s_sel;                    // MCQ highlighted choice
// Assemble: pool of remaining tiles (item indices) + the order picked so far.
static int  s_pool[TG_MAX_ITEMS], s_pool_n, s_asm_sel;
static int  s_answer[TG_MAX_ITEMS], s_answer_n;

static TafsirCard *cur(void) { return &s_deck[s_card_i]; }

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

static int glossed_words(int ayah)
{
    int nw = wordmeaning_word_count(&s_wm, ayah), n = 0;
    for (int w = 0; w < nw; w++) if (wordmeaning_word(&s_wm, ayah, w)[0]) n++;
    return n;
}

// Build `prefer`, else fall back to the other MCQ kinds so a card still lands.
static bool make_one(TafsirSource *src, int ayah, TafsirCardKind prefer,
                     uint32_t *rng, TafsirCard *out)
{
    if (tg_make_card(src, ayah, prefer, rng, out)) return true;
    const TafsirCardKind fb[2] = { TG_QUIZ, TG_CLOZE };
    for (int i = 0; i < 2; i++)
        if (fb[i] != prefer && tg_make_card(src, ayah, fb[i], rng, out)) return true;
    return false;
}

// Reset per-card interaction; if it's an assemble, seed the shuffled pool.
static void begin_card(void)
{
    s_answered = false;
    s_correct = false;
    s_sel = 0;
    s_answer_n = 0;
    s_asm_sel = 0;
    TafsirCard *c = cur();
    if (c->kind == TG_ASSEMBLE) {
        s_pool_n = c->n_items;
        for (int i = 0; i < c->n_items; i++) s_pool[i] = c->show[i];
    }
}

// Turn the current ayah into a deck of cards.
static void build_deck(void)
{
    s_deck_n = 0;
    s_card_i = 0;
    s_ayah_ok = s_ayah_total = 0;
    if (s_idx < 0 || s_idx >= s_n) return;

    QRef r = qdb_from_global(s_session[s_idx]);
    if (r.surah <= 0) return;
    open_pack(r.surah);
    open_wm(r.surah);
    if (!s_wm_ok) return;

    int gl = glossed_words(r.ayah);
    if (gl < 1) return;
    int want = gl < CARDS_PER_AYAH ? gl : CARDS_PER_AYAH;

    // Assemble only for short ayat; longer ones rotate quiz/cloze.
    bool allow_asm = (gl >= 3 && gl <= ASM_MAX_TILES);
    TafsirCardKind pool[3]; int pn = 0;
    pool[pn++] = TG_QUIZ;
    pool[pn++] = TG_CLOZE;
    if (allow_asm) pool[pn++] = TG_ASSEMBLE;

    TafsirSource src = { .wm = &s_wm, .surah = r.surah };
    for (int i = 0; i < want; i++) {
        uint32_t rng;
        tg_seed(&rng, s_seed_base + (uint32_t)s_idx * 40503u
                    + (uint32_t)r.ayah * 97u + (uint32_t)i * 131u);
        TafsirCardKind prefer = pool[(s_spin + (unsigned)i) % (unsigned)pn];
        if (make_one(&src, r.ayah, prefer, &rng, &s_deck[s_deck_n])) s_deck_n++;
    }
    s_spin += (unsigned)want;
    if (s_deck_n > 0) begin_card();
}

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
    s_spin = (plat_millis() >> 3) & 3u;
    build_session();
    build_deck();
}

static void on_leave(void)
{
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; s_pack_surah = -1; }
    if (s_wm_ok)   { wordmeaning_close(&s_wm);  s_wm_ok = false;   s_wm_surah = -1; }
    tglearn_flush();
}

// -------------------------------------------------------------------------
// Progression
// -------------------------------------------------------------------------
static void tally(bool ok) { s_ayah_total++; if (ok) s_ayah_ok++; }

static void grade_ayah(void)
{
    int wrong = s_ayah_total - s_ayah_ok;
    TgGrade g = (s_ayah_total == 0 || wrong == 0) ? TG_GOT
              : (wrong * 2 <= s_ayah_total)       ? TG_HARD
                                                  : TG_WRONG;
    QRef r = qdb_from_global(s_session[s_idx]);
    tglearn_grade(r.surah, r.ayah, g);
    s_done_total++;
    if (g == TG_GOT) s_done_ok++;
}

static void advance_card(void)
{
    s_card_i++;
    if (s_card_i < s_deck_n) { begin_card(); return; }
    grade_ayah();
    s_idx++;
    if (s_idx < s_n) build_deck();
}

// Move past an ayah we couldn't build a card for, without grading it.
static void skip_ayah(void)
{
    s_idx++;
    if (s_idx < s_n) build_deck();
}

// -------------------------------------------------------------------------
// String helpers
// -------------------------------------------------------------------------
static void join_reading(const TafsirCard *c, char *buf, int cap)
{
    int len = 0; buf[0] = '\0';
    for (int i = 0; i < c->n_items; i++) {
        const char *s = tg_item(c, i);
        int l = (int)strlen(s);
        if (len + (i ? 1 : 0) + l >= cap - 1) break;
        if (i) buf[len++] = ' ';
        memcpy(buf + len, s, (size_t)l); len += l; buf[len] = '\0';
    }
}

static void join_order(const TafsirCard *c, const int *order, int n, char *buf, int cap)
{
    int len = 0; buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        const char *s = tg_item(c, order[i]);
        int l = (int)strlen(s);
        if (len + (i ? 1 : 0) + l >= cap - 1) break;
        if (i) buf[len++] = ' ';
        memcpy(buf + len, s, (size_t)l); len += l; buf[len] = '\0';
    }
}

// -------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------
static void render_ayah(Canvas *c, int surah, int ayah, int top, int bot, int hl)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, surah, ayah, &g)) return;
    int x = (CANVAS_WIDTH - g.w) / 2;
    int y = top;
    if (g.h < bot - top) y = top + (bot - top - g.h) / 2;   // center a short ayah
    arabic_draw_ayah(c, x, y, &g, THEME_TEXT, hl, THEME_ACCENT);
}

// Draw a gloss into a fixed width: font_small if it fits, else font_tiny, else
// font_tiny truncated with ".." — real Quran glosses ("of those who go astray")
// overflow a choice row otherwise.
static void draw_fit(Canvas *c, int x, int y, int maxw, const char *s, color_t col)
{
    if (font_string_width(&font_small, s) <= maxw) {
        font_draw_string(c, x, y, &font_small, s, col);
        return;
    }
    if (font_string_width(&font_tiny, s) <= maxw) {
        font_draw_string(c, x, y + 1, &font_tiny, s, col);
        return;
    }
    char buf[80];
    int len = (int)strlen(s);
    if (len > (int)sizeof buf - 3) len = (int)sizeof buf - 3;
    for (; len > 0; len--) {
        memcpy(buf, s, (size_t)len);
        buf[len] = '.'; buf[len + 1] = '.'; buf[len + 2] = '\0';
        if (font_string_width(&font_tiny, buf) <= maxw) break;
    }
    if (len <= 0) buf[0] = '\0';
    font_draw_string(c, x, y + 1, &font_tiny, buf, col);
}

static void draw_choices(Canvas *c, const TafsirCard *card, int top)
{
    for (int i = 0; i < card->n_choices; i++) {
        int y = top + i * 34;
        bool sel = (i == s_sel);
        color_t fg = THEME_TEXT;
        if (s_answered) {
            if (i == card->correct) {
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
        char label[2] = { (char)('A' + i), 0 };
        font_draw_string(c, 30, y + 8, &font_small, label, fg);
        draw_fit(c, 52, y + 8, CANVAS_WIDTH - 20 - 52 - 6, tg_choice(card, i), fg);
    }
}

// QUIZ + CLOZE share the multiple-choice layout; cloze also shows the sentence.
static void render_mcq(Canvas *c, const TafsirCard *card, int surah, int ayah, int hdr_y)
{
    bool cloze = (card->kind == TG_CLOZE);
    int kb_top = CANVAS_HEIGHT - THEME_KEYBAR_H;
    int choices_top = kb_top - 8 - card->n_choices * 34;

    char lines[3][FONT_WRAP_LINE_CAP];
    int nlines = 0, sentence_top = choices_top;
    if (cloze) {
        char sent[256];
        join_reading(card, sent, sizeof sent);
        nlines = font_wrap_lines(&font_small, sent, CANVAS_WIDTH - 24,
                                 (char (*)[FONT_WRAP_LINE_CAP])lines, 3);
        sentence_top = choices_top - 10 - nlines * 16;
    }
    int prompt_y = (cloze ? sentence_top : choices_top) - 16;

    render_ayah(c, surah, ayah, hdr_y + 8, prompt_y - 6, card->word);

    const char *prompt = s_answered
        ? (s_correct ? "Correct" : (cloze ? "The missing word is:" : "The gold word means:"))
        : (cloze ? "Fill the blank:" : "What does the gold word mean?");
    font_draw_string_centered(c, prompt_y, &font_tiny, prompt,
                              s_answered ? (s_correct ? THEME_ACTIVE : THEME_BADGE)
                                         : THEME_LABEL);
    for (int i = 0; i < nlines; i++)
        font_draw_string_centered(c, sentence_top + i * 16, &font_small, lines[i], THEME_TEXT);

    draw_choices(c, card, choices_top);

    if (!s_answered) {
        KeyChip k[3] = {
            { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
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

static void render_assemble(Canvas *c, const TafsirCard *card, int surah, int ayah, int hdr_y)
{
    int kb_top = CANVAS_HEIGHT - THEME_KEYBAR_H;

    if (s_answered) {
        // One row per placed tile, green if it landed in the right slot.
        int rows_top = kb_top - 8 - card->n_items * 30;
        font_draw_string_centered(c, rows_top - 18, &font_tiny,
                                  s_correct ? "Correct order" : "Not quite",
                                  s_correct ? THEME_ACTIVE : THEME_BADGE);
        render_ayah(c, surah, ayah, hdr_y + 8, rows_top - 24, -1);
        for (int k = 0; k < card->n_items; k++) {
            int y = rows_top + k * 30;
            bool ok = (s_answer[k] == k);
            canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 26, ok ? THEME_ACTIVE : THEME_BADGE);
            char n[3]; snprintf(n, sizeof n, "%d", k + 1);
            font_draw_string(c, 28, y + 6, &font_small, n, THEME_SEL_TEXT);
            draw_fit(c, 48, y + 6, CANVAS_WIDTH - 20 - 48 - 6, tg_item(card, s_answer[k]),
                     THEME_SEL_TEXT);
        }
        KeyChip k2[2] = {
            { "OK", "NEXT", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k2, 2);
        return;
    }

    // Pool of remaining tiles at the bottom, cursor selectable.
    int pool_top = kb_top - 8 - s_pool_n * 28;
    // "Built so far" above the pool.
    char built[256];
    join_order(card, s_answer, s_answer_n, built, sizeof built);
    char lines[2][FONT_WRAP_LINE_CAP];
    int nlines = s_answer_n ? font_wrap_lines(&font_small, built, CANVAS_WIDTH - 24,
                                              (char (*)[FONT_WRAP_LINE_CAP])lines, 2) : 0;
    int built_top = pool_top - 10 - (nlines ? nlines * 16 : 16);
    int prompt_y = built_top - 16;

    render_ayah(c, surah, ayah, hdr_y + 8, prompt_y - 6, -1);
    font_draw_string_centered(c, prompt_y, &font_tiny, "Put the meaning in order:", THEME_LABEL);
    if (nlines)
        for (int i = 0; i < nlines; i++)
            font_draw_string_centered(c, built_top + i * 16, &font_small, lines[i], THEME_TEXT);
    else
        font_draw_string_centered(c, built_top, &font_tiny, "(pick the words in order)", THEME_DIM);

    for (int i = 0; i < s_pool_n; i++) {
        int y = pool_top + i * 28;
        bool sel = (i == s_asm_sel);
        if (sel) { theme_sel_block(c, 20, y, CANVAS_WIDTH - 40, 24); }
        else {
            canvas_rect_fill(c, 20, y, CANVAS_WIDTH - 40, 24, THEME_PANEL);
            canvas_rect(c, 20, y, CANVAS_WIDTH - 40, 24, THEME_GRID);
        }
        draw_fit(c, 30, y + 5, CANVAS_WIDTH - 20 - 30 - 6, tg_item(card, s_pool[i]),
                 sel ? THEME_SEL_TEXT : THEME_TEXT);
    }

    KeyChip k[4] = {
        { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
        { "OK", "PLACE", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        { "<", "UNDO", 1, { INPUT_NAV_LEFT } },
        { "BK", "STOP", 1, { INPUT_BTN_BACK } },
    };
    theme_keybar(c, k, 4);
}

static void on_render(Canvas *c)
{
    theme_clear(c);

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

    if (s_idx >= s_n) {
        theme_header(c, "TAFSIR", THEME_TITLE, "done", THEME_LABEL);
        char line[32];
        snprintf(line, sizeof line, "%d / %d ayat", s_done_ok, s_done_total);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2 - 16, &font_tiny, "clean recall", THEME_DIM);
        font_draw_string_centered(c, CANVAS_HEIGHT / 2, &font_medium, line, THEME_ACTIVE);
        KeyChip k[1] = { { "OK", "FINISH", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } } };
        theme_keybar(c, k, 1);
        return;
    }

    QRef r = qdb_from_global(s_session[s_idx]);
    char ref[24];
    snprintf(ref, sizeof ref, "%d:%d  %d/%d", r.surah, r.ayah, s_idx + 1, s_n);
    int hdr_y = theme_header(c, "TAFSIR", THEME_TITLE, ref, THEME_LABEL);

    if (s_deck_n == 0) {
        int kb_top = CANVAS_HEIGHT - THEME_KEYBAR_H;
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

    TafsirCard *card = cur();
    if (card->kind == TG_ASSEMBLE) render_assemble(c, card, r.surah, r.ayah, hdr_y);
    else                           render_mcq(c, card, r.surah, r.ayah, hdr_y);
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void mcq_answer(void)
{
    TafsirCard *card = cur();
    s_answered = true;
    s_correct = (s_sel == card->correct);
    hal_audio_click(true);
    tally(s_correct);
}

static void asm_pick(void)
{
    TafsirCard *card = cur();
    if (s_pool_n <= 0) return;
    s_answer[s_answer_n++] = s_pool[s_asm_sel];
    for (int i = s_asm_sel; i < s_pool_n - 1; i++) s_pool[i] = s_pool[i + 1];
    s_pool_n--;
    if (s_asm_sel >= s_pool_n && s_asm_sel > 0) s_asm_sel--;
    hal_audio_click(false);
    if (s_pool_n == 0) {
        bool ok = true;
        for (int k = 0; k < card->n_items; k++) if (s_answer[k] != k) { ok = false; break; }
        s_answered = true;
        s_correct = ok;
        hal_audio_click(true);
        tally(ok);
    }
}

static void asm_undo(void)
{
    if (s_answer_n <= 0) return;
    s_pool[s_pool_n++] = s_answer[--s_answer_n];   // back to the end of the pool
    hal_audio_click(false);
}

static void on_input(InputEvent e)
{
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

    if (e.type == INPUT_BTN_BACK || e.type == INPUT_BTN_MENU) { leave_to_caller(); return; }

    if (s_deck_n == 0) {                     // "no meaning" — OK skips the ayah
        if (e.type == INPUT_NAV_SELECT || e.type == INPUT_ENC_PUSH) skip_ayah();
        return;
    }

    TafsirCard *card = cur();

    if (s_answered) {                        // feedback shown — OK advances
        if (e.type == INPUT_NAV_SELECT || e.type == INPUT_ENC_PUSH) {
            hal_audio_click(true);
            advance_card();
        }
        return;
    }

    if (card->kind == TG_ASSEMBLE) {
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_asm_sel > 0)             { s_asm_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_asm_sel < s_pool_n - 1)  { s_asm_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: asm_pick(); break;
        case INPUT_NAV_LEFT: asm_undo(); break;
        default: break;
        }
        return;
    }

    // QUIZ / CLOZE
    switch (e.type) {
    case INPUT_NAV_UP:
    case INPUT_ENC_CCW:  if (s_sel > 0)                   { s_sel--; hal_audio_click(false); } break;
    case INPUT_NAV_DOWN:
    case INPUT_ENC_CW:   if (s_sel < card->n_choices - 1) { s_sel++; hal_audio_click(false); } break;
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH: mcq_answer(); break;
    default: break;
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
