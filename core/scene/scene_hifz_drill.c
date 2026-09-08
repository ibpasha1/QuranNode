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
#include "recite.h"
#include "voice_activity.h"
#include "quran_db.h"
#include "prefs.h"
#include "player.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRILL_TARGET_WORDS 40   // ~half a page: fits the band at reader_sm

// Optional mic scoring (mirrors the teacher's sizing; freed on scene exit so two
// scenes never pin ~6.4 MB of PSRAM at once).
#define MIC_HZ      16000
#define REC_MAX_N   (MIC_HZ * 40)      // 40s recording cap
#define REF_MAX_N   (44100 * 22)       // reference PCM cap (~22s)
#define DRILL_MAX_WORDS 64

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

// Optional mic scoring during RECALL. Verdicts SUGGEST, never commit: an offline
// analysis pre-selects a grade (downward only — its reliable signal is omission
// detection, not correctness) and the user still presses OK.
typedef enum { MIC_OFF = 0, MIC_REC, MIC_ANALYZE } MicState;
static MicState s_mic;
static int16_t *s_rec, *s_ref;         // malloc'd on first use, freed on exit
static uint32_t s_rec_n, s_ref_n, s_ref_hz;
static VoiceActivity s_va;
static int  s_suggest = -1;            // -1 = none, else 0/1/2 grade index
static const char *s_suggest_msg;

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
    s_mic = MIC_OFF;
    s_suggest = -1;
    s_suggest_msg = NULL;
    s_rec_n = s_ref_n = 0;

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
    hal_mic_stop();
    if (s_pack_ok) { glyphpack_close(&s_pack); s_pack_ok = false; s_pack_surah = -1; }
    if (s_wm_ok) { wordmeaning_close(&s_wm); s_wm_ok = false; }
    // Free the big capture buffers — finding 4: two scenes would otherwise pin
    // ~6.4 MB of PSRAM between them.
    free(s_rec); s_rec = NULL;
    free(s_ref); s_ref = NULL;
    hifz_flush();
}

// Begin recording the user's recall of the current segment's first ayah. Fully
// optional: any missing piece (mic, reference audio, memory) just declines.
static void start_mic(void)
{
    const HifzSeg *s = hifz_drill_cur_seg();
    if (!s) return;
    int surah = hifz_drill_surah();
    if (!s_rec) s_rec = malloc(REC_MAX_N * sizeof(int16_t));
    if (!s_ref) s_ref = malloc(REF_MAX_N * sizeof(int16_t));
    if (!s_rec || !s_ref) { toast("Out of memory"); return; }

    // Reference recitation for the segment's first ayah (the scorer needs it).
    player_load(&g_player, surah, s->a0);
    s_ref_n = g_player.clip
            ? hal_audio_read_pcm16(g_player.clip, 0, s_ref, REF_MAX_N, &s_ref_hz) : 0;
    if (!s_ref_n) { toast("No reference audio here"); return; }
    if (!hal_mic_start(MIC_HZ)) { toast("No microphone"); return; }

    s_rec_n = 0;
    va_init(&s_va, MIC_HZ);
    hal_audio_click(true);
    s_mic = MIC_REC;
}

// Score the take against the reference and SUGGEST a grade (never commit it).
// Offline DTW's reliable signal is omission detection, so it only ever suggests
// DOWNWARD — a clean-sounding take leaves the choice to the reader.
static void analyze_and_suggest(void)
{
    hal_mic_stop();
    s_mic = MIC_OFF;
    s_suggest = -1;
    s_suggest_msg = NULL;

    const HifzSeg *s = hifz_drill_cur_seg();
    int a0 = s ? s->a0 : 0;
    if (!s_va.heard) { s_suggest_msg = "Didn't hear you"; goto assess; }

    uint32_t a, b;
    va_span(&s_va, s_rec_n, &a, &b);
    int nw = timing_word_count(&g_player.timing, a0);
    if (nw <= 0 || !s_ref_n || b <= a) {
        s_suggest_msg = "Couldn't score - grade yourself";
        goto assess;
    }
    if (nw > DRILL_MAX_WORDS) nw = DRILL_MAX_WORDS;
    WordTiming wt[DRILL_MAX_WORDS];
    for (int i = 0; i < nw; i++) timing_word(&g_player.timing, a0, i, &wt[i]);
    ReciteWord rw[DRILL_MAX_WORDS];
    if (!recite_analyze(s_ref, s_ref_n, s_ref_hz, s_rec + a, b - a, MIC_HZ,
                        wt, nw, rw)) {
        s_suggest_msg = "Couldn't align - grade yourself";
        goto assess;
    }

    int missing = 0, bad = 0;
    for (int i = 0; i < nw; i++) {
        if (rw[i].verdict == RECITE_MISSING) missing++;
        else if (rw[i].verdict == RECITE_MISMATCH) bad++;
    }
    if (missing >= (nw + 1) / 2) {
        s_suggest = 2;  s_suggest_msg = "Heard gaps - suggested NO";
    } else if (missing > 0 || bad >= (nw + 2) / 3) {
        s_suggest = 1;  s_suggest_msg = "Some gaps - suggested SHAKY";
    } else {
        s_suggest = -1; s_suggest_msg = "No omissions heard - your call";
    }

assess:
    if (s_suggest >= 0) s_grade_sel = s_suggest;   // move the cursor; user confirms
    hifz_drill_skip(plat_millis());                // RECALL -> ASSESS
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;

    // Mic recording/analysis pauses the drill (like the meaning sheet).
    if (s_mic == MIC_REC) {
        int got = hal_mic_read(s_rec + s_rec_n, (int)(REC_MAX_N - s_rec_n));
        if (va_feed(&s_va, s_rec, REC_MAX_N, &s_rec_n, got) != VA_RUNNING)
            s_mic = MIC_ANALYZE;
        if (s_toast_ttl > 0) s_toast_ttl--;
        return;
    }
    if (s_mic == MIC_ANALYZE) { analyze_and_suggest(); return; }

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

#define BAND_GAP 8
#define BAND_MAX_AYAT 32

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

    // Measure the stacked segment (index-only — no blob streaming), so a tall
    // multi-ayah segment can be scrolled instead of overflowing the band.
    int na = s->a1 - s->a0 + 1;
    if (na > BAND_MAX_AYAT) na = BAND_MAX_AYAT;
    int hs[BAND_MAX_AYAT];
    int content_h = 0;
    for (int i = 0; i < na; i++) {
        int w = 0, h = 0;
        hs[i] = (s_pack_ok && glyphpack_dims(&s_pack, surah, s->a0 + i, &w, &h)) ? h : 0;
        content_h += hs[i] + BAND_GAP;
    }
    if (content_h > 0) content_h -= BAND_GAP;
    int band_h = band_bot - band_top;

    // Teleprompter scroll so the lower ayat of a tall segment are reachable:
    // during audio LISTEN follow the playing ayah; while the meaning sheet is
    // open follow the selected ayah; on a timed rep pan top->bottom at tempo.
    int scroll = 0;
    if (content_h > band_h) {
        int focus = -1;
        if (s_sheet && sel_ayah >= 0)            focus = sel_ayah - s->a0;
        else if (ph == DRILL_LISTEN && hifz_drill_is_audio())
                                                 focus = g_player.ayah - s->a0;
        if (focus >= 0) {
            if (focus >= na) focus = na - 1;
            int off = 0;
            for (int i = 0; i < focus; i++) off += hs[i] + BAND_GAP;
            scroll = off - band_h / 3;           // keep the focus in the upper third
        } else {
            uint32_t rms = hifz_drill_rep_ms();
            int reps = hifz_drill_reps(), rep = hifz_drill_rep();
            float prog = 0.f;
            if (reps > 0 && rms > 0) {
                uint32_t rem = hifz_drill_rep_remaining(plat_millis());
                float inrep = (float)(rms - (rem > rms ? rms : rem)) / (float)rms;
                prog = ((float)rep + inrep) / (float)reps;
            }
            if (prog < 0.f) prog = 0.f;
            if (prog > 1.f) prog = 1.f;
            scroll = (int)(prog * (float)(content_h - band_h));
        }
        if (scroll < 0) scroll = 0;
        if (scroll > content_h - band_h) scroll = content_h - band_h;
    }

    int y = band_top - scroll;
    for (int i = 0; i < na; i++) {
        int a = s->a0 + i;
        if (y + hs[i] > band_top && y < band_bot) {   // cull ayat outside the band
            int wlo = (a == s->a0) ? s->w0 : 0;
            int whi = (a == s->a1) ? s->w1 : qdb_word_count(surah, a) - 1;
            int hl = (a == sel_ayah) ? sel_w : -1;
            draw_seg_ayah(c, surah, a, wlo, whi, y, ph, vm, vpct, hl);
        }
        y += hs[i] + BAND_GAP;
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
    int hdr_y = theme_header(c, "DRILL", THEME_TITLE, ref, THEME_LABEL);

    int band_top = 34;
    int band_bot = CANVAS_HEIGHT - THEME_KEYBAR_H - 76;
    render_band(c, band_top, band_bot);
    // Clip the band: a scrolled tall segment can overdraw a partial ayah past
    // either edge (arabic_draw_ayah only clips to the canvas), so erase the
    // strips above and below so nothing bleeds into the header or the label.
    if (band_top > hdr_y)
        canvas_rect_fill(c, 0, hdr_y, CANVAS_WIDTH, band_top - hdr_y, THEME_BG);
    canvas_rect_fill(c, 0, band_bot, CANVAS_WIDTH,
                     (CANVAS_HEIGHT - THEME_KEYBAR_H) - band_bot, THEME_BG);

    int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 68;
    if (s_mic == MIC_REC) {
        font_draw_string_centered(c, iy, &font_medium, "RECORDING", THEME_BADGE);
        font_draw_string_centered(c, iy + 20, &font_tiny,
                                  s_va.heard ? "reciting... pause when done"
                                             : "recite from memory", THEME_DIM);
        theme_meter(c, 40, iy + 34, CANVAS_WIDTH - 80, 8, s_va.peak);
    } else if (s_sheet) {
        render_sheet(c, iy - 8);
    } else if (ph == DRILL_ASSESS) {
        render_assess(c, iy - 44);
        // A mic suggestion moves the cursor, never commits — say why it landed.
        if (s_suggest_msg)
            font_draw_string_centered(c, iy - 58, &font_tiny, s_suggest_msg,
                                      THEME_ACCENT);
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
    if (s_mic == MIC_REC) {
        KeyChip k[2] = {
            { "OK", "DONE NOW", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "BK", "CANCEL", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
    } else if (s_sheet) {
        KeyChip k[2] = {
            { "<>", "WORD", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT,
                                 INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "MD", "CLOSE", 3, { INPUT_BTN_MODE, INPUT_NAV_SELECT, INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
    } else if (ph == DRILL_RECALL) {
        KeyChip k[5] = {
            { "OK", "DONE", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "^", "REC", 1, { INPUT_NAV_UP } },
            { "v", "PEEK", 1, { INPUT_NAV_DOWN } },
            { ">", "MEANING", 2, { INPUT_NAV_RIGHT, INPUT_BTN_MODE } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 5);
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

    // While recording, the endpointer auto-stops; OK finishes early, BK cancels.
    if (s_mic == MIC_REC) {
        switch (e.type) {
        case INPUT_NAV_SELECT:
        case INPUT_ENC_PUSH: s_mic = MIC_ANALYZE; break;
        case INPUT_BTN_BACK: hal_mic_stop(); s_mic = MIC_OFF; break;
        default: break;
        }
        return;
    }

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
    case INPUT_NAV_UP:
        // Record my recall (RECALL only) — endpoints, scores, suggests a grade.
        if (ph == DRILL_RECALL) start_mic();
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
