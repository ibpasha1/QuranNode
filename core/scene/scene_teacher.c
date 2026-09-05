// scene_teacher.c — the Quran Teacher: listen -> recite -> analyze -> review.
//
// The practice loop: the teacher (reference reciter) plays the ayah, the user
// recites it back into the mic, the recording is aligned to the reference
// (core/audio/recite.c) and each word of the Arabic gets a verdict mark —
// green underline = close match, amber = worth a listen, coral = significant
// mismatch or missing. In review, words are selectable: hear the teacher's
// word, hear your own attempt at it, or re-record the whole ayah.
//
// V1 scoring is acoustic similarity, and the UI says so ("match", never
// "correct") — see the honesty note in recite.h.
#include "scene.h"
#include "arabic_text.h"
#include "recite.h"
#include "timing.h"
#include "progress.h"
#include "prefs.h"
#include "quran_db.h"
#include "theme.h"
#include "font.h"
#include "canvas.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIC_HZ        16000
#define REC_MAX_SEC   40                      // recording cap
#define REC_MAX_N     (MIC_HZ * REC_MAX_SEC)
#define REF_MAX_N     (48000 * 45)            // reference PCM cap (~45s @48k)
#define MAX_WORDS     64

typedef enum {
    TEA_READY,      // ayah loaded; prompt to listen
    TEA_LISTEN,     // teacher playing the ayah
    TEA_RECITE,     // mic open, user reciting
    TEA_ANALYZE,    // one-tick analysis pass
    TEA_REVIEW,     // per-word verdicts on the Arabic; word nav + playback
    TEA_NO_MIC,     // platform has no microphone
    TEA_NO_DATA,    // ayah has no audio/timing in the bundle
} TeaState;

static TeaState s_state;
static int  s_surah = 1, s_ayah = 1;

static GlyphPack s_pack;
static bool s_pack_ok;
static int  s_pack_surah = -1;   // packs are per-surah; reload when the surah changes
static struct HalAudioClip *s_clip;
static TimingTable s_timing;
static bool s_timing_ok;

static int16_t *s_rec;          // user recording (malloc'd on enter)
static uint32_t s_rec_n;
static int16_t *s_ref;          // reference mono PCM (malloc'd on enter)
static uint32_t s_ref_n, s_ref_hz;

static ReciteWord s_words[MAX_WORDS];
static int  s_nwords;
static int  s_sel_word;         // selected word in review (reading order)
static uint32_t s_seg_stop_ms;  // stop teacher playback at this clip pos (0=off)
static float s_level;           // live mic level 0..1 (recite view meter)

// Voice-activity endpointing: the recording auto-starts (right after LISTEN)
// and auto-finishes, so the user just recites and pauses — no button needed.
// The analyzed audio is trimmed to [s_voice_a, s_voice_b): without the trim,
// the silent lead-in (while the user draws breath) aligns to the first word
// and scores it "not heard" — the V1 field bug.
#define VOICE_PEAK      700       // s16 peak that counts as voice (~0.021 fs)
                                  // low on purpose: quiet mics/AGC — the
                                  // scorer has its own take-relative floor
#define VOICE_PREROLL   (MIC_HZ / 4)          // keep 250ms before first voice
#define VOICE_TAILPAD   (MIC_HZ / 3)          // keep 330ms after last voice
#define AUTO_STOP_MS    1600      // this much silence after voice = done
#define NO_VOICE_MS     12000     // never heard anything = give up
static bool     s_heard;          // any voice yet this take
static uint32_t s_voice_a, s_voice_b;   // first/last voiced sample bounds
static uint32_t s_sil_ms;         // silence since the last voiced chunk
static uint32_t s_wait_ms;        // total time waiting with no voice at all
static uint32_t s_rec_off_ms;     // trim offset (maps analysis times -> s_rec)
static const char *s_ready_hint;  // one-shot status line on the READY view

static void load_ayah(int surah, int ayah)
{
    if (s_clip) { hal_audio_close(s_clip); s_clip = NULL; }
    if (s_timing_ok) { timing_close(&s_timing); s_timing_ok = false; }
    hal_pcm_stop();
    s_surah = surah; s_ayah = ayah;
    s_rec_n = 0; s_ref_n = 0; s_nwords = 0; s_sel_word = 0; s_seg_stop_ms = 0;

    // Per-surah glyph pack: (re)load when the surah changes.
    if (!s_pack_ok || s_pack_surah != surah) {
        if (s_pack_ok) glyphpack_close(&s_pack);
        s_pack_ok = glyphpack_open(&s_pack, prefs_font_pack(surah));
        s_pack_surah = surah;
    }

    char path[64];
    snprintf(path, sizeof(path), "audio/%s/%d/%d.mp3", "abdulbasit", surah, ayah);
    s_clip = hal_audio_open(path);
    s_timing_ok = timing_open(&s_timing, surah);
    if (!s_clip || !s_timing_ok ||
        timing_word_count(&s_timing, ayah) <= 0) {
        s_state = TEA_NO_DATA;
        return;
    }
    s_state = TEA_READY;
}

static void on_enter(void)
{
    if (!s_rec) s_rec = malloc(REC_MAX_N * sizeof(int16_t));
    if (!s_ref) s_ref = malloc(REF_MAX_N * sizeof(int16_t));
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    load_ayah(r.surah, r.ayah);
}

static void on_leave(void)
{
    hal_mic_stop();
    hal_pcm_stop();
    if (s_clip) { hal_audio_close(s_clip); s_clip = NULL; }
    if (s_timing_ok) { timing_close(&s_timing); s_timing_ok = false; }
    // Keep s_rec/s_ref/pack resident: re-entry is common, sim/PSRAM have room.
}

static void start_listen(void)
{
    hal_pcm_stop();
    s_seg_stop_ms = 0;
    hal_audio_seek_ms(s_clip, 0);
    hal_audio_set_rate(s_clip, 1.0f);
    hal_audio_play(s_clip);
    s_state = TEA_LISTEN;
}

static void start_recite(void)
{
    hal_audio_pause(s_clip);
    if (!hal_mic_start(MIC_HZ)) { s_state = TEA_NO_MIC; return; }
    s_rec_n = 0;
    s_level = 0;
    s_heard = false;
    s_voice_a = s_voice_b = 0;
    s_sil_ms = s_wait_ms = 0;
    s_ready_hint = NULL;
    hal_audio_click(true);   // audible "your turn" cue
    s_state = TEA_RECITE;
}

static void finish_recite(void)
{
    hal_mic_stop();
    if (!s_heard) {          // nothing to analyze — back to the prompt
        s_ready_hint = "Didn't hear you - try again";
        s_state = TEA_READY;
        return;
    }
    s_state = TEA_ANALYZE;   // next tick runs the analysis
}

static void run_analysis(void)
{
    s_nwords = timing_word_count(&s_timing, s_ayah);
    if (s_nwords > MAX_WORDS) s_nwords = MAX_WORDS;

    // Pull the reference PCM once per ayah (cached until the ayah changes).
    if (s_ref_n == 0)
        s_ref_n = hal_audio_read_pcm16(s_clip, 0, s_ref, REF_MAX_N, &s_ref_hz);

    WordTiming wt[MAX_WORDS];
    for (int i = 0; i < s_nwords; i++)
        timing_word(&s_timing, s_ayah, i, &wt[i]);

    // Analyze only the voiced span (plus a little tail) — leading silence
    // otherwise aligns to the first words and marks them "not heard".
    uint32_t a = s_voice_a, b = s_voice_b + VOICE_TAILPAD;
    if (b > s_rec_n) b = s_rec_n;
    if (a >= b) { a = 0; b = s_rec_n; }
    s_rec_off_ms = a * 1000u / MIC_HZ;

    bool ok = s_ref_n && b > a &&
              recite_analyze(s_ref, s_ref_n, s_ref_hz,
                             s_rec + a, b - a, MIC_HZ,
                             wt, s_nwords, s_words);
    if (!ok)
        for (int i = 0; i < s_nwords; i++)
            s_words[i] = (ReciteWord){ RECITE_MISSING, 99.f, 0, 0 };

    // Jump the review cursor to the first word needing attention.
    s_sel_word = 0;
    for (int i = 0; i < s_nwords; i++)
        if (s_words[i].verdict != RECITE_GOOD) { s_sel_word = i; break; }
    s_state = TEA_REVIEW;
}

// Play the teacher's audio for just the selected word.
static void play_teacher_word(void)
{
    WordTiming wt;
    if (!timing_word(&s_timing, s_ayah, s_sel_word, &wt)) return;
    hal_pcm_stop();
    hal_audio_seek_ms(s_clip, wt.start_ms);
    s_seg_stop_ms = wt.end_ms;
    hal_audio_play(s_clip);
}

// Play the stretch of the user's recording the alignment matched to the word.
static void play_user_word(void)
{
    const ReciteWord *w = &s_words[s_sel_word];
    if (w->user_end_ms <= w->user_start_ms) return;
    hal_audio_pause(s_clip);
    // Analysis times are relative to the trimmed take; map into s_rec.
    uint32_t a = (w->user_start_ms + s_rec_off_ms) * (MIC_HZ / 1000);
    uint32_t b = (w->user_end_ms + s_rec_off_ms) * (MIC_HZ / 1000);
    if (b > s_rec_n) b = s_rec_n;
    if (a >= b) return;
    hal_pcm_play(s_rec + a, b - a, MIC_HZ);
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    switch (s_state) {
    case TEA_LISTEN:
        if (!hal_audio_is_playing(s_clip)) start_recite();
        break;
    case TEA_RECITE: {
        int got = hal_mic_read(s_rec + s_rec_n, (int)(REC_MAX_N - s_rec_n));
        if (got > 0) {
            // Peak of this chunk: drives the meter and the voice endpointer.
            int peak = 0;
            for (int i = 0; i < got; i++) {
                int v = s_rec[s_rec_n + i];
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            float lv = (float)peak / 32768.f;
            s_level = lv > s_level ? lv : s_level * 0.85f;

            uint32_t chunk_ms = (uint32_t)got * 1000u / MIC_HZ;
            if (peak >= VOICE_PEAK) {
                if (!s_heard) {
                    s_heard = true;
                    s_voice_a = s_rec_n > VOICE_PREROLL ? s_rec_n - VOICE_PREROLL : 0;
                }
                s_voice_b = s_rec_n + (uint32_t)got;
                s_sil_ms = 0;
            } else if (s_heard) {
                s_sil_ms += chunk_ms;
            } else {
                s_wait_ms += chunk_ms;
            }

            s_rec_n += (uint32_t)got;
            // Fluid flow: recited then paused -> analyze automatically;
            // never spoke at all -> give up back to the prompt.
            if ((s_heard && s_sil_ms >= AUTO_STOP_MS) ||
                (!s_heard && s_wait_ms >= NO_VOICE_MS) ||
                s_rec_n >= REC_MAX_N)
                finish_recite();
        }
        break;
    }
    case TEA_ANALYZE:
        run_analysis();
        break;
    case TEA_REVIEW:
        // Stop teacher playback at the end of the selected word's segment.
        if (s_seg_stop_ms && hal_audio_is_playing(s_clip) &&
            hal_audio_pos_ms(s_clip) >= s_seg_stop_ms) {
            hal_audio_pause(s_clip);
            s_seg_stop_ms = 0;
        }
        break;
    default: break;
    }
}

static color_t verdict_color(ReciteVerdict v)
{
    switch (v) {
    case RECITE_GOOD:    return THEME_ACTIVE;
    case RECITE_UNSURE:  return THEME_ACCENT;
    default:             return THEME_BADGE;
    }
}

static const char *verdict_text(ReciteVerdict v)
{
    switch (v) {
    case RECITE_GOOD:    return "close match";
    case RECITE_UNSURE:  return "a bit different - listen";
    case RECITE_MISMATCH:return "quite different - compare";
    default:             return "not heard - try again";
    }
}

// Draw the ayah centered in the band; in review, underline each word in its
// verdict color and box the selected word.
static int draw_ayah_marked(Canvas *c, int band_top, int band_bot, bool marked)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, s_surah, s_ayah, &g)) return -1;
    int top = band_top + (band_bot - band_top - g.h) / 2;
    if (top < band_top) top = band_top;
    int x = (CANVAS_WIDTH - g.w) / 2;
    arabic_draw_ayah(c, x, top, &g, THEME_TEXT,
                     marked ? s_sel_word : -1, THEME_PLAYHEAD);
    if (marked) {
        int n = g.n_words < s_nwords ? g.n_words : s_nwords;
        for (int i = 0; i < n; i++) {
            AtWordBox b;
            if (!ayah_word_box(&g, i, &b)) continue;
            int uy = top + b.y + b.h + 2;
            if (uy > band_bot - 2) uy = band_bot - 2;
            canvas_rect_fill(c, x + b.x, uy, b.w, 3,
                             verdict_color(s_words[i].verdict));
            if (i == s_sel_word)
                canvas_rect(c, x + b.x - 2, top + b.y - 2, b.w + 4, b.h + 8,
                            THEME_ACCENT);
        }
    }
    return top;
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    char ref[24];
    snprintf(ref, sizeof(ref), "%d:%d", s_surah, s_ayah);
    theme_header(c, "QURAN TEACHER", THEME_TITLE, ref, THEME_LABEL);

    int band_top = 26, band_bot = CANVAS_HEIGHT - THEME_KEYBAR_H - 64;

    switch (s_state) {
    case TEA_NO_DATA:
        font_draw_string_centered(c, 200, &font_small, "No audio for this ayah",
                                  THEME_DIM);
        font_draw_string_centered(c, 224, &font_tiny,
                                  "(only Al-Fatihah is bundled)", THEME_DIM);
        break;
    case TEA_NO_MIC:
        font_draw_string_centered(c, 200, &font_small, "No microphone",
                                  THEME_DIM);
        font_draw_string_centered(c, 224, &font_tiny,
                                  "Mic capture isn't available here yet",
                                  THEME_DIM);
        break;

    case TEA_READY:
    case TEA_LISTEN: {
        draw_ayah_marked(c, band_top, band_bot, false);
        int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 52;
        bool listening = (s_state == TEA_LISTEN);
        font_draw_string_centered(c, iy, &font_medium,
                                  listening ? "LISTEN" : "READY",
                                  listening ? THEME_ACTIVE : THEME_TITLE);
        font_draw_string_centered(c, iy + 26, &font_tiny,
                                  listening ? "recite it back when the teacher finishes"
                                  : s_ready_hint ? s_ready_hint
                                                 : "the teacher recites, then you repeat",
                                  !listening && s_ready_hint ? THEME_BADGE : THEME_DIM);
        if (listening) {
            uint32_t pos = hal_audio_pos_ms(s_clip), len = hal_audio_len_ms(s_clip);
            canvas_progress_bar(c, 40, iy + 40, CANVAS_WIDTH - 80, 4,
                                len ? (float)pos / len : 0.f,
                                THEME_ACTIVE, THEME_GRID);
        }
        break;
    }

    case TEA_RECITE: {
        draw_ayah_marked(c, band_top, band_bot, false);
        int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 52;
        // The endpointer drives the flow: recite, pause, it analyzes itself.
        font_draw_string_centered(c, iy, &font_medium,
                                  s_heard ? "HEARING YOU" : "RECITE",
                                  s_heard ? THEME_ACTIVE : THEME_BADGE);
        font_draw_string_centered(c, iy + 26, &font_tiny,
                                  s_heard ? "pause when you finish - I'll notice"
                                          : "go ahead - I'm listening",
                                  THEME_DIM);
        // Live mic meter.
        theme_meter(c, 40, iy + 38, CANVAS_WIDTH - 80, 8, s_level);
        break;
    }

    case TEA_ANALYZE:
        draw_ayah_marked(c, band_top, band_bot, false);
        font_draw_string_centered(c, CANVAS_HEIGHT - THEME_KEYBAR_H - 44,
                                  &font_medium, "ANALYZING...", THEME_TITLE);
        break;

    case TEA_REVIEW: {
        draw_ayah_marked(c, band_top, band_bot, true);
        // Verdict panel for the selected word.
        int py = CANVAS_HEIGHT - THEME_KEYBAR_H - 60;
        canvas_rect_fill(c, 0, py, CANVAS_WIDTH, 60, THEME_PANEL);
        canvas_hline(c, 0, py, CANVAS_WIDTH, THEME_GRID);
        const ReciteWord *w = &s_words[s_sel_word];
        char line[48];
        snprintf(line, sizeof(line), "Word %d of %d", s_sel_word + 1, s_nwords);
        font_draw_string(c, 12, py + 8, &font_small, line, THEME_TEXT);
        // Match legend counts: good / unsure / flagged.
        int ng = 0, nu = 0, nb = 0;
        for (int i = 0; i < s_nwords; i++) {
            if (s_words[i].verdict == RECITE_GOOD) ng++;
            else if (s_words[i].verdict == RECITE_UNSURE) nu++;
            else nb++;
        }
        snprintf(line, sizeof(line), "%d ok  %d unsure  %d flagged", ng, nu, nb);
        font_draw_string_right(c, CANVAS_WIDTH - 12, py + 10, &font_tiny, line,
                               THEME_DIM);
        canvas_rect_fill(c, 12, py + 26, 24, 3, verdict_color(w->verdict));
        font_draw_string(c, 44, py + 24, &font_tiny, verdict_text(w->verdict),
                         verdict_color(w->verdict));
        font_draw_string(c, 12, py + 42, &font_tiny,
                         "similarity only - not a tajweed judgement", THEME_DIM);
        break;
    }
    }

    // Keybar per state.
    switch (s_state) {
    case TEA_READY: {
        KeyChip k[3] = {
            { "OK", "LISTEN", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "^v", "AYAH", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "BK", "HOME", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 3);
        break;
    }
    case TEA_LISTEN: {
        KeyChip k[3] = {
            { "OK", "SKIP TO RECITE", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "BK", "STOP", 1, { INPUT_BTN_BACK } },
            { "", "", 0, { INPUT_NONE } },
        };
        theme_keybar(c, k, 2);
        break;
    }
    case TEA_RECITE: {
        KeyChip k[2] = {
            { "OK", "DONE NOW", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "BK", "CANCEL", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
        break;
    }
    case TEA_REVIEW: {
        KeyChip k[5] = {
            { "<>", "WORD", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "OK", "TEACHER", 2, { INPUT_NAV_SELECT, INPUT_ENC_PUSH } },
            { "PL", "YOU", 1, { INPUT_BTN_PLAY } },
            { "MD", "RETRY", 1, { INPUT_BTN_MODE } },
            { "BK", "DONE", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 5);
        break;
    }
    default: {
        KeyChip k[2] = {
            { "^v", "AYAH", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "BK", "HOME", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 2);
        break;
    }
    }
}

static void change_ayah(int dir)
{
    int a = s_ayah + dir;
    if (a < 1 || a > qdb_ayah_count(s_surah)) return;
    hal_audio_click(false);
    load_ayah(s_surah, a);
}

static void review_move(int dir)
{
    int n = s_sel_word + dir;
    if (n < 0 || n >= s_nwords) return;
    s_sel_word = n;
    hal_audio_click(false);
}

static void on_input(InputEvent e)
{
    switch (s_state) {
    case TEA_READY:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            hal_audio_click(true); start_listen(); break;
        case INPUT_NAV_UP: case INPUT_ENC_CCW: change_ayah(-1); break;
        case INPUT_NAV_DOWN: case INPUT_ENC_CW: change_ayah(+1); break;
        case INPUT_BTN_BACK: scene_switch(SCENE_HOME); break;
        default: break;
        }
        break;

    case TEA_LISTEN:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            hal_audio_click(true); start_recite(); break;
        case INPUT_BTN_BACK:
            hal_audio_pause(s_clip); s_state = TEA_READY; break;
        default: break;
        }
        break;

    case TEA_RECITE:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            hal_audio_click(true); finish_recite(); break;
        case INPUT_BTN_BACK:
            hal_mic_stop(); s_state = TEA_READY; break;
        default: break;
        }
        break;

    case TEA_REVIEW:
        switch (e.type) {
        // Reading order is right-to-left on screen; LEFT = next word feels
        // natural against the Arabic, but keep it simple: LEFT/CCW = previous.
        case INPUT_NAV_LEFT: case INPUT_ENC_CCW: review_move(-1); break;
        case INPUT_NAV_RIGHT: case INPUT_ENC_CW: review_move(+1); break;
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH:
            hal_audio_click(true); play_teacher_word(); break;
        case INPUT_BTN_PLAY:
            hal_audio_click(true); play_user_word(); break;
        case INPUT_BTN_MODE:
            hal_audio_click(true); start_listen(); break;   // full retry
        case INPUT_BTN_BACK:
            hal_audio_pause(s_clip); hal_pcm_stop(); s_state = TEA_READY; break;
        default: break;
        }
        break;

    default:   // NO_DATA / NO_MIC
        switch (e.type) {
        case INPUT_NAV_UP: case INPUT_ENC_CCW: change_ayah(-1); break;
        case INPUT_NAV_DOWN: case INPUT_ENC_CW: change_ayah(+1); break;
        case INPUT_BTN_BACK: scene_switch(SCENE_HOME); break;
        default: break;
        }
        break;
    }
}

static const SceneCallbacks CB = {
    .on_enter = on_enter,
    .on_exit = on_leave,
    .on_render = on_render,
    .on_input = on_input,
    .on_tick = on_tick,
};

void scene_teacher_register(void) { scene_register(SCENE_TEACHER, &CB); }
