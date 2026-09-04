// loop_test.c — deterministic unit test for the Ayah Loop engine.
//
// Drives Player's loop state machine with a virtual audio clip (fixed length,
// ends when the playhead reaches it) and a controllable clock, so we can assert
// the exact number of clip plays for a given LoopConfig without real audio.
#include "player.h"
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// --- controllable clock ---
static uint32_t g_clock = 0;
uint32_t plat_millis(void) { return g_clock; }

// --- filesystem (for timing_open) ---
bool hal_fs_slurp(const char *rel, uint8_t **out, size_t *len)
{
    char p[512]; snprintf(p, sizeof(p), "sdcard/%s", rel);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return false; }
    fclose(f); *out = b; *len = sz; return true;
}
bool hal_fs_exists(const char *r){ (void)r; return true; }
int  hal_fs_list(const char *r, char n[][64], int m, bool d){ (void)r;(void)n;(void)m;(void)d; return 0; }
void hal_display_push(const uint16_t *fb){ (void)fb; }
bool hal_input_poll(InputEvent *e){ (void)e; return false; }
bool hal_init(void){ return true; }
void hal_shutdown(void){}
bool hal_running(void){ return true; }

// --- virtual audio clip ---
#define CLIP_LEN 1000u
struct HalAudioClip { int _u; };
static struct HalAudioClip s_clip;
static double s_pos; static bool s_playing; static float s_rate = 1.0f;
static int s_completions = 0;

HalAudioClip *hal_audio_open(const char *r){ (void)r; s_pos = 0; return &s_clip; }
void hal_audio_close(HalAudioClip *c){ (void)c; s_playing = false; }
void hal_audio_play(HalAudioClip *c){ (void)c; s_playing = true; }
void hal_audio_pause(HalAudioClip *c){ (void)c; s_playing = false; }
bool hal_audio_is_playing(HalAudioClip *c){ (void)c; return s_playing && s_pos < CLIP_LEN; }
uint32_t hal_audio_pos_ms(HalAudioClip *c){ (void)c; return (uint32_t)s_pos; }
uint32_t hal_audio_len_ms(HalAudioClip *c){ (void)c; return CLIP_LEN; }
void hal_audio_seek_ms(HalAudioClip *c, uint32_t m){ (void)c; s_pos = m; }
void hal_audio_set_rate(HalAudioClip *c, float r){ (void)c; s_rate = r; }
void hal_audio_set_volume(float v){ (void)v; }

static void advance(uint32_t dt)
{
    if (s_playing && s_pos < CLIP_LEN) {
        double np = s_pos + (double)dt * s_rate;
        if (np >= CLIP_LEN && s_pos < CLIP_LEN) s_completions++;  // clip finished
        s_pos = np;
    }
    g_clock += dt;
}

int main(void)
{
    Player p;
    player_init(&p, "abdulbasit");
    p.surah = 1;

    LoopConfig cfg = { .start_ayah = 1, .end_ayah = 3, .each_reps = 2,
                       .section_reps = 2, .pause_ms = 500, .rate = 1.0f };
    player_start_loop(&p, &cfg);

    int last_a = -1, last_e = -1, last_s = -1;
    for (int step = 0; step < 20000 && p.loop.active; step++) {
        advance(50);
        player_update(&p);
        if (p.ayah != last_a || p.loop_each_i != last_e || p.loop_section_i != last_s) {
            printf("t=%5ums  ayah %d  each_i=%d  section=%d  %s\n",
                   g_clock, p.ayah, p.loop_each_i, p.loop_section_i,
                   p.in_pause ? "(pause)" : "");
            last_a = p.ayah; last_e = p.loop_each_i; last_s = p.loop_section_i;
        }
    }

    int expected = 3 * 2 * 2;   // 3 ayat x each x2 x section x2
    printf("\nplays completed = %d (expected %d)   loop.active=%d\n",
           s_completions, expected, p.loop.active);
    bool ok = (s_completions == expected) && !p.loop.active;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
