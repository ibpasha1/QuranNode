#include "player.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PLAYER";

#define RATE_MIN 0.5f
#define RATE_MAX 2.0f
#define RATE_STEP 0.05f

// The single shared transport instance.
Player g_player;

void player_init(Player *p, const char *reciter)
{
    memset(p, 0, sizeof(*p));
    p->reciter = reciter;
    p->surah = 1;
    p->ayah = 1;
    p->ayah_min = 1;
    p->ayah_max = 1;
    p->active_word = -1;
    p->rate = 1.0f;
    p->autoplay_next = true;
}

static void open_clip(Player *p)
{
    if (p->clip) { hal_audio_close(p->clip); p->clip = NULL; }
    char path[96];
    snprintf(path, sizeof(path), "audio/%s/%d/%d.mp3", p->reciter, p->surah, p->ayah);
    p->clip = hal_audio_open(path);
    if (!p->clip) QN_LOGE(TAG, "no audio: %s", path);
    hal_audio_set_rate(p->clip, p->rate);
    p->active_word = -1;
}

void player_load(Player *p, int surah, int ayah)
{
    if (surah != p->surah || !p->timing_ok) {
        if (p->timing_ok) { timing_close(&p->timing); p->timing_ok = false; }
        p->surah = surah;
        p->timing_ok = timing_open(&p->timing, surah);
        // Navigable range: from the timing table if present, else just this ayah.
        p->ayah_min = 1;
        p->ayah_max = p->timing_ok ? p->timing.n_ayat : ayah;
    }
    if (ayah < p->ayah_min) ayah = p->ayah_min;
    if (ayah > p->ayah_max) ayah = p->ayah_max;
    p->ayah = ayah;
    open_clip(p);
}

void player_play(Player *p)
{
    if (!p->clip) return;
    hal_audio_play(p->clip);
    p->playing = true;
}

void player_pause(Player *p)
{
    if (p->clip) hal_audio_pause(p->clip);
    p->playing = false;
}

void player_toggle(Player *p)
{
    if (p->playing) player_pause(p);
    else player_play(p);
}

static void goto_ayah(Player *p, int ayah, bool keep_playing)
{
    if (ayah < p->ayah_min) ayah = p->ayah_min;
    if (ayah > p->ayah_max) ayah = p->ayah_max;
    if (ayah == p->ayah) return;
    p->ayah = ayah;
    open_clip(p);
    if (keep_playing) player_play(p);
}

void player_next_ayah(Player *p) { player_stop_loop(p); goto_ayah(p, p->ayah + 1, p->playing); }
void player_prev_ayah(Player *p) { player_stop_loop(p); goto_ayah(p, p->ayah - 1, p->playing); }

void player_set_rate(Player *p, float rate)
{
    if (rate < RATE_MIN) rate = RATE_MIN;
    if (rate > RATE_MAX) rate = RATE_MAX;
    p->rate = rate;
    hal_audio_set_rate(p->clip, rate);
}

void player_nudge_rate(Player *p, int dir)
{
    player_set_rate(p, p->rate + dir * RATE_STEP);
}

void player_start_loop(Player *p, const LoopConfig *cfg)
{
    p->loop = *cfg;
    p->loop.active = true;
    p->loop_each_i = 0;
    p->loop_section_i = 0;
    p->in_pause = false;
    player_set_rate(p, cfg->rate);
    player_load(p, p->surah, cfg->start_ayah);
    player_play(p);
    QN_LOGI(TAG, "loop %d:%d-%d each x%d section x%d pause %ums",
            p->surah, cfg->start_ayah, cfg->end_ayah,
            cfg->each_reps, cfg->section_reps, cfg->pause_ms);
}

void player_stop_loop(Player *p)
{
    p->loop.active = false;
    p->in_pause = false;
}

// Queue `ayah` to play next: (re)load its clip, then either pause first or play.
static void queue_ayah(Player *p, int ayah)
{
    p->ayah = ayah;
    open_clip(p);   // loads + rewinds; leaves it paused
    if (p->loop.pause_ms > 0) {
        p->in_pause = true;
        p->pause_until = plat_millis() + p->loop.pause_ms;
    } else {
        player_play(p);
    }
}

// One ayah of the loop just finished — decide what plays next.
static void loop_on_ayah_end(Player *p)
{
    p->loop_each_i++;
    if (p->loop_each_i < p->loop.each_reps) {
        queue_ayah(p, p->ayah);                 // repeat the same ayah
        return;
    }
    p->loop_each_i = 0;
    int next = p->ayah + 1;
    if (next > p->loop.end_ayah) {              // finished a pass over the range
        p->loop_section_i++;
        if (p->loop.section_reps == 0 || p->loop_section_i < p->loop.section_reps) {
            queue_ayah(p, p->loop.start_ayah);  // start the range again
        } else {
            p->loop.active = false;             // loop complete
            p->playing = false;
        }
    } else {
        queue_ayah(p, next);
    }
}

void player_update(Player *p)
{
    if (!p->clip) return;

    // Inter-clip pause: hold until the deadline, then start the queued clip.
    if (p->in_pause) {
        p->active_word = -1;
        if (plat_millis() >= p->pause_until) {
            p->in_pause = false;
            player_play(p);
        }
        return;
    }

    uint32_t pos = hal_audio_pos_ms(p->clip);
    if (p->timing_ok)
        p->active_word = timing_active_word(&p->timing, p->ayah, pos);

    // End-of-clip: we intended to play, but it stopped on its own.
    if (p->playing && !hal_audio_is_playing(p->clip)) {
        if (p->loop.active) {
            loop_on_ayah_end(p);
        } else if (p->autoplay_next && p->ayah < p->ayah_max) {
            goto_ayah(p, p->ayah + 1, true);
        } else {
            p->playing = false;   // reached the end of the range
        }
    }
}

uint32_t player_pos_ms(const Player *p) { return p->clip ? hal_audio_pos_ms(p->clip) : 0; }
uint32_t player_len_ms(const Player *p) { return p->clip ? hal_audio_len_ms(p->clip) : 0; }
