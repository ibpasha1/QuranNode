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
    TEA_READY,      // ayah loaded; prompt to start
    TEA_PICK,       // choose surah + ayah range for the read-along
    TEA_LISTEN,     // teacher playing the ayah
    TEA_RECITE,     // mic open, user reciting (live verdicts settle here)
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
static float s_listen_scroll;     // karaoke follow-scroll for a tall ayah in LISTEN

// Live score-follower: streams the take AS it's recited and lights each word
// green/amber/red a beat behind the reciter (no separate analyze pass). Armed
// lazily on the first recite of an ayah so plain ^v nav stays snappy.
static ReciteLive *s_live;
static int s_live_ayah = -1;      // surah*1000+ayah the follower is armed for

// Read-along trade-off session: the reciter and the user take turns through a
// range of ayat. REPEAT = user echoes each ayah the reciter just read; TAKE-
// TURNS = they alternate ayat. `s_run` is set while a session is in progress.
enum { STYLE_REPEAT = 0, STYLE_TURNS = 1 };
static int  s_style = STYLE_REPEAT;
static int  s_range_end = 1;      // last ayah of the current session
static bool s_run;                // a trade-off session is active

// Turn size: each turn covers one ayah, or a whole mushaf page (several ayat
// read in a row before handing off). s_turn_start/end are the current turn's
// ayah span.
enum { UNIT_AYAH = 0, UNIT_PAGE = 1 };
static int  s_unit = UNIT_AYAH;
static int  s_turn_start = 1, s_turn_end = 1;
static uint32_t s_feed_base;      // page read-through: follower feeds s_rec from here

// In-mode picker: choose surah + [from..to] (+ turn size) to read, instead of
// inheriting the global Quran resume point.
static int s_pick_surah = 1, s_pick_from = 1, s_pick_to = 1, s_pick_row;

static void load_ayah(int surah, int ayah)
{
    if (s_clip) { hal_audio_close(s_clip); s_clip = NULL; }
    if (s_timing_ok) { timing_close(&s_timing); s_timing_ok = false; }
    hal_pcm_stop();
    s_surah = surah; s_ayah = ayah;
    s_rec_n = 0; s_ref_n = 0; s_nwords = 0; s_sel_word = 0; s_seg_stop_ms = 0;
    s_listen_scroll = 0;
    s_live_ayah = -1;   // re-arm the follower on the next recite of this ayah

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
    if (!s_live) s_live = recite_live_create();
    ResumePoint r = progress_has_resume() ? progress_resume()
                                          : (ResumePoint){ 1, 1, 1.0f };
    load_ayah(r.surah, r.ayah);
    s_range_end = qdb_ayah_count(s_surah);   // default: from here to end of surah
    // Open the picker first — choosing surah/range is the front door of Recite.
    s_pick_surah = s_surah; s_pick_from = s_ayah;
    s_pick_to = qdb_ayah_count(s_surah); s_pick_row = 0;
    if (s_state != TEA_NO_DATA) s_state = TEA_PICK;
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
    recite_live_destroy(s_live); s_live = NULL; s_live_ayah = -1;
    // Keep s_rec/s_ref/pack resident: re-entry is common, sim/PSRAM have room.
}

static void start_listen(void)
{
    hal_pcm_stop();
    s_seg_stop_ms = 0;
    s_listen_scroll = 0;
    hal_audio_seek_ms(s_clip, 0);
    hal_audio_set_rate(s_clip, 1.0f);
    hal_audio_play(s_clip);
    s_state = TEA_LISTEN;
}

// Arm the live follower for the current ayah (extract the reference features
// once; a re-record of the same ayah just resets). Practice mode only.
static void arm_live(void)
{
    if (!s_live || s_train >= 0) return;
    int key = s_surah * 1000 + s_ayah;
    if (s_live_ayah == key) { recite_live_reset(s_live); return; }
    if (s_ref_n == 0)
        s_ref_n = hal_audio_read_pcm16(s_clip, 0, s_ref, REF_MAX_N, &s_ref_hz);
    s_nwords = timing_word_count(&s_timing, s_ayah);
    if (s_nwords > MAX_WORDS) s_nwords = MAX_WORDS;
    WordTiming wt[MAX_WORDS];
    for (int i = 0; i < s_nwords; i++) timing_word(&s_timing, s_ayah, i, &wt[i]);
    if (s_ref_n && recite_live_begin(s_live, s_ref, s_ref_n, s_ref_hz, wt, s_nwords))
        s_live_ayah = key;
}

static void start_recite(void)
{
    hal_audio_pause(s_clip);
    // Reserve the mic's I2S DMA FIRST. On the ESP32 that DMA must live in
    // internal RAM, and arming the follower beforehand (reference decode + its
    // feature buffers) can starve it, failing i2s_new_channel -> "No mic".
    if (!hal_mic_start(MIC_HZ)) { s_state = TEA_NO_MIC; return; }
    arm_live();   // now safe to extract ref features (mic buffers meanwhile)
    if (s_live) recite_live_set_follow(s_live, true);   // reading-aid tracker
    s_rec_n = 0;
    s_feed_base = 0;
    s_level = 0;
    s_listen_scroll = 0;
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

// Last ayah of the turn that starts at `from`: itself for ayah-turns, or the
// last ayah still on the same mushaf page (clamped to the session end).
static int compute_turn_end(int from)
{
    if (s_unit == UNIT_AYAH || from >= s_range_end) return from;
    int page = qdb_page_of(s_surah, from), e = from;
    while (e < s_range_end && qdb_page_of(s_surah, e + 1) == page) e++;
    return e;
}

static void end_session(const char *hint)
{
    s_run = false;
    hal_mic_stop();
    hal_audio_pause(s_clip);
    s_ready_hint = hint;
    s_state = TEA_READY;
}

// Begin a turn at ayah `from`; reciter=true => the reciter reads it (LISTEN),
// false => your turn (RECITE). Past the range end -> the session is done.
static void begin_turn(int from, bool reciter)
{
    if (from > s_range_end) { end_session("Finished - masha'Allah"); return; }
    if (from != s_ayah || s_state == TEA_NO_DATA) load_ayah(s_surah, from);
    if (s_state == TEA_NO_DATA) { end_session("No audio for that range"); return; }
    s_turn_start = from;
    s_turn_end = compute_turn_end(from);
    if (reciter) start_listen();
    else         start_recite();
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
    if (s_run) {
        // Still ayat left in YOUR turn (page turns span several)? recite on.
        if (s_ayah < s_turn_end) { load_ayah(s_surah, s_ayah + 1); start_recite(); return; }
        // Your turn done -> the reciter takes the next turn (both styles).
        begin_turn(s_turn_end + 1, true);
        return;
    }
    s_ready_hint = "Recited - go again or pick another ayah";
    s_state = TEA_READY;
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

// Advance to the next ayah of a PAGE turn WITHOUT stopping the mic or clicking
// — a continuous read-through. The recording keeps running; the new ayah's
// follower consumes audio from here on (s_feed_base).
static void seamless_next_ayah(void)
{
    s_ayah++;
    if (s_clip) { hal_audio_close(s_clip); s_clip = NULL; }
    char path[64];
    snprintf(path, sizeof(path), "audio/%s/%d/%d.mp3", "abdulbasit", s_surah, s_ayah);
    s_clip = hal_audio_open(path);
    s_ref_n = 0;            // re-read the reference PCM for the new ayah
    s_live_ayah = -1;
    arm_live();             // begin the follower on the new ayah (same surah pack/timing)
    if (s_live) recite_live_set_follow(s_live, true);
    s_listen_scroll = 0;
    s_feed_base = s_rec_n;  // the new ayah's follower feeds from here on
}

static void on_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    switch (s_state) {
    case TEA_LISTEN:
        if (!hal_audio_is_playing(s_clip)) {
            if (s_run) {
                // Still ayat left in the reciter's turn (page turns)? play on.
                if (s_ayah < s_turn_end) { load_ayah(s_surah, s_ayah + 1); start_listen(); break; }
                // Reciter's turn done: REPEAT -> you echo the SAME turn;
                // TAKE-TURNS -> you do the NEXT turn.
                if (s_style == STYLE_REPEAT) begin_turn(s_turn_start, false);
                else                         begin_turn(s_turn_end + 1, false);
                break;
            }
            start_recite();
        }
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
        // Stream the voiced span into the follower so words light up live.
        // Bounded to the voiced end (+tailpad) so trailing silence doesn't get
        // charged to the last word as "missing".
        if (s_live && s_train < 0 && s_live_ayah == s_surah * 1000 + s_ayah) {
            uint32_t a, b;
            va_span(&s_va, s_rec_n, &a, &b);
            if (b > s_feed_base)
                recite_live_feed(s_live, s_rec + s_feed_base, b - s_feed_base, MIC_HZ);
            // Seamless page read-through: when the cursor finishes this ayah and
            // the turn has more ayat, glide to the next with no pause / re-click.
            if (s_run && s_ayah < s_turn_end && s_nwords > 0 &&
                recite_live_cursor_word(s_live) >= s_nwords - 1 &&
                (s_rec_n - s_feed_base) > (uint32_t)(MIC_HZ / 2)) {
                seamless_next_ayah();
            }
        }
        if (r != VA_RUNNING) finish_recite();
        break;
    }
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

// Ayah 1 of every surah but 1 (which IS the basmala) and 9 (which has none) is
// drawn with a leading 4-word "Bismillah…" that the reciter's timing/audio do
// not carry (the clip starts at the ayah's own first word).
#define BASMALA_WORDS 4

// Map a reciter timing-word index onto the DRAWN glyph words — mirrors the
// reader's hl_word(). Without this the marks land on the basmala while the
// audio recites the actual ayah (Al-Masad: text shows Bismillah, audio doesn't).
// Returns -1 when there's nothing to place.
static int tea_glyph_of(int tw_idx)
{
    if (tw_idx < 0) return -1;
    int tw = timing_word_count(&s_timing, s_ayah);
    if (tw <= 0) return -1;
    if (qdb_words_agree(s_surah, s_ayah, tw)) return tw_idx;   // line up 1:1
    int dw = qdb_word_count(s_surah, s_ayah);
    if (dw <= 0) return -1;
    // Ayah 1 carries the basmala prefix the timing lacks: skip past it.
    if (s_ayah == 1 && s_surah != 1 && s_surah != 9 && dw == tw + BASMALA_WORDS)
        return tw_idx + BASMALA_WORDS;
    // Splits disagree some other way: sweep proportionally so it still tracks.
    int mapped = (int)(((float)tw_idx + 0.5f) * (float)dw / (float)tw);
    if (mapped < 0) mapped = 0;
    if (mapped >= dw) mapped = dw - 1;
    return mapped;
}

// Draw the ayah centered in the band; in review, underline each recited word in
// its verdict color and box the selected word (timing words mapped to glyphs).
static int draw_ayah_marked(Canvas *c, int band_top, int band_bot, bool marked)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, s_surah, s_ayah, &g)) return -1;
    int top = band_top + (band_bot - band_top - g.h) / 2;
    if (top < band_top) top = band_top;
    int x = (CANVAS_WIDTH - g.w) / 2;
    arabic_draw_ayah(c, x, top, &g, THEME_TEXT,
                     marked ? tea_glyph_of(s_sel_word) : -1, THEME_PLAYHEAD);
    if (marked) {
        for (int w = 0; w < s_nwords; w++) {
            int gi = tea_glyph_of(w);
            AtWordBox b;
            if (gi < 0 || gi >= g.n_words || !ayah_word_box(&g, gi, &b)) continue;
            int uy = top + b.y + b.h + 2;
            if (uy > band_bot - 2) uy = band_bot - 2;
            canvas_rect_fill(c, x + b.x, uy, b.w, 3,
                             verdict_color(s_words[w].verdict));
            if (w == s_sel_word)
                canvas_rect(c, x + b.x - 2, top + b.y - 2, b.w + 4, b.h + 8,
                            THEME_ACCENT);
        }
    }
    return top;
}

// Opaque status band below `band_bot`: masks any overflow of a tall ayah and
// gives the bottom prompt a clean backdrop. REVIEW paints its own richer panel.
static void draw_status_band(Canvas *c, int band_bot)
{
    int h = (CANVAS_HEIGHT - THEME_KEYBAR_H) - band_bot;
    canvas_rect_fill(c, 0, band_bot, CANVAS_WIDTH, h, THEME_PANEL);
    canvas_hline(c, 0, band_bot, CANVAS_WIDTH, THEME_GRID);
}

// Colour the words already passed (0..hl) with a "read" underline so progress
// is visible in the read-along — position/coverage, NOT accuracy grading. Bars
// outside the band are skipped (the header/status band cover the overflow).
static void draw_read_progress(Canvas *c, int x, int top, const AyahGlyphs *g,
                               int hl, int band_top, int band_bot)
{
    if (hl < 0) return;
    int n = hl < g->n_words ? hl : g->n_words - 1;
    for (int i = 0; i <= n; i++) {
        AtWordBox b;
        if (!ayah_word_box(g, i, &b)) continue;
        int uy = top + b.y + b.h + 2;
        if (uy < band_top || uy > band_bot - 2) continue;
        canvas_rect_fill(c, x + b.x, uy, b.w, 3, THEME_ACTIVE);
    }
}

// LISTEN/READY/RECITE view of the ayah: box the current word (hl, -1 = none),
// trail a green "read so far" underline behind it, and — when the ayah is taller
// than the band — glide a karaoke follow-scroll keeping that word ~42% down.
static void draw_ayah_listen(Canvas *c, int band_top, int band_bot, int hl, bool playing)
{
    AyahGlyphs g;
    if (!s_pack_ok || !glyphpack_get(&s_pack, s_surah, s_ayah, &g)) return;
    int view_h = band_bot - band_top;
    int x = (CANVAS_WIDTH - g.w) / 2;

    if (g.h <= view_h) {                 // fits: center, no scroll
        s_listen_scroll = 0;
        int top = band_top + (view_h - g.h) / 2;
        arabic_draw_ayah(c, x, top, &g, THEME_TEXT, hl, THEME_PLAYHEAD);
        draw_read_progress(c, x, top, &g, hl, band_top, band_bot);
        return;
    }

    // Taller than the band: follow the recited word, eased.
    int max_scroll = g.h - view_h;
    int target = (int)s_listen_scroll;   // hold between words / at ayah end
    AtWordBox b;
    if (playing && hl >= 0 && ayah_word_box(&g, hl, &b))
        target = (b.y + b.h / 2) - (int)(view_h * 0.42f);
    if (target < 0) target = 0;
    if (target > max_scroll) target = max_scroll;
    float d = (float)target - s_listen_scroll; if (d < 0) d = -d;
    if (d < 0.75f) s_listen_scroll = (float)target;
    else           s_listen_scroll += ((float)target - s_listen_scroll) * 0.25f;

    int top = band_top - (int)(s_listen_scroll + 0.5f);
    arabic_draw_ayah(c, x, top, &g, THEME_TEXT, hl, THEME_PLAYHEAD);
    draw_read_progress(c, x, top, &g, hl, band_top, band_bot);

    // Repaint the header over glyphs that scrolled up under it.
    canvas_rect_fill(c, 0, 0, CANVAS_WIDTH, band_top, THEME_BG);
    char ref[24];
    snprintf(ref, sizeof ref, "%d:%d", s_surah, s_ayah);
    theme_header(c, "RECITE", THEME_TITLE, ref, THEME_LABEL);

    // Slim scrollbar: how far through the ayah we are.
    int thumb_h = view_h * view_h / g.h; if (thumb_h < 12) thumb_h = 12;
    int thumb_y = band_top + (int)((view_h - thumb_h) * (s_listen_scroll / (float)max_scroll));
    canvas_rect_fill(c, CANVAS_WIDTH - 3, band_top, 2, view_h, THEME_GRID);
    canvas_rect_fill(c, CANVAS_WIDTH - 3, thumb_y, 2, thumb_h, THEME_ACCENT);
}

static void on_render(Canvas *c)
{
    theme_clear(c);
    char ref[24];
    snprintf(ref, sizeof(ref), "%d:%d", s_surah, s_ayah);
    theme_header(c, "RECITE", THEME_TITLE, ref, THEME_LABEL);

    int band_top = 26, band_bot = CANVAS_HEIGHT - THEME_KEYBAR_H - 64;

    switch (s_state) {
    case TEA_NO_DATA:
        font_draw_string_centered(c, 200, &font_small, "No audio for this ayah",
                                  THEME_DIM);
        font_draw_string_centered(c, 224, &font_tiny,
                                  "Copy the repo's sdcard/ to the card", THEME_DIM);
        break;
    case TEA_NO_MIC:
        font_draw_string_centered(c, 200, &font_small, "No microphone",
                                  THEME_DIM);
        font_draw_string_centered(c, 224, &font_tiny,
                                  "Mic capture isn't available here yet",
                                  THEME_DIM);
        break;

    case TEA_PICK: {
        const char *lbl[5] = { "Surah", "From ayah", "To ayah", "Turn size", "Start read-along" };
        char val[5][40];
        snprintf(val[0], sizeof val[0], "%d  %s", s_pick_surah, qdb_surah_name(s_pick_surah));
        snprintf(val[1], sizeof val[1], "%d", s_pick_from);
        snprintf(val[2], sizeof val[2], "%d", s_pick_to);
        snprintf(val[3], sizeof val[3], "%s", s_unit == UNIT_PAGE ? "Page" : "Ayah");
        val[4][0] = 0;
        int y = 64;
        for (int i = 0; i < 5; i++) {
            int ry = y + i * 42;
            bool sel = (i == s_pick_row);
            if (sel) theme_sel_block(c, 16, ry, CANVAS_WIDTH - 32, 32);
            color_t fg = sel ? THEME_SEL_TEXT : THEME_TEXT;
            font_draw_string(c, 28, ry + 7, &font_small, lbl[i],
                             sel ? THEME_SEL_TEXT : THEME_DIM);
            if (val[i][0])
                font_draw_string_right(c, CANVAS_WIDTH - 28, ry + 7, &font_small, val[i], fg);
        }
        font_draw_string_centered(c, y + 5 * 42 + 14, &font_tiny,
                                  "< > change   OK next   BK back", THEME_DIM);
        break;
    }

    case TEA_READY:
    case TEA_LISTEN: {
        bool listening = (s_state == TEA_LISTEN);
        // While the teacher recites, light up the current word (rolled back by
        // the output latency so it tracks what's heard). Only when the timing's
        // word split matches the glyphs 1:1 — otherwise the index would be off.
        int hl = -1;
        if (listening && s_timing_ok) {
            uint32_t pos = hal_audio_pos_ms(s_clip);
            uint32_t lat = hal_audio_latency_ms(s_clip);
            int aw = timing_active_word(&s_timing, s_ayah, pos > lat ? pos - lat : 0);
            hl = tea_glyph_of(aw);   // maps past the basmala prefix on ayah 1
        }
        draw_ayah_listen(c, band_top, band_bot, hl, listening);
        draw_status_band(c, band_bot);
        int iy = CANVAS_HEIGHT - THEME_KEYBAR_H - 52;
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
                                  listening ? "your turn is next - follow along"
                                  : s_ready_hint ? s_ready_hint
                                  : s_style == STYLE_REPEAT ? "Repeat after the reciter - OK to start"
                                                            : "Take turns with the reciter - OK to start",
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
        if (s_train < 0) {
            // Reading aid: the highlight + auto-scroll FOLLOW the reader's voice
            // (same karaoke scroll as LISTEN, driven by the live cursor).
            int cur = (s_live && s_live_ayah == s_surah * 1000 + s_ayah)
                        ? tea_glyph_of(recite_live_cursor_word(s_live)) : -1;
            draw_ayah_listen(c, band_top, band_bot, cur, true);
        } else {
            draw_ayah_marked(c, band_top, band_bot, false);
        }
        draw_status_band(c, band_bot);
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
            font_draw_string_centered(c, iy, &font_medium,
                                      s_va.heard ? "FOLLOWING YOU" : "RECITE",
                                      s_va.heard ? THEME_ACTIVE : THEME_BADGE);
            font_draw_string_centered(c, iy + 26, &font_tiny,
                                      s_va.heard ? "reading along - pause when you finish"
                                                 : "start reciting - I'll follow along",
                                      THEME_DIM);
        }
        // Live mic meter.
        theme_meter(c, 40, iy + 38, CANVAS_WIDTH - 80, 8, s_level);
        break;
    }

    case TEA_TRAIN: {
        draw_ayah_marked(c, band_top, band_bot, false);
        draw_status_band(c, band_bot);
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
        KeyChip k[5] = {
            { "OK", "START", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "^v", "AYAH", 4, { INPUT_NAV_UP, INPUT_NAV_DOWN, INPUT_ENC_CW, INPUT_ENC_CCW } },
            { "<", "MODE", 1, { INPUT_NAV_LEFT } },
            { ">", "CHOOSE", 1, { INPUT_NAV_RIGHT } },
            { "BK", "HOME", 1, { INPUT_BTN_BACK } },
        };
        theme_keybar(c, k, 5);
        break;
    }
    case TEA_PICK: {
        KeyChip k[4] = {
            { "^v", "FIELD", 2, { INPUT_NAV_UP, INPUT_NAV_DOWN } },
            { "<>", "CHANGE", 4, { INPUT_NAV_LEFT, INPUT_NAV_RIGHT, INPUT_ENC_CCW, INPUT_ENC_CW } },
            { "OK", "NEXT", 3, { INPUT_NAV_SELECT, INPUT_ENC_PUSH, INPUT_BTN_PLAY } },
            { "BK", "BACK", 1, { INPUT_BTN_BACK } },
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
    s_range_end = qdb_ayah_count(s_surah);   // nudging the start = "from here to end"
}

static void review_move(int dir)
{
    int n = s_sel_word + dir;
    if (n < 0 || n >= s_nwords) return;
    s_sel_word = n;
    hal_audio_click(false);
}

// Adjust the field the picker cursor is on (surah / from / to).
static void pick_adjust(int dir)
{
    switch (s_pick_row) {
    case 0:
        s_pick_surah += dir;
        if (s_pick_surah < 1) s_pick_surah = QDB_SURAH_COUNT;
        if (s_pick_surah > QDB_SURAH_COUNT) s_pick_surah = 1;
        s_pick_from = 1; s_pick_to = qdb_ayah_count(s_pick_surah);
        break;
    case 1: {
        int n = qdb_ayah_count(s_pick_surah);
        s_pick_from += dir;
        if (s_pick_from < 1) s_pick_from = 1;
        if (s_pick_from > n) s_pick_from = n;
        if (s_pick_to < s_pick_from) s_pick_to = s_pick_from;
        break;
    }
    case 2: {
        int n = qdb_ayah_count(s_pick_surah);
        s_pick_to += dir;
        if (s_pick_to < s_pick_from) s_pick_to = s_pick_from;
        if (s_pick_to > n) s_pick_to = n;
        break;
    }
    case 3:
        s_unit = (s_unit == UNIT_AYAH) ? UNIT_PAGE : UNIT_AYAH;
        break;
    }
    hal_audio_click(false);
}

static void on_input(InputEvent e)
{
    switch (s_state) {
    case TEA_READY:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            // Start the read-along session over [current ayah .. s_range_end].
            hal_audio_click(true);
            if (s_range_end < s_ayah || s_range_end > qdb_ayah_count(s_surah))
                s_range_end = qdb_ayah_count(s_surah);
            s_run = true;
            begin_turn(s_ayah, true);
            break;
        case INPUT_NAV_UP: case INPUT_ENC_CCW: change_ayah(-1); break;
        case INPUT_NAV_DOWN: case INPUT_ENC_CW: change_ayah(+1); break;
        case INPUT_NAV_RIGHT:   // choose surah + range to read
            hal_audio_click(true);
            s_pick_surah = s_surah; s_pick_from = s_ayah;
            s_pick_to = qdb_ayah_count(s_surah); s_pick_row = 0;
            s_state = TEA_PICK;
            break;
        case INPUT_BTN_MODE:    // (serial/dev key) training-capture mode
            hal_audio_click(true);
            s_train = 0;
            s_ready_hint = NULL;
            s_state = TEA_TRAIN;
            break;
        case INPUT_NAV_LEFT:   // toggle read-along style
            s_style = (s_style == STYLE_REPEAT) ? STYLE_TURNS : STYLE_REPEAT;
            s_ready_hint = NULL;
            hal_audio_click(false);
            break;
        case INPUT_BTN_BACK: scene_switch(SCENE_HOME); break;
        default: break;
        }
        break;

    case TEA_PICK:
        switch (e.type) {
        case INPUT_NAV_UP:    if (s_pick_row > 0) { s_pick_row--; hal_audio_click(false); } break;
        case INPUT_NAV_DOWN:  if (s_pick_row < 4) { s_pick_row++; hal_audio_click(false); } break;
        case INPUT_NAV_LEFT:  case INPUT_ENC_CCW: pick_adjust(-1); break;
        case INPUT_NAV_RIGHT: case INPUT_ENC_CW:  pick_adjust(+1); break;
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            if (s_pick_row < 4) { s_pick_row++; hal_audio_click(false); }
            else {   // Start row: apply the selection
                hal_audio_click(true);
                load_ayah(s_pick_surah, s_pick_from);
                s_range_end = s_pick_to;
                if (s_state != TEA_NO_DATA) { s_ready_hint = NULL; s_state = TEA_READY; }
            }
            break;
        case INPUT_BTN_BACK: s_state = TEA_READY; break;
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
            end_session(NULL); break;
        default: break;
        }
        break;

    case TEA_RECITE:
        switch (e.type) {
        case INPUT_NAV_SELECT: case INPUT_ENC_PUSH: case INPUT_BTN_PLAY:
            hal_audio_click(true); finish_recite(); break;
        case INPUT_BTN_BACK:
            hal_mic_stop();
            if (s_train >= 0) s_state = TEA_TRAIN;
            else end_session(NULL);
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
