// scene_hifz_drill.c — the talqeen drill, on screen.
//
// scene_hifz launches this with a portion; here we chunk it into word-range
// segments and render the drill engine's phases: hear it (LISTEN), echo it
// (ECHO), then recite it under a progressively heavier veil (FADE 33 -> 66 ->
// RECALL 100), grade the recall, and stitch segments together (CONNECT). All
// the judgement lives in core/quran/hifz_drill.c; this file draws it and feeds
// it input.
//
// Two things are forced here per the plan's findings:
//   * the drill always uses packs/reader_sm regardless of g_prefs.font_size —
//     a recall you can only see a third of can't be self-assessed (2:282 is
//     5+ screens at the reader's own size), and it falls back to the user's
//     pack only if the small one is missing;
//   * the veil is painted as a POST-PASS over the word boxes (arabic_veil_ayah),
//     never threaded through the glyph blit.
#include "scene.h"
#include "hifz.h"
#include "hifz_drill.h"
#include "wordmeaning.h"
#include "arabic_text.h"
#include "quran_db.h"
#include "prefs.h"
#include "player.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

#define DRILL_TARGET_WORDS 40   // ~half a page: fits the band at reader_sm

static int  s_portion = -1;     // set by scene_hifz before switching in
static bool s_applied;          // grades committed to hifz on completion

static GlyphPack s_pack;
static bool s_pack_ok;
static int  s_pack_surah = -1;

static int  s_grade_sel;        // 0 = GOT, 1 = SHAKY, 2 = NO
static const char *s_toast;
static int  s_toast_ttl;

// Word-by-word meaning sheet (toggled by MODE / RIGHT).
static WordMeaning s_wm;
static bool s_wm_ok;
static bool s_sheet;            // meaning sheet open (drill paused while open)
static int  s_selw;             // selected word, flattened over the segment

// scene_hifz calls this, then scene_switch(SCENE_HIFZ_DRILL).
void scene_hifz_drill_set_portion(int portion) { s_portion = portion; }

static void toast(const char *m) { s_toast = m; s_toast_ttl = 90; }

// The current segment's words, flattened over its (possibly multiple) ayat.
static int seg_total_words(const HifzSeg *s, int surah)
{
    int n = 0;
    for (int a = s->a0; a <= s->a1; a++) {
        int wlo = (a == s->a0) ? s->w0 : 0;
        int whi = (a == s->a1) ? s->w1 : qdb_word_count(surah, a) - 1;
        n += whi - wlo + 1;
    }
    return n;
}

// Map flattened index -> (ayah, word-in-ayah). Returns false if out of range.
static bool seg_word_ref(const HifzSeg *s, int surah, int idx, int *ayah, int *w)
{
    for (int a = s->a0; a <= s->a1; a++) {
        int wlo = (a == s->a0) ? s->w0 : 0;
        int whi = (a == s->a1) ? s->w1 : qdb_word_count(surah, a) - 1;
        int cnt = whi - wlo + 1;
        if (idx < cnt) { *ayah = a; *w = wlo + idx; return true; }
        idx -= cnt;
    }
    return false;
}

static void open_pack(int surah)
{
    if (s_pack_ok && s_pack_surah == surah) return;
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; }
    // Force the small pack; only the tajweed variant follows the user's prefs.
    char path[48];
    snprintf(path, sizeof path, "packs/reader_sm%s/%d.qgp",
             g_prefs.tajweed ? "_tj" : "", surah);
    s_pack_ok = glyphpack_open(&s_pack, path);
    if (!s_pack_ok)   // fall back to whatever the reader would use
        s_pack_ok = glyphpack_open(&s_pack, prefs_font_pack(surah));
    s_pack_surah = surah;
}

static void on_enter(void)
{
    s_applied = false;
    s_grade_sel = 0;
    s_toast_ttl = 0;
    s_sheet = false;
    s_selw = 0;

    const HifzPortion *p = hifz_portion(s_portion);
    if (!p || !p->first_g) { scene_switch(SCENE_HIFZ); return; }

    // A portion is a contiguous global range; the drill is single-surah, so
    // clamp to the surah of its first ayah (cross-surah portions are rare and
    // get their tail drilled next session — full multi-surah is H6).
    QRef a = qdb_from_global(p->first_g);
    QRef b = qdb_from_global(p->last_g);
    int surah = a.surah;
    int a0 = a.ayah;
    int a1 = (b.surah == surah) ? b.ayah : qdb_ayah_count(surah);

    static HifzSeg segs[HIFZ_DRILL_MAX_SEGS];
    int n = hifz_chunk(surah, a0, a1, DRILL_TARGET_WORDS, segs, HIFZ_DRILL_MAX_SEGS);
    if (n <= 0) { scene_switch(SCENE_HIFZ); return; }

    open_pack(surah);
    s_wm_ok = wordmeaning_open(&s_wm, surah);   // absent until the fetch is run

    HifzDrillCfg cfg = {
        .listen_reps = (uint8_t)hifz_cfg_listen_reps(),
        .echo_reps   = (uint8_t)hifz_cfg_echo_reps(),
        .pace_pct    = (uint16_t)hifz_cfg_pace_pct(),
    };
    hifz_drill_begin(surah, segs, n, &cfg);
}

static void on_leave(void)
{
    hifz_drill_end();
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; s_pack_surah = -1; }
    if (s_wm_ok) { wordmeaning_close(&s_wm); s_wm_ok = false; }
    hifz_flush();
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    // The meaning sheet pauses the drill — studying a word shouldn't burn the
    // recall clock or auto-advance the phase out from under the reader.
    if (!s_sheet) hifz_drill_tick(plat_millis());
    if (s_toast_ttl > 0) s_toast_ttl--;

    if (hifz_drill_done() && !s_applied) {
        s_applied = true;
        // One grade per portion — hifz_grade cascades it to per-ayah strength.
        // The per-segment grades already shaped the worst-of aggregate.
        hifz_grade(s_portion, hifz_drill_portion_grade(), hifz_drill_peeks());
        scene_switch(SCENE_HIFZ);
    }
}

// -------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------
// Short title (font_medium) + a subtitle (font_tiny) — the one-line form
// overflowed the 320px band, clipping at both edges.
static const char *phase_name(DrillPhase p)
{
    switch (p) {
    case DRILL_LISTEN:  return "LISTEN";
    case DRILL_ECHO:    return "ECHO";
    case DRILL_FADE1:
    case DRILL_FADE2:   return "FADING";
    case DRILL_RECALL:  return "RECALL";
    case DRILL_ASSESS:  return "HOW DID IT GO?";
    case DRILL_CONNECT: return "CONNECT";
    case DRILL_CHUNK_CONNECT: return "FULL RUN";
    default:            return "";
    }
}

static const char *phase_hint(DrillPhase p)
{
    switch (p) {
    case DRILL_LISTEN:  return "listen, follow the words";
    case DRILL_ECHO:    return "recite along, out loud";
    case DRILL_FADE1:
    case DRILL_FADE2:   return "keep reciting as it fades";
    case DRILL_RECALL:  return "from memory now";
    case DRILL_CONNECT: return "join it to what came before";
    case DRILL_CHUNK_CONNECT: return "the whole chunk, end to end";
    default:            return "";
    }
}

// Draw one ayah of the current segment, veiled per the engine, into the band.
// Returns the y just below what it drew. `hl_word` highlights a chosen word
// (the meaning-sheet selection), else the LISTEN active word.
static int draw_seg_ayah(Canvas *c, int surah, int ayah, int wlo, int whi,
                         int y, DrillPhase ph, VeilMode vm, int vpct, int hl_word)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, surah, ayah, &g)) return y;
    int x = (CANVAS_WIDTH - g.w) / 2;

    int hl = hl_word;
    if (hl < 0 && ph == DRILL_LISTEN && ayah == g_player.ayah)
        hl = g_player.active_word;
    arabic_draw_ayah(c, x, y, &g, THEME_TEXT, hl, THEME_PLAYHEAD);

    if (vm != VEIL_NONE) {
        static uint8_t mask[192];
        int nw = g.n_words < (int)sizeof mask ? g.n_words : (int)sizeof mask;
        ayah_veil_mask(mask, nw, wlo, whi, vm, vpct, 2);
        bool outline = (ph == DRILL_FADE1 || ph == DRILL_FADE2);
        AyahVeil v = {
            .mask = mask, .n_words = nw, .outline = outline,
            .curtain = outline ? THEME_PANEL : THEME_BG,
            .edge = THEME_GRID,
        };
        arabic_veil_ayah(c, x, y, &g, &v);
    }
    return y + g.h + 8;
}

static void render_band(Canvas *c, int band_top, int band_bot)
{
    const HifzSeg *s = hifz_drill_cur_seg();
    if (!s) return;
    DrillPhase ph = hifz_drill_phase();
    VeilMode vm; int vpct;
    hifz_drill_veil(&vm, &vpct);
    int surah = hifz_drill_surah();

    // The meaning sheet reveals the text (veil off) and highlights the word.
    int sel_ayah = -1, sel_w = -1;
    if (s_sheet) {
        vm = VEIL_NONE;
        seg_word_ref(s, surah, s_selw, &sel_ayah, &sel_w);
    }

    int y = band_top;
    for (int a = s->a0; a <= s->a1 && y < band_bot; a++) {
        int wlo = (a == s->a0) ? s->w0 : 0;
        int whi = (a == s->a1) ? s->w1 : qdb_word_count(surah, a) - 1;
        int hl = (a == sel_ayah) ? sel_w : -1;
        y = draw_seg_ayah(c, surah, a, wlo, whi, y, ph, vm, vpct, hl);
    }
}

static void render_assess(Canvas *c, int y)
{
    static const char *L[3] = { "GOT IT", "SHAKY", "NO" };
    static const char *H[3] = { "clean, no hesitation", "got there, but rough",
                                "blanked or needed the text" };
    for (int i = 0; i < 3; i++) {
        int ry = y + i * 40;
        bool sel = (i == s_grade_sel);
        if (sel) theme_sel_block(c, 20, ry, CANVAS_WIDTH - 40, 34);
        else {
            canvas_rect_fill(c, 20, ry, CANVAS_WIDTH - 40, 34, THEME_PANEL);
            canvas_rect(c, 20, ry, CANVAS_WIDTH - 40, 34, THEME_GRID);
        }
        color_t fg = sel ? THEME_SEL_TEXT : THEME_TEXT;
        font_draw_string(c, 34, ry + 5, &font_small, L[i], fg);
        font_draw_string(c, 34, ry + 21, &font_tiny, H[i],
                         sel ? THEME_SEL_TEXT : THEME_DIM);
    }
}

static void render_countdown(Canvas *c, int y)
{
    // The timed rep gives silence a shape: a shrinking bar at the recite tempo.
    uint32_t rem = hifz_drill_rep_remaining(plat_millis());
    int reps = hifz_drill_reps(), rep = hifz_drill_rep();
    char t[24];
    if (reps > 1) snprintf(t, sizeof t, "rep %d / %d", rep + 1, reps);
    else          snprintf(t, sizeof t, "%s", rem ? "reciting..." : "");
    font_draw_string_centered(c, y, &font_tiny, t, THEME_DIM);
    // A rough 0..1 fill: full at rep start, empties as the deadline nears.
    float f = rem > 3000 ? 1.f : (float)rem / 3000.f;
    canvas_progress_bar(c, 40, y + 12, CANVAS_WIDTH - 80, 4, f,
                        THEME_ACTIVE, THEME_GRID);
}

// The bottom meaning sheet: the selected word's position + its English gloss,
// wrapped to the panel width. The Arabic word itself is highlighted in place up
// in the band (render_band), so the eye maps gloss -> word without a second copy.
static void render_sheet(Canvas *c, int top)
{
    const HifzSeg *s = hifz_drill_cur_seg();
    int surah = hifz_drill_surah();
    int total = s ? seg_total_words(s, surah) : 0;
    int ayah = -1, w = -1;
    const char *gloss = "";
    if (s && seg_word_ref(s, surah, s_selw, &ayah, &w) && s_wm_ok)
        gloss = wordmeaning_word(&s_wm, ayah, w);

    int h = CANVAS_HEIGHT - THEME_KEYBAR_H - top;
    canvas_rect_fill(c, 0, top, CANVAS_WIDTH, h, THEME_PANEL);
    canvas_hline(c, 0, top, CANVAS_WIDTH, THEME_ACCENT);

    char pos[24];
    snprintf(pos, sizeof pos, "word %d / %d", s_selw + 1, total);
    font_draw_string(c, 12, top + 6, &font_tiny, pos, THEME_LABEL);
    font_draw_string_right(c, CANVAS_WIDTH - 12, top + 6, &font_tiny,
                           s_wm_ok ? "MEANING" : "no meanings", THEME_DIM);

    if (!gloss[0]) {
        font_draw_string_centered(c, top + 30, &font_small,
                                  s_wm_ok ? "(no meaning for this word)"
                                          : "run tools/build_wordmeanings.py",
                                  THEME_DIM);
        return;
    }
    char lines[4][FONT_WRAP_LINE_CAP];
    int n = font_wrap_lines(&font_small, gloss, CANVAS_WIDTH - 24,
                            (char (*)[FONT_WRAP_LINE_CAP])lines, 4);
    for (int i = 0; i < n; i++)
        font_draw_string_centered(c, top + 26 + i * 18, &font_small, lines[i],
                                  THEME_TEXT);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    DrillPhase ph = hifz_drill_phase();

    char ref[24];
    const HifzSeg *s = hifz_drill_cur_seg();
    int surah = hifz_drill_surah();
    if (s) snprintf(ref, sizeof ref, "%d:%d  seg %d/%d", surah, s->a0,
                    hifz_drill_seg() + 1, hifz_drill_nsegs());
    else   snprintf(ref, sizeof ref, "%d", surah);
    theme_header(c, "DRILL", THEME_TITLE, ref, THEME_LABEL);

    int band_top = 34;
    int band_bot = CANVAS_HEIGHT - THEME_KEYBAR_H - 76;
    render_band(c, band_top, band_bot);

    int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 68;
    if (s_sheet) {
        render_sheet(c, iy - 8);
    } else if (ph == DRILL_ASSESS) {
        render_assess(c, iy - 44);
    } else {
        font_draw_string_centered(c, iy, &font_medium, phase_name(ph),
                                  ph == DRILL_LISTEN ? THEME_ACTIVE : THEME_TITLE);
        font_draw_string_centered(c, iy + 20, &font_tiny, phase_hint(ph), THEME_DIM);
        if (ph == DRILL_ECHO || ph == DRILL_FADE1 || ph == DRILL_FADE2 ||
            ph == DRILL_RECALL || ph == DRILL_CONNECT || ph == DRILL_CHUNK_CONNECT)
            render_countdown(c, iy + 32);
        if (hifz_drill_peeking())
            font_draw_string_centered(c, iy + 52, &font_tiny,
                                      "peeking caps you at SHAKY", THEME_BADGE);
    }

    if (s_toast_ttl > 0 && s_toast)
        font_draw_string_centered(c, band_top + 4, &font_tiny, s_toast, THEME_BADGE);

    // Keybar per state.
    if (s_sheet) {
        KeyChip k[2] = {
            { "<>", "WORD", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT,
                                 INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "MD", "CLOSE", 3, { INPUT_BTN_MODE, INPUT_NAV_SELECT, INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
    } else if (ph == DRILL_ASSESS) {
        KeyChip k[2] = {
            { "^v", "CHOOSE", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN,
                                   INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "OK", "GRADE", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
        };
        theme_keybar(c, k, 2);
    } else if (ph == DRILL_DONE) {
        KeyChip k[1] = { { "", "", 0, { INPUT_NONE } } };
        theme_keybar(c, k, 1);
    } else {
        KeyChip k[5] = {
            { "OK", ph == DRILL_LISTEN ? "SKIP" : "DONE", 2,
              { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "<", "AGAIN", 2, { INPUT_NAV_LEFT, INPUT_ENC_CCW } },
            { "v", "PEEK", 1, { INPUT_NAV_DOWN } },
            { ">", "MEANING", 2, { INPUT_NAV_RIGHT, INPUT_BTN_MODE } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 5);
    }
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
static void on_input(InputEvent e)
{
    uint32_t now = plat_millis();
    DrillPhase ph = hifz_drill_phase();

    // Meaning sheet takes over input while open (the drill is paused).
    if (s_sheet) {
        const HifzSeg *s = hifz_drill_cur_seg();
        int total = s ? seg_total_words(s, hifz_drill_surah()) : 0;
        switch (e.type) {
        case INPUT_NAV_RIGHT:
        case INPUT_ENC_CW:  if (s_selw < total - 1) { s_selw++; hal_audio_click(false); } break;
        case INPUT_NAV_LEFT:
        case INPUT_ENC_CCW: if (s_selw > 0) { s_selw--; hal_audio_click(false); } break;
        case INPUT_BTN_MODE:
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH:
        case INPUT_BTN_BACK: hal_audio_click(true); s_sheet = false; break;
        default: break;
        }
        return;
    }

    if (ph == DRILL_ASSESS) {
        switch (e.type) {
        case INPUT_NAV_UP:
        case INPUT_ENC_CCW:  if (s_grade_sel > 0) { s_grade_sel--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:
        case INPUT_ENC_CW:   if (s_grade_sel < 2) { s_grade_sel++; hal_audio_click(false); } break;
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: {
            static const HifzGrade G[3] = { HZ_GOT, HZ_SHAKY, HZ_NO };
            hal_audio_click(true);
            hifz_drill_grade(G[s_grade_sel], now);
            s_grade_sel = 0;
            break;
        }
        case INPUT_BTN_BACK: hifz_drill_end(); scene_switch(SCENE_HIFZ); break;
        default: break;
        }
        return;
    }

    switch (e.type) {
    case INPUT_NAV_SELECT:
    case INPUT_ENC_PUSH:
        hal_audio_click(true);
        hifz_drill_skip(now);
        break;
    case INPUT_NAV_LEFT:
    case INPUT_ENC_CCW:
        hal_audio_click(false);
        hifz_drill_repeat(now);
        break;
    case INPUT_NAV_DOWN:
        hifz_drill_peek(now);
        break;
    case INPUT_NAV_RIGHT:
    case INPUT_BTN_MODE:
        // Open the meaning sheet. It reveals the text, so it counts as a peek
        // (harmless in LISTEN/ECHO, honest during recall).
        if (!s_wm_ok) { toast("Meanings not installed"); break; }
        hal_audio_click(true);
        s_sheet = true;
        s_selw = 0;
        hifz_drill_peek(now);
        break;
    case INPUT_BTN_BACK:
    case INPUT_BTN_MENU:
        hifz_drill_end();
        scene_switch(SCENE_HIFZ);
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

void scene_hifz_drill_register(void) { scene_register(SCENE_HIFZ_DRILL, &CB); }
