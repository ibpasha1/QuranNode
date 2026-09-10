// hifz_drill_test.c — deterministic unit test for the talqeen drill machine.
//
// Reuses loop_test.c's virtual-clip + controllable-clock harness (no SDL, no
// real audio) and the real surah-1 timings on the SD card, so we can assert the
// EXACT phase sequence, the sliding connect windows, the NO/SHAKY branches, the
// grade bookkeeping, and that g_player is restored — all without a device.
#include "hifz_drill.h"
#include "player.h"
#include "quran_db.h"
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- controllable clock ---
static uint32_t g_clock = 0;
uint32_t plat_millis(void) { return g_clock; }

// --- filesystem (timing_open slurps quran/timings/<s>.qtm) ---
bool hal_fs_slurp(const char *rel, uint8_t **out, size_t *len)
{
    char p[512]; snprintf(p, sizeof(p), "sdcard/%s", rel);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return false; }
    fclose(f); *out = b; *len = sz; return true;
}
bool hal_fs_exists(const char *r){ (void)r; return true; }
int  hal_fs_list(const char *r, char n[][64], int m, bool d){ (void)r;(void)n;(void)m;(void)d; return 0; }
void hal_display_push(const uint16_t *fb){ (void)fb; }
bool hal_input_poll(InputEvent *e){ (void)e; return false; }
bool hal_init(void){ return true; }
void hal_shutdown(void){}
bool hal_running(void){ return true; }
void hal_audio_click(bool b){ (void)b; }

// --- virtual audio clip (identical model to loop_test.c) ---
#define CLIP_LEN 1000u
struct HalAudioClip { int _u; };
static struct HalAudioClip s_clip;
static double s_pos; static bool s_playing; static float s_rate = 1.0f;

HalAudioClip *hal_audio_open(const char *r){ (void)r; s_pos = 0; return &s_clip; }
void hal_audio_close(HalAudioClip *c){ (void)c; s_playing = false; }
void hal_audio_play(HalAudioClip *c){ (void)c; s_playing = true; }
void hal_audio_pause(HalAudioClip *c){ (void)c; s_playing = false; }
bool hal_audio_is_playing(HalAudioClip *c){ (void)c; return s_playing && s_pos < CLIP_LEN; }
uint32_t hal_audio_pos_ms(HalAudioClip *c){ (void)c; return (uint32_t)s_pos; }
uint32_t hal_audio_len_ms(HalAudioClip *c){ (void)c; return CLIP_LEN; }
uint32_t hal_audio_latency_ms(HalAudioClip *c){ (void)c; return 0; }
void hal_audio_seek_ms(HalAudioClip *c, uint32_t m){ (void)c; s_pos = m; }
void hal_audio_set_rate(HalAudioClip *c, float r){ (void)c; s_rate = r; }
void hal_audio_set_volume(float v){ (void)v; }

static void advance(uint32_t dt)
{
    if (s_playing && s_pos < CLIP_LEN) s_pos += (double)dt * s_rate;
    g_clock += dt;
}

// -------------------------------------------------------------------------
// A trace of phase transitions, so tests can assert the exact sequence.
// -------------------------------------------------------------------------
#define TRACE_MAX 400
typedef struct { DrillPhase ph; int seg, w0, w1; } Ev;
static Ev  s_tr[TRACE_MAX];
static int s_ntr;
static DrillPhase s_last = (DrillPhase)-1;

static void note(void)
{
    DrillPhase ph = hifz_drill_phase();
    if (ph == s_last) return;
    if (s_ntr < TRACE_MAX) {
        int w0 = 0, w1 = 0; hifz_drill_window(&w0, &w1);
        s_tr[s_ntr++] = (Ev){ ph, hifz_drill_seg(), w0, w1 };
    }
    s_last = ph;
}

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *PN[] = { "LISTEN","ECHO","FADE1","FADE2","RECALL","ASSESS",
                            "CONNECT","CHUNK_CONNECT","DONE" };

// A grade policy: given the segment and how many times it has been assessed so
// far, return the grade to give. NULL grade fn never reached (test bug).
typedef HifzGrade (*GradeFn)(int seg, int nth);

// Drive the machine to completion. DT is huge so every timed phase and every
// audio playthrough resolves in a single tick — one phase transition per tick.
static void drive(GradeFn grade)
{
    const uint32_t DT = 20000;
    int assess_n[HIFZ_DRILL_MAX_SEGS] = {0};
    s_ntr = 0; s_last = (DrillPhase)-1;
    note();   // initial phase (LISTEN)
    for (int i = 0; i < 3000 && !hifz_drill_done(); i++) {
        advance(DT);
        hifz_drill_tick(g_clock);
        note();
        if (hifz_drill_phase() == DRILL_ASSESS) {
            int seg = hifz_drill_seg();
            HifzGrade g = grade(seg, assess_n[seg]++);
            hifz_drill_grade(g, g_clock);
            note();   // capture the post-grade phase (e.g. CONNECT) before it ticks away
        }
    }
    note();
}

// Count trace events matching a phase (optionally for a given segment).
static int count_phase(DrillPhase ph, int seg /* -1 = any */)
{
    int n = 0;
    for (int i = 0; i < s_ntr; i++)
        if (s_tr[i].ph == ph && (seg < 0 || s_tr[i].seg == seg)) n++;
    return n;
}

static void print_trace(void)
{
    printf("  trace:");
    for (int i = 0; i < s_ntr; i++)
        printf(" %s[s%d]", PN[s_tr[i].ph], s_tr[i].seg);
    printf("\n");
}

// Build a chunk of `n` consecutive whole-ayah segments starting at ayah `a0`.
static int whole_ayah_segs(int surah, int a0, int n, HifzSeg *out)
{
    for (int i = 0; i < n; i++) {
        int a = a0 + i;
        out[i].a0 = out[i].a1 = (int16_t)a;
        out[i].w0 = 0;
        out[i].w1 = (int16_t)(qdb_word_count(surah, a) - 1);
    }
    return n;
}

static HifzGrade all_got(int seg, int nth) { (void)seg; (void)nth; return HZ_GOT; }
static HifzGrade no_seg0(int seg, int nth) { (void)nth; return seg == 0 ? HZ_NO : HZ_GOT; }
static HifzGrade all_shaky(int seg, int nth) { (void)seg; (void)nth; return HZ_SHAKY; }

// -------------------------------------------------------------------------
int main(void)
{
    HifzSeg segs[8];
    HifzDrillCfg cfg = { .listen_reps = 1, .echo_reps = 1, .pace_pct = 115 };

    printf("-- all-GOT chunk: exact phase sequence --\n");
    {
        int n = whole_ayah_segs(1, 1, 3, segs);
        player_init(&g_player, "abdulbasit");
        hifz_drill_begin(1, segs, n, &cfg);
        drive(all_got);
        print_trace();
        // seg0: LISTEN ECHO FADE1 FADE2 RECALL ASSESS  (no connect, k==0)
        // seg1: LISTEN ECHO FADE1 FADE2 RECALL ASSESS CONNECT
        // seg2: LISTEN ECHO FADE1 FADE2 RECALL ASSESS CONNECT
        //       CHUNK_CONNECT DONE
        DrillPhase want[] = {
            DRILL_LISTEN, DRILL_ECHO, DRILL_FADE1, DRILL_FADE2, DRILL_RECALL, DRILL_ASSESS,
            DRILL_LISTEN, DRILL_ECHO, DRILL_FADE1, DRILL_FADE2, DRILL_RECALL, DRILL_ASSESS, DRILL_CONNECT,
            DRILL_LISTEN, DRILL_ECHO, DRILL_FADE1, DRILL_FADE2, DRILL_RECALL, DRILL_ASSESS, DRILL_CONNECT,
            DRILL_CHUNK_CONNECT, DRILL_DONE,
        };
        int wn = (int)(sizeof want / sizeof want[0]);
        CHECK(s_ntr == wn, "trace len %d, want %d", s_ntr, wn);
        for (int i = 0; i < wn && i < s_ntr; i++)
            CHECK(s_tr[i].ph == want[i], "step %d: got %s want %s",
                  i, PN[s_tr[i].ph], PN[want[i]]);
        CHECK(hifz_drill_done(), "not done");
        CHECK(hifz_drill_portion_grade() == HZ_GOT, "portion grade not GOT");
        for (int k = 0; k < n; k++)
            CHECK(hifz_drill_seg_grade(k) == HZ_GOT, "seg %d not GOT", k);
        // one grade per segment: no segment graded twice, none left pending
        CHECK(count_phase(DRILL_ASSESS, -1) == n, "assessed %d times, want %d",
              count_phase(DRILL_ASSESS, -1), n);
    }

    printf("-- sliding connect windows [max(0,k-2)..k], not restart-from-0 --\n");
    {
        int n = whole_ayah_segs(1, 1, 4, segs);
        player_init(&g_player, "abdulbasit");
        hifz_drill_begin(1, segs, n, &cfg);
        drive(all_got);
        print_trace();
        // Find each CONNECT and check its window.
        int exp0[] = { 0, 0, 1 };   // seg1->[0,1] seg2->[0,2] seg3->[1,3]
        int exp1[] = { 1, 2, 3 };
        int ci = 0;
        for (int i = 0; i < s_ntr; i++) {
            if (s_tr[i].ph != DRILL_CONNECT) continue;
            CHECK(ci < 3, "too many CONNECTs");
            if (ci < 3) {
                CHECK(s_tr[i].w0 == exp0[ci] && s_tr[i].w1 == exp1[ci],
                      "connect %d window [%d,%d] want [%d,%d]",
                      ci, s_tr[i].w0, s_tr[i].w1, exp0[ci], exp1[ci]);
            }
            ci++;
        }
        CHECK(ci == 3, "%d connects, want 3", ci);
        // full-chunk connect spans everything
        int fc = -1;
        for (int i = 0; i < s_ntr; i++)
            if (s_tr[i].ph == DRILL_CHUNK_CONNECT) fc = i;
        CHECK(fc >= 0 && s_tr[fc].w0 == 0 && s_tr[fc].w1 == n - 1,
              "chunk connect window wrong");
    }

    printf("-- NO re-loops at most twice, then advances marked weak --\n");
    {
        int n = whole_ayah_segs(1, 1, 2, segs);
        player_init(&g_player, "abdulbasit");
        hifz_drill_begin(1, segs, n, &cfg);
        // seg0: always NO; seg1: GOT.
        drive(no_seg0);
        print_trace();
        // seg0 entered LISTEN 3 times: initial + 2 re-loops, then advance.
        CHECK(count_phase(DRILL_LISTEN, 0) == 3, "seg0 LISTEN x%d, want 3",
              count_phase(DRILL_LISTEN, 0));
        CHECK(count_phase(DRILL_ASSESS, 0) == 3, "seg0 ASSESS x%d, want 3",
              count_phase(DRILL_ASSESS, 0));
        CHECK(hifz_drill_seg_grade(0) == HZ_NO, "seg0 not marked weak (NO)");
        CHECK(hifz_drill_portion_grade() == HZ_NO, "portion not NO");
    }

    printf("-- SHAKY inserts exactly one extra ECHO+RECALL --\n");
    {
        int n = whole_ayah_segs(1, 1, 1, segs);
        player_init(&g_player, "abdulbasit");
        hifz_drill_begin(1, segs, n, &cfg);
        drive(all_shaky);
        print_trace();
        // Normal path has ECHO once + RECALL once; the SHAKY reinforcement adds
        // exactly one more of each -> 2 ECHO, 2 RECALL, and still 1 ASSESS.
        CHECK(count_phase(DRILL_ECHO, 0) == 2, "ECHO x%d, want 2", count_phase(DRILL_ECHO, 0));
        CHECK(count_phase(DRILL_RECALL, 0) == 2, "RECALL x%d, want 2", count_phase(DRILL_RECALL, 0));
        CHECK(count_phase(DRILL_ASSESS, 0) == 1, "ASSESS x%d, want 1", count_phase(DRILL_ASSESS, 0));
        CHECK(hifz_drill_seg_grade(0) == HZ_SHAKY, "seg0 not SHAKY");
    }

    printf("-- peeking caps a GOT at SHAKY --\n");
    {
        int n = whole_ayah_segs(1, 1, 1, segs);
        player_init(&g_player, "abdulbasit");
        hifz_drill_begin(1, segs, n, &cfg);
        // Drive by hand: run until RECALL, peek, then grade GOT at ASSESS.
        const uint32_t DT = 20000;
        bool peeked = false;
        for (int i = 0; i < 3000 && !hifz_drill_done(); i++) {
            advance(DT);
            hifz_drill_tick(g_clock);
            if (hifz_drill_phase() == DRILL_RECALL && !peeked) {
                hifz_drill_peek(g_clock);
                peeked = true;
            }
            if (hifz_drill_phase() == DRILL_ASSESS)
                hifz_drill_grade(HZ_GOT, g_clock);
        }
        CHECK(peeked, "never reached RECALL to peek");
        CHECK(hifz_drill_peeks() >= 1, "peek not counted");
        CHECK(hifz_drill_seg_grade(0) == HZ_SHAKY, "GOT after peek not capped to SHAKY");
    }

    printf("-- hifz_drill_end restores g_player surah/ayah/rate --\n");
    {
        player_init(&g_player, "abdulbasit");
        player_load(&g_player, 1, 4);
        player_set_rate(&g_player, 0.9f);
        int save_surah = g_player.surah, save_ayah = g_player.ayah;
        float save_rate = g_player.rate;

        int n = whole_ayah_segs(1, 1, 2, segs);
        hifz_drill_begin(1, segs, n, &cfg);
        drive(all_got);
        hifz_drill_end();
        CHECK(g_player.surah == save_surah, "surah %d, want %d", g_player.surah, save_surah);
        CHECK(g_player.ayah == save_ayah, "ayah %d, want %d", g_player.ayah, save_ayah);
        CHECK(g_player.rate == save_rate, "rate %.2f, want %.2f", g_player.rate, save_rate);
    }

    if (fails) { printf("\nhifz-drill-test: %d checks FAILED\n", fails); return 1; }
    printf("\nhifz-drill-test: all checks passed\n");
    return 0;
}
