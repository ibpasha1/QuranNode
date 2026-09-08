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
#include "voice_activity.h"
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
#define REF_MAX_N     (44100 * 22)            // reference PCM cap (~22s @44.1k
                                              //  = recite.c's 20s frame cap;
                                              //  frees heap for the DTW matrix)
#define MAX_WORDS     64

typedef enum {
    TEA_READY,      // ayah loaded; prompt to listen
    TEA_LISTEN,     // teacher playing the ayah
    TEA_RECITE,     // mic open, user reciting (practice OR training capture)
    TEA_ANALYZE,    // one-tick analysis pass
    TEA_REVIEW,     // per-word verdicts on the Arabic; word nav + playback
    TEA_TRAIN,      // training-capture prompt (label + take number)
    TEA_NO_MIC,     // platform has no microphone
    TEA_NO_DATA,    // ayah has no audio/timing in the bundle
} TeaState;

// --- Training capture (labeled dataset for algorithm tuning) ---------------
// A scripted sequence of takes: recite each prompt, the recording is written
// straight to the SD card as a labeled WAV (state/train_NN_label.wav) with NO
// analysis in between — speak, pause, next. The host-side batch evaluator
// (tools/recite_eval.c) then replays the whole dataset against any tweak of
// the scoring algorithm offline.
static const struct { const char *label; const char *tag; int count; } TRAIN[] = {
    { "Sincere - best effort",      "sincere",   5 },
    { "Sincere - fast",             "fast",      5 },
    { "Gibberish - same rhythm",    "gibberish", 5 },
    { "SKIP word 2, rest correct",  "skipw2",    3 },
    { "Recite 1:3's words instead", "wrongayah", 2 },
};
#define TRAIN_PHASES ((int)(sizeof(TRAIN) / sizeof(TRAIN[0])))
#define TRAIN_TOTAL  20
static int s_train = -1;   // -1 = practice mode; else take index 0..TOTAL-1

// Every take is also kept in RAM (PSRAM has room) and can be served over the
// OTA web server at http://<ip>/takes — the field SD card fails persistently
// on fresh-cluster writes, and the takes only need to reach the host once.
static struct { void *wav; uint32_t len; char name[40]; } s_kept[TRAIN_TOTAL];
static bool s_share;       // Wi-Fi sharing started this session

static void train_label(int take, const char **label, char *tag, int tagsz)
{
    int acc = 0;
    for (int p = 0; p < TRAIN_PHASES; p++) {
        if (take < acc + TRAIN[p].count) {
            *label = TRAIN[p].label;
            snprintf(tag, tagsz, "%s", TRAIN[p].tag);
            return;
        }
        acc += TRAIN[p].count;
    }
    *label = "?";
    tag[0] = 0;
}

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
static bool s_online;           // last analysis came from the scoring server
static int  s_sel_word;         // selected word in review (reading order)
static uint32_t s_seg_stop_ms;  // stop teacher playback at this clip pos (0=off)
static float s_level;           // live mic level 0..1 (recite view meter)

// Voice-activity endpointing lives in core/audio/voice_activity.{c,h} now (the
// drill shares it, and vad-test pins the field-tuned constants). The recording
// auto-starts (right after LISTEN) and auto-finishes on a pause; the analysed
// span is trimmed to skip the silent lead-in that would otherwise align to the
// first word and score it "not heard".
static VoiceActivity s_va;
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
    // +64B slop: the training saver shifts the PCM right to prepend a WAV
    // header in place (single-buffer hal_state_save).
    if (!s_rec) s_rec = malloc(REC_MAX_N * sizeof(int16_t) + 64);
    if (!s_ref) s_ref = malloc(REF_MAX_N * sizeof(int16_t));
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    load_ayah(r.surah, r.ayah);
}

static void on_leave(void)
{
    hal_mic_stop();
    hal_pcm_stop();
    s_train = -1;
    // Free the RAM takes and unregister them from the web server (dangling
    // pointers otherwise). Downloads happen while the teacher stays open.
    for (int i = 0; i < TRAIN_TOTAL; i++) {
        hal_serve_blob(i, NULL, NULL, 0);
        free(s_kept[i].wav);
        s_kept[i].wav = NULL;
        s_kept[i].len = 0;
    }
    s_share = false;
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
    va_init(&s_va, MIC_HZ);
    s_ready_hint = NULL;
    hal_audio_click(true);   // audible "your turn" cue
    s_state = TEA_RECITE;
}

// Save the whole (untrimmed) take as a 16k mono WAV named by take + label —
// untrimmed on purpose: offline tuning can then re-run endpointing too.
static void train_save_take(void)
{
    const char *label; char tag[16];
    train_label(s_train, &label, tag, sizeof(tag));
    char name[48];
    snprintf(name, sizeof(name), "train_%02d_%s.wav", s_train + 1, tag);

    uint32_t bytes = s_rec_n * sizeof(int16_t);
    uint8_t *raw = (uint8_t *)s_rec;
    memmove(raw + 44, raw, bytes);
    va_wav_header(raw, bytes, MIC_HZ);
    // Keep a RAM copy regardless of the SD outcome — downloadable over
    // Wi-Fi (LEFT on the training screen), and re-registered live if
    // sharing is already on.
    free(s_kept[s_train].wav);
    s_kept[s_train].wav = malloc(44 + bytes);
    if (s_kept[s_train].wav) {
        memcpy(s_kept[s_train].wav, raw, 44 + bytes);
        s_kept[s_train].len = 44 + bytes;
        snprintf(s_kept[s_train].name, sizeof(s_kept[s_train].name), "%s", name);
        if (s_share)
            hal_serve_blob(s_train, name, s_kept[s_train].wav, 44 + bytes);
    }

    // SD cards throw transient write errors on long bursts (field log:
    // sdmmc r2=0x2000 on take 5 of 20) — retry a few times; with the RAM
    // copy held, an SD failure no longer blocks the session at all.
    bool ok = false;
    for (int try = 0; try < 3 && !ok; try++)
        ok = hal_state_save(name, raw, 44 + bytes);
    QN_LOGI("TEACHER", "train take %d (%s): %ums -> state/%s %s%s",
            s_train + 1, tag, s_rec_n / (MIC_HZ / 1000), name,
            ok ? "saved" : "SD FAILED (3 tries)",
            s_kept[s_train].wav ? " [in RAM]" : "");
    s_rec_n = 0;   // buffer content was shifted; recording is consumed

    if (!ok && !s_kept[s_train].wav) {   // nowhere at all — redo the take
        s_ready_hint = "Save failed - OK to re-record this take";
        s_state = TEA_TRAIN;
        return;
    }
    if (!ok) s_ready_hint = "SD failed - kept in RAM (< = wifi)";
    if (++s_train >= TRAIN_TOTAL) {
        // Stay in the trainer — the RAM takes are served from HERE, and
        // leaving the scene frees them. "<" shares over Wi-Fi.
        s_train = TRAIN_TOTAL - 1;
        s_ready_hint = "All 20 done!  < = wifi download";
        s_state = TEA_TRAIN;
        return;
    }
    s_state = TEA_TRAIN;
}

// Register the RAM takes on the web server and bring Wi-Fi + HTTP up.
static void share_takes(void)
{
    int n = 0;
    for (int i = 0; i < TRAIN_TOTAL; i++)
        if (s_kept[i].wav) {
            hal_serve_blob(i, s_kept[i].name, s_kept[i].wav, s_kept[i].len);
            n++;
        }
    if (!n) { s_ready_hint = "No takes in RAM - record first"; return; }
    hal_audio_click(true);
    hal_ota_start();   // blocks a few seconds while Wi-Fi joins
    s_share = true;
}

static void finish_recite(void)
{
    hal_mic_stop();
    if (!s_va.heard) {       // nothing captured — back to the prompt
        s_ready_hint = "Didn't hear you - try again";
        s_state = (s_train >= 0) ? TEA_TRAIN : TEA_READY;
        return;
    }
    if (s_train >= 0) { train_save_take(); return; }
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
    uint32_t a, b;
    va_span(&s_va, s_rec_n, &a, &b);
    s_rec_off_ms = a * 1000u / MIC_HZ;

    // Coverage gate: a grunt (a syllable or two) can't be graded word-by-
    // word. ABSOLUTE floor only — field data shows fluent recitation runs
    // 2.5-3.5x faster than the murattal reference (a full sincere take can
    // be 1.2s of voice), so any ratio-based gate rejects honest takes; the
    // tempo-normalized aligner handles pace, and real grunts are <0.9s.
    {
        uint32_t voiced_ms = (s_va.voice_b - s_va.voice_a) / (MIC_HZ / 1000);
        if (voiced_ms < 900) {
            QN_LOGI("TEACHER", "too short: voiced=%ums — not grading", voiced_ms);
            s_ready_hint = "Too short - recite the whole ayah";
            s_state = TEA_READY;
            return;
        }
    }

    bool ok = s_ref_n && b > a &&
              recite_analyze(s_ref, s_ref_n, s_ref_hz,
                             s_rec + a, b - a, MIC_HZ,
                             wt, s_nwords, s_words);
    if (!ok)
        for (int i = 0; i < s_nwords; i++)
            s_words[i] = (ReciteWord){ RECITE_MISSING, 99.f, 0, 0 };

    // V2: ask the scoring server for word-level verdicts (it transcribes the
    // take and compares words to the canonical text — content, not acoustics;
    // docs/TEACHER_V2.md). On success its verdicts REPLACE the local ones;
    // the local spans are kept for per-word replay until the server sends
    // timestamps (M3). Offline / no server -> the local verdicts stand.
    s_online = false;
    if (b > a) {
        uint32_t pcm_bytes = (b - a) * sizeof(int16_t);
        uint8_t *wav = malloc(44 + pcm_bytes);
        if (wav) {
            va_wav_header(wav, pcm_bytes, MIC_HZ);
            memcpy(wav + 44, s_rec + a, pcm_bytes);
            RemoteWord rw[MAX_WORDS];
            int rn = hal_score_remote(wav, 44 + pcm_bytes, s_surah, s_ayah,
                                      rw, MAX_WORDS);
            free(wav);
            if (rn == s_nwords) {
                for (int i = 0; i < s_nwords; i++) {
                    s_words[i].verdict = (ReciteVerdict)rw[i].verdict;
                    s_words[i].score = 1.f - rw[i].score;   // similarity -> distance-ish
                    if (rw[i].end_ms > rw[i].start_ms) {
                        s_words[i].user_start_ms = rw[i].start_ms;
                        s_words[i].user_end_ms = rw[i].end_ms;
                    }
                }
                s_online = true;
            }
        }
    }

    QN_LOGI("TEACHER", "analyze %d:%d ok=%d ref=%ums@%u take=%ums (rec=%ums voiced=[%u..%u]ms)",
            s_surah, s_ayah, ok, s_ref_hz ? s_ref_n / (s_ref_hz / 1000) : 0, s_ref_hz,
            (b - a) / (MIC_HZ / 1000), s_rec_n / (MIC_HZ / 1000),
            s_va.voice_a / (MIC_HZ / 1000), s_va.voice_b / (MIC_HZ / 1000));
    for (int i = 0; i < s_nwords; i++)
        QN_LOGI("TEACHER", "  word %d: verdict=%d score=%.2f user=[%u..%u]ms",
                i, s_words[i].verdict, s_words[i].score,
                s_words[i].user_start_ms, s_words[i].user_end_ms);

    // If NOTHING aligned (slope-limited DTW found no valid path — take too
    // warped/short/mangled to judge), don't show a review of gray marks.
    int unclear = 0;
    for (int i = 0; i < s_nwords; i++)
        if (s_words[i].verdict == RECITE_UNCLEAR) unclear++;
    if (unclear == s_nwords && s_nwords > 0) {
        s_ready_hint = "Couldn't align - recite the whole ayah";
        s_state = TEA_READY;
        return;
    }

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
        // The shared endpointer applies the warmup drop, tracks the voiced
        // span, and decides when the take is done. Fluid flow: recited then
        // paused -> analyze; never spoke -> give up back to the prompt.
        VaResult r = va_feed(&s_va, s_rec, REC_MAX_N, &s_rec_n, got);
        if (got > 0) {
            float lv = s_va.peak;
            s_level = lv > s_level ? lv : s_level * 0.85f;
        }
        if (r != VA_RUNNING) finish_recite();
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
    case RECITE_UNCLEAR: return THEME_DIM;      // gray: no judgement made
    default:             return THEME_BADGE;
    }
}

static const char *verdict_text(ReciteVerdict v)
{
    switch (v) {
    case RECITE_GOOD:    return "close match";
    case RECITE_UNSURE:  return "a bit different - listen";
    case RECITE_MISMATCH:return "quite different - compare";
    case RECITE_UNCLEAR: return "couldn't judge - listen + retry";
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
        if (!listening && s_share) {
            char u[48];
            const char *url = hal_ota_url();
            snprintf(u, sizeof(u), "%stakes  <- download here",
                     url ? url : "wifi connecting... ");
            font_draw_string_centered(c, iy + 26, &font_tiny, u, THEME_ACTIVE);
        } else
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
        if (s_train >= 0) {   // training capture: show what to perform
            const char *label; char tag[16];
            train_label(s_train, &label, tag, sizeof(tag));
            char t[40];
            snprintf(t, sizeof(t), "TAKE %d/%d - %s", s_train + 1, TRAIN_TOTAL,
                     s_va.heard ? "hearing you" : "go");
            font_draw_string_centered(c, iy, &font_small, t,
                                      s_va.heard ? THEME_ACTIVE : THEME_BADGE);
            font_draw_string_centered(c, iy + 20, &font_tiny, label, THEME_TEXT);
        } else {
            // The endpointer drives the flow: recite, pause, it analyzes.
            font_draw_string_centered(c, iy, &font_medium,
                                      s_va.heard ? "HEARING YOU" : "RECITE",
                                      s_va.heard ? THEME_ACTIVE : THEME_BADGE);
            font_draw_string_centered(c, iy + 26, &font_tiny,
                                      s_va.heard ? "pause when you finish - I'll notice"
                                                 : "go ahead - I'm listening",
                                      THEME_DIM);
        }
        // Live mic meter.
        theme_meter(c, 40, iy + 38, CANVAS_WIDTH - 80, 8, s_level);
        break;
    }

    case TEA_TRAIN: {
        draw_ayah_marked(c, band_top, band_bot, false);
        int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 64;
        const char *label; char tag[16];
        train_label(s_train, &label, tag, sizeof(tag));
        char t[48];
        snprintf(t, sizeof(t), "TRAINING  %d / %d", s_train + 1, TRAIN_TOTAL);
        font_draw_string_centered(c, iy, &font_small, t, THEME_TITLE);
        font_draw_string_centered(c, iy + 20, &font_small, label, THEME_TEXT);
        if (s_share) {   // Wi-Fi sharing line takes priority: show the URL
            const char *url = hal_ota_url();
            snprintf(t, sizeof(t), "%stakes  <- download here",
                     url ? url : "wifi connecting... ");
            font_draw_string_centered(c, iy + 40, &font_tiny, t, THEME_ACTIVE);
        } else {
            font_draw_string_centered(c, iy + 40, &font_tiny,
                                      s_ready_hint ? s_ready_hint
                                                   : "OK records - it saves and moves on",
                                      s_ready_hint ? THEME_BADGE : THEME_DIM);
        }
        break;
    }

    case TEA_ANALYZE:
        draw_ayah_marked(c, band_top, band_bot, false);
        font_draw_string_centered(c, CANVAS_HEIGHT - THEME_KEYBAR_H - 44,
                                  &font_medium, "ANALYZING...", THEME_TITLE);
        break;

    case TEA_REVIEW: {
        draw_ayah_marked(c, band_top, band_bot, true);
        // Panel: the OVERALL result headlines (the cursor sits on the worst
        // word, and its verdict alone read like a judgement of the whole
        // take — "not heard, try again" after a 3-of-4-green recitation).
        int py = CANVAS_HEIGHT - THEME_KEYBAR_H - 60;
        canvas_rect_fill(c, 0, py, CANVAS_WIDTH, 60, THEME_PANEL);
        canvas_hline(c, 0, py, CANVAS_WIDTH, THEME_GRID);
        int ng = 0;
        for (int i = 0; i < s_nwords; i++)
            if (s_words[i].verdict == RECITE_GOOD) ng++;
        char line[56];
        if (ng == s_nwords)
            snprintf(line, sizeof(line), "MashaAllah - all matched");
        else
            snprintf(line, sizeof(line), "%d/%d matched - review", ng, s_nwords);
        font_draw_string(c, 12, py + 8, &font_small, line,
                         ng == s_nwords ? THEME_ACTIVE : THEME_TEXT);
        // Selected-word detail, clearly scoped to that word.
        const ReciteWord *w = &s_words[s_sel_word];
        canvas_rect_fill(c, 12, py + 28, 24, 3, verdict_color(w->verdict));
        snprintf(line, sizeof(line), "word %d: %s", s_sel_word + 1,
                 verdict_text(w->verdict));
        font_draw_string(c, 44, py + 26, &font_tiny, line,
                         verdict_color(w->verdict));
        font_draw_string(c, 12, py + 44, &font_tiny,
                         s_online ? "scored by teacher server"
                                  : "similarity only - offline estimate",
                         THEME_DIM);
        font_draw_string_right(c, CANVAS_WIDTH - 12, py + 44, &font_tiny,
                               s_online ? "online" : "offline",
                               s_online ? THEME_ACTIVE : THEME_DIM);
        break;
    }
    }

    // Keybar per state.
    switch (s_state) {
    case TEA_READY: {
        KeyChip k[4] = {
            { "OK", "LISTEN", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "^v", "AYAH", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { ">", "TRAIN", 2, { INPUT_NAV_RIGHT, INPUT_BTN_MODE } },
            { "BK", "HOME", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 4);
        break;
    }
    case TEA_TRAIN: {
        KeyChip k[4] = {
            { "OK", "RECORD", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "^v", "SKIP/REDO", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "<", "WIFI", 1, { INPUT_NAV_LEFT } },
            { "BK", "EXIT", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 4);
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
            { "^", "YOU", 2, { INPUT_NAV_UP, INPUT_BTN_PLAY } },
            { "v", "RETRY", 2, { INPUT_NAV_DOWN, INPUT_BTN_MODE } },
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
        // RIGHT enters training capture: the device's only physical control
        // is the 5-way (MODE/PLAY exist only as serial-monitor keys).
        case INPUT_NAV_RIGHT:
        case INPUT_BTN_MODE:
            hal_audio_click(true);
            s_train = 0;
            s_ready_hint = NULL;
            s_state = TEA_TRAIN;
            break;
        case INPUT_NAV_LEFT:   // takes still in RAM? share them from here too
            share_takes();
            break;
        case INPUT_BTN_BACK: scene_switch(SCENE_HOME); break;
        default: break;
        }
        break;

    case TEA_TRAIN:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            start_recite(); break;   // finish_recite saves + advances
        case INPUT_NAV_UP: case INPUT_ENC_CCW:
            if (s_train > 0) { s_train--; hal_audio_click(false); }
            s_ready_hint = NULL;
            break;
        case INPUT_NAV_DOWN: case INPUT_ENC_CW:
            if (s_train < TRAIN_TOTAL - 1) { s_train++; hal_audio_click(false); }
            s_ready_hint = NULL;
            break;
        case INPUT_NAV_LEFT:   // share the RAM takes over Wi-Fi
            share_takes();
            break;
        case INPUT_BTN_BACK:
            s_train = -1;
            s_ready_hint = NULL;
            s_state = TEA_READY;
            break;
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
            hal_mic_stop();
            s_state = (s_train >= 0) ? TEA_TRAIN : TEA_READY;
            break;
        default: break;
        }
        break;

    case TEA_REVIEW:
        switch (e.type) {
        // 5-way-only friendly: <> = word, OK = teacher, UP = your own take,
        // DOWN = retry. (PLAY/MODE still work from the sim/serial keys.)
        case INPUT_NAV_LEFT: case INPUT_ENC_CCW: review_move(-1); break;
        case INPUT_NAV_RIGHT: case INPUT_ENC_CW: review_move(+1); break;
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH:
            hal_audio_click(true); play_teacher_word(); break;
        case INPUT_NAV_UP:
        case INPUT_BTN_PLAY:
            hal_audio_click(true); play_user_word(); break;
        case INPUT_NAV_DOWN:
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
