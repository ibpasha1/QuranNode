#include "hifz_drill.h"
#include "player.h"
#include "quran_db.h"
#include "timing.h"
#include "hal.h"
#include "plat.h"
#include <string.h>

static const char *TAG = "DRILL";

// -------------------------------------------------------------------------
// State
// -------------------------------------------------------------------------
static int         s_surah;
static HifzSeg     s_segs[HIFZ_DRILL_MAX_SEGS];
static int         s_nsegs;
static int         s_k;             // current segment
static DrillPhase  s_phase;

static uint8_t     s_listen_reps, s_echo_reps;
static uint16_t    s_pace;

// Timed-rep state (ECHO/FADE/RECALL/CONNECT/CHUNK, and audio-less LISTEN).
static int         s_rep_i, s_reps_target;
static uint32_t    s_rep_ms, s_rep_deadline;

// Audio-LISTEN state.
static bool        s_audio;         // LISTEN is driving g_player, not a timer
static uint32_t    s_listen_seq0;   // loop_done_seq snapshot at LISTEN start
static bool        s_seek;          // partial-ayah seek+watchdog playback
static uint32_t    s_seek_a, s_seek_b;
static int         s_seek_reps_left;

// Per-segment run state.
static int         s_no_count;      // NO re-loops used on this segment
static bool        s_reinforce;     // in the extra ECHO+RECALL a SHAKY triggers
static int         s_seg_peeks;     // peeks on the current segment
static bool        s_peeking;       // text currently revealed

// Connect window (segment indices, inclusive).
static int         s_win0, s_win1;

// Results.
static int8_t      s_grade[HIFZ_DRILL_MAX_SEGS];   // -1 = pending
static int         s_peeks_total;

// Saved shared-player state.
static int         s_save_surah, s_save_ayah;
static float       s_save_rate;
static bool        s_active;

// -------------------------------------------------------------------------
// Reference-duration helpers (drive the timed reps and the pace)
// -------------------------------------------------------------------------
static bool seg_is_whole_ayah(const HifzSeg *s)
{
    // A whole-ayah run starts at word 0 and ends at the last word of a1; the
    // in-ayah splits the chunker makes always have a0 == a1.
    return s->w0 == 0 && s->w1 == qdb_word_count(s_surah, s->a1) - 1;
}

static int seg_word_count(const HifzSeg *s)
{
    if (s->a0 == s->a1) return s->w1 - s->w0 + 1;
    int n = qdb_word_count(s_surah, s->a0) - s->w0;
    for (int a = s->a0 + 1; a < s->a1; a++) n += qdb_word_count(s_surah, a);
    return n + s->w1 + 1;
}

static uint32_t ayah_dur_ms(int ayah)
{
    if (!g_player.timing_ok) return 0;
    int n = timing_word_count(&g_player.timing, ayah);
    WordTiming wt;
    if (n > 0 && timing_word(&g_player.timing, ayah, n - 1, &wt)) return wt.end_ms;
    return 0;
}

static uint32_t seg_ref_ms(const HifzSeg *s)
{
    uint32_t ms = 0;
    if (g_player.timing_ok) {
        if (seg_is_whole_ayah(s)) {
            for (int a = s->a0; a <= s->a1; a++) ms += ayah_dur_ms(a);
        } else {
            int a = s->a0;
            // Only trust word-level timing where the timing split agrees with
            // the drawn split; otherwise fall back to the whole ayah.
            if (qdb_words_agree(s_surah, a,
                                timing_word_count(&g_player.timing, a))) {
                WordTiming w0, w1;
                if (timing_word(&g_player.timing, a, s->w0, &w0) &&
                    timing_word(&g_player.timing, a, s->w1, &w1) &&
                    w1.end_ms > w0.start_ms)
                    ms = w1.end_ms - w0.start_ms;
            } else {
                ms = ayah_dur_ms(a);
            }
        }
    }
    if (ms == 0) ms = (uint32_t)seg_word_count(s) * 400u;   // no timing: ~2.5 wps
    return ms;
}

static uint32_t window_ref_ms(int from, int to)
{
    uint32_t ms = 0;
    for (int i = from; i <= to && i < s_nsegs; i++) ms += seg_ref_ms(&s_segs[i]);
    return ms;
}

// -------------------------------------------------------------------------
// Playback (only LISTEN plays audio; every other phase is a silent recite)
// -------------------------------------------------------------------------
static bool play_listen_audio(const HifzSeg *s, int reps)
{
    s_seek = false;
    int a = s->a0;

    // A trustworthy in-ayah split is the only case that needs word-level
    // seeking. Everything else — whole-ayah runs, and partial ayat whose timing
    // split disagrees with the drawn split — plays the ayah range on the loop
    // engine (ayah-level fallback for the disagreeing case).
    bool seekable = !seg_is_whole_ayah(s) &&
                    g_player.timing_ok &&
                    qdb_words_agree(s_surah, a,
                                    timing_word_count(&g_player.timing, a));
    if (!seekable) {
        int end = seg_is_whole_ayah(s) ? s->a1 : a;   // disagreeing split → 1 ayah
        LoopConfig cfg = { .active = false, .start_ayah = s->a0,
                           .end_ayah = end, .each_reps = 1,
                           .section_reps = reps, .pause_ms = 300, .rate = 1.0f };
        g_player.surah = s_surah;          // player_start_loop reads p->surah
        player_start_loop(&g_player, &cfg);
        return g_player.playing;
    }

    WordTiming w0, w1;
    if (!g_player.timing_ok ||
        !timing_word(&g_player.timing, a, s->w0, &w0) ||
        !timing_word(&g_player.timing, a, s->w1, &w1))
        return false;
    if (g_player.surah != s_surah || g_player.ayah != a)
        player_load(&g_player, s_surah, a);
    if (!g_player.clip) return false;
    s_seek = true;
    s_seek_a = w0.start_ms;
    s_seek_b = w1.end_ms;
    s_seek_reps_left = reps;
    hal_audio_set_rate(g_player.clip, 1.0f);
    hal_audio_seek_ms(g_player.clip, s_seek_a);
    hal_audio_play(g_player.clip);
    g_player.playing = true;
    return true;
}

// -------------------------------------------------------------------------
// Phase entry
// -------------------------------------------------------------------------
static void arm_timer(uint32_t now, uint32_t rep_ms, int reps)
{
    s_rep_i = 0;
    s_reps_target = reps < 1 ? 1 : reps;
    s_rep_ms = rep_ms < 100 ? 100 : rep_ms;
    s_rep_deadline = now + s_rep_ms;
}

static uint32_t paced(uint32_t ref) { return (uint32_t)((uint64_t)ref * s_pace / 100u); }

static void enter_listen(uint32_t now)
{
    s_phase = DRILL_LISTEN;
    s_peeking = false;
    s_audio = play_listen_audio(&s_segs[s_k], s_listen_reps);
    if (s_audio) s_listen_seq0 = g_player.loop_done_seq;
    else arm_timer(now, seg_ref_ms(&s_segs[s_k]), s_listen_reps);
}

static void enter_echo(uint32_t now)
{
    s_phase = DRILL_ECHO;
    s_peeking = false;
    arm_timer(now, paced(seg_ref_ms(&s_segs[s_k])), s_echo_reps);
}

static void enter_recall_chain(uint32_t now, DrillPhase p)
{
    s_phase = p;
    s_peeking = false;
    arm_timer(now, paced(seg_ref_ms(&s_segs[s_k])), 1);
}

static void enter_assess(void)
{
    s_phase = DRILL_ASSESS;   // no timer: waits for a grade. Text stays veiled.
}

static void enter_connect(uint32_t now)
{
    s_win0 = s_k - 2 < 0 ? 0 : s_k - 2;
    s_win1 = s_k;
    s_phase = DRILL_CONNECT;
    s_peeking = false;
    arm_timer(now, paced(window_ref_ms(s_win0, s_win1)), 1);
}

static void enter_chunk_connect(uint32_t now)
{
    s_win0 = 0;
    s_win1 = s_nsegs - 1;
    s_phase = DRILL_CHUNK_CONNECT;
    s_peeking = false;
    arm_timer(now, paced(window_ref_ms(s_win0, s_win1)), 1);
}

static void finish(void)
{
    s_phase = DRILL_DONE;
    if (g_player.clip) hal_audio_pause(g_player.clip);
    player_stop_loop(&g_player);
}

// Move past the (already-graded) current segment: stitch it in, then advance.
static void advance_segment(uint32_t now)
{
    s_k++;
    if (s_k >= s_nsegs) { enter_chunk_connect(now); return; }
    s_no_count = 0;
    s_seg_peeks = 0;
    enter_listen(now);
}

static void leave_segment(uint32_t now)
{
    if (s_k > 0) enter_connect(now);   // stitch the seam to what came before
    else advance_segment(now);
}

static void record_grade(HifzGrade g)
{
    // Peeking caps a GOT at SHAKY — the same anti-gaming rule hifz_grade uses
    // for the portion; applied per segment so the heat map can't be inflated.
    if (g == HZ_GOT && s_seg_peeks > 0) g = HZ_SHAKY;
    s_grade[s_k] = (int8_t)g;
}

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------
void hifz_drill_begin(int surah, const HifzSeg *segs, int nsegs,
                      const HifzDrillCfg *cfg)
{
    s_surah = surah;
    s_nsegs = nsegs < 1 ? 0 : (nsegs > HIFZ_DRILL_MAX_SEGS ? HIFZ_DRILL_MAX_SEGS
                                                           : nsegs);
    for (int i = 0; i < s_nsegs; i++) s_segs[i] = segs[i];
    for (int i = 0; i < HIFZ_DRILL_MAX_SEGS; i++) s_grade[i] = -1;

    s_listen_reps = cfg && cfg->listen_reps ? cfg->listen_reps : 4;
    s_echo_reps   = cfg && cfg->echo_reps   ? cfg->echo_reps   : 3;
    s_pace        = cfg && cfg->pace_pct    ? cfg->pace_pct    : 115;

    s_k = 0;
    s_no_count = 0;
    s_reinforce = false;
    s_seg_peeks = 0;
    s_peeks_total = 0;
    s_peeking = false;
    s_win0 = s_win1 = 0;

    // g_player is shared with the reader; remember what to put back.
    s_save_surah = g_player.surah;
    s_save_ayah  = g_player.ayah;
    s_save_rate  = g_player.rate;
    s_active = true;

    // Load the drill surah's timing up front so seg_ref_ms works before any
    // audio (the device only bundles Al-Fatihah, so most drills are timed-only).
    player_load(&g_player, surah, s_segs[0].a0);

    QN_LOGI(TAG, "begin surah %d: %d segs, listen x%u echo x%u pace %u%%",
            surah, s_nsegs, s_listen_reps, s_echo_reps, s_pace);

    if (s_nsegs > 0) enter_listen(0);
    else finish();
}

void hifz_drill_end(void)
{
    if (!s_active) return;
    s_active = false;
    player_stop_loop(&g_player);
    if (g_player.clip) hal_audio_pause(g_player.clip);
    player_load(&g_player, s_save_surah, s_save_ayah);
    player_set_rate(&g_player, s_save_rate);
    QN_LOGI(TAG, "end: restored player to %d:%d", s_save_surah, s_save_ayah);
}

// -------------------------------------------------------------------------
// Phase completion (a timed phase ran out, audio finished, or OK was pressed)
// -------------------------------------------------------------------------
static void on_phase_complete(uint32_t now)
{
    switch (s_phase) {
    case DRILL_LISTEN: enter_echo(now); break;
    case DRILL_ECHO:
        if (s_reinforce) enter_recall_chain(now, DRILL_RECALL);
        else             enter_recall_chain(now, DRILL_FADE1);
        break;
    case DRILL_FADE1:  enter_recall_chain(now, DRILL_FADE2); break;
    case DRILL_FADE2:  enter_recall_chain(now, DRILL_RECALL); break;
    case DRILL_RECALL:
        if (s_reinforce) { s_reinforce = false; leave_segment(now); }
        else             enter_assess();
        break;
    case DRILL_CONNECT:       advance_segment(now); break;
    case DRILL_CHUNK_CONNECT: finish(); break;
    default: break;   // ASSESS / DONE don't complete on a timer
    }
}

void hifz_drill_tick(uint32_t now)
{
    if (!s_active) return;

    if (s_phase == DRILL_LISTEN && s_audio) {
        if (s_seek) {
            if (g_player.clip && hal_audio_pos_ms(g_player.clip) >= s_seek_b) {
                if (--s_seek_reps_left > 0) {
                    hal_audio_seek_ms(g_player.clip, s_seek_a);
                    hal_audio_play(g_player.clip);
                } else {
                    if (g_player.clip) hal_audio_pause(g_player.clip);
                    on_phase_complete(now);
                }
            }
        } else {
            player_update(&g_player);
            if (g_player.loop_done_seq != s_listen_seq0) on_phase_complete(now);
        }
        return;
    }

    // Timed phases (incl. audio-less LISTEN): advance rep by rep.
    switch (s_phase) {
    case DRILL_LISTEN: case DRILL_ECHO: case DRILL_FADE1: case DRILL_FADE2:
    case DRILL_RECALL: case DRILL_CONNECT: case DRILL_CHUNK_CONNECT:
        if (now >= s_rep_deadline) {
            if (++s_rep_i >= s_reps_target) on_phase_complete(now);
            else s_rep_deadline = now + s_rep_ms;
        }
        break;
    default: break;
    }
}

// -------------------------------------------------------------------------
// Input
// -------------------------------------------------------------------------
void hifz_drill_grade(HifzGrade g, uint32_t now)
{
    if (s_phase != DRILL_ASSESS) return;

    if (g == HZ_GOT || g == HZ_SHAKY) {
        record_grade(g);
        if (g == HZ_SHAKY) {           // one extra ECHO+RECALL, then move on
            s_reinforce = true;
            enter_echo(now);
        } else {
            leave_segment(now);
        }
    } else {   // HZ_NO
        if (s_no_count < 2) {          // re-drill from the top, at most twice
            s_no_count++;
            s_seg_peeks = 0;           // fresh attempt
            enter_listen(now);
        } else {                       // give up: mark it weak and advance
            record_grade(HZ_NO);
            leave_segment(now);
        }
    }
}

void hifz_drill_skip(uint32_t now)
{
    switch (s_phase) {
    case DRILL_LISTEN:
        if (s_audio) { player_stop_loop(&g_player);
                       if (g_player.clip) hal_audio_pause(g_player.clip); }
        on_phase_complete(now);
        break;
    case DRILL_ECHO: case DRILL_FADE1: case DRILL_FADE2:
    case DRILL_RECALL: case DRILL_CONNECT: case DRILL_CHUNK_CONNECT:
        on_phase_complete(now);
        break;
    default: break;
    }
}

void hifz_drill_repeat(uint32_t now)
{
    switch (s_phase) {
    case DRILL_LISTEN:
        enter_listen(now);
        break;
    case DRILL_ECHO: case DRILL_FADE1: case DRILL_FADE2:
    case DRILL_RECALL: case DRILL_CONNECT: case DRILL_CHUNK_CONNECT:
        s_rep_deadline = now + s_rep_ms;   // restart the current rep's clock
        break;
    default: break;
    }
}

void hifz_drill_peek(uint32_t now)
{
    (void)now;
    // Only meaningful once the text is (partly) hidden.
    if (s_phase == DRILL_FADE1 || s_phase == DRILL_FADE2 ||
        s_phase == DRILL_RECALL || s_phase == DRILL_ASSESS ||
        s_phase == DRILL_CONNECT || s_phase == DRILL_CHUNK_CONNECT) {
        if (!s_peeking) { s_seg_peeks++; s_peeks_total++; }
        s_peeking = true;
    }
}

bool hifz_drill_peeking(void) { return s_peeking; }

// -------------------------------------------------------------------------
// View + result
// -------------------------------------------------------------------------
DrillPhase hifz_drill_phase(void) { return s_phase; }
int  hifz_drill_seg(void)   { return s_k; }
int  hifz_drill_nsegs(void) { return s_nsegs; }
int  hifz_drill_surah(void) { return s_surah; }

const HifzSeg *hifz_drill_cur_seg(void)
{
    int k = (s_k < s_nsegs) ? s_k : (s_nsegs ? s_nsegs - 1 : 0);
    return s_nsegs ? &s_segs[k] : NULL;
}

void hifz_drill_window(int *from, int *to)
{
    if (from) *from = s_win0;
    if (to)   *to   = s_win1;
}

void hifz_drill_veil(VeilMode *mode, int *pct)
{
    VeilMode m = VEIL_NONE; int p = 0;
    if (!s_peeking) {
        switch (s_phase) {
        case DRILL_FADE1: m = VEIL_TAIL; p = 33;  break;
        case DRILL_FADE2: m = VEIL_TAIL; p = 66;  break;
        case DRILL_RECALL: case DRILL_ASSESS:
        case DRILL_CONNECT: case DRILL_CHUNK_CONNECT:
            m = VEIL_TAIL; p = 100; break;
        default: break;   // LISTEN / ECHO show the text
        }
    }
    if (mode) *mode = m;
    if (pct)  *pct  = p;
}

int hifz_drill_rep(void)  { return s_rep_i; }
int hifz_drill_reps(void) { return s_reps_target; }

uint32_t hifz_drill_rep_remaining(uint32_t now)
{
    if (s_phase == DRILL_LISTEN && s_audio) return 0;
    return now < s_rep_deadline ? s_rep_deadline - now : 0;
}

bool hifz_drill_done(void) { return s_phase == DRILL_DONE; }

HifzGrade hifz_drill_portion_grade(void)
{
    HifzGrade worst = HZ_GOT;
    bool any = false;
    for (int i = 0; i < s_nsegs; i++) {
        if (s_grade[i] < 0) continue;
        any = true;
        if ((HifzGrade)s_grade[i] < worst) worst = (HifzGrade)s_grade[i];
    }
    return any ? worst : HZ_NO;
}

int hifz_drill_seg_grade(int k)
{
    if (k < 0 || k >= s_nsegs) return -1;
    return s_grade[k];
}

int hifz_drill_peeks(void) { return s_peeks_total; }
