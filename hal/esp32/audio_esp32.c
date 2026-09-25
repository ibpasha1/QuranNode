// audio_esp32.c — ESP32 audio HAL: streaming MP3 -> I2S (MAX98357A).
//
// Decoding a whole ayah up-front blocks the caller for seconds (the long ayat
// tripped the task watchdog), so instead the audio task decodes ONE MP3 frame at
// a time (minimp3) and writes it straight to I2S — real-time, no freeze, tiny
// memory. hal_audio_open just keeps the file bytes; the decoder runs during
// playback. Position is the count of decoded frames, so the reader's word
// highlight follows along. Most ayat are 44.1kHz stereo, but a few surahs ship
// at 22.05kHz (e.g. Al-Baqarah from quranicaudio.com), so the I2S clock is
// retuned to each stream's real rate on the fly — otherwise a 22.05kHz file
// clocked out at 44.1kHz plays at 2x speed. Mono is up-mixed to stereo.
#include "hal.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include "wsola.h"   // pitch-preserving time-stretch for recitation speed

static const char *TAG = "AUDIO";
#define OUT_RATE 44100

struct HalAudioClip {
    uint8_t *mp3;      // owned file bytes
    size_t   mp3_len;
    size_t   rd_pos;   // decode cursor (bytes)
    int      hz;       // discovered from the stream
    uint32_t played;   // source frames emitted (drives pos_ms)
    uint32_t len_ms;   // estimated from bitrate on the first frame
};

static i2s_chan_handle_t s_tx;
static SemaphoreHandle_t s_mtx;
static mp3dec_t          s_dec;
static HalAudioClip     *s_cur;
static volatile bool     s_playing;
static bool              s_i2s_ok;
// Is the I2S channel currently enabled? At true idle we DISABLE it (stops the
// BCLK/WS/DATA clock) and drop the amp so the device is electrically silent —
// otherwise a running I2S clock + class-D amp idling on silence emits an audible
// switching "drone/click". Toggled only from the audio task. See the idle branch
// in audio_task and i2s_resume().
static bool              s_i2s_running;

// Decode scratch (static, not on the audio task's stack).
static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

// Recitation-speed time-stretch. Playback rate lives in s_rate (1.0 = normal,
// which BYPASSES WSOLA entirely so default playback is bit-for-bit unchanged);
// only a non-1.0 rate routes decoded PCM through the pitch-preserving stretcher.
static WsolaState    s_ws;
static volatile float s_rate = 1.0f;
static HalAudioClip *s_ws_clip;            // last clip the stretcher saw
static volatile bool s_ws_reset_req;       // seek asks the audio task to flush
#define WS_PULL 1024   // <= MINIMP3_MAX_SAMPLES_PER_FRAME, so s_stereo fits a batch
static int16_t s_ws_out[WS_PULL * WS_MAXCH];
static inline bool rate_is_bypass(void) { return s_rate > 0.999f && s_rate < 1.001f; }

// Software volume/gain, Q8 fixed point (256 = 1.0x). Default ~1.8x because the
// recitation MP3s sit ~6 dB below full scale and line-out into a speaker is quiet.
// Above ~2x the loud peaks clip (harsh) — a real amp is the fix for more volume.
static int32_t s_vol_q8 = 460;
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

// Output routing. Mode: 0 = headphone (amp always off), 1 = speaker (amp on when
// playing), 2 = auto (follow the jack-detect pin). In auto, inserting a plug (GPIO46
// reads HIGH) routes to the line-out jack and mutes the amp.
static int s_output_mode = 2;   // auto

static inline bool hp_present(void) { return gpio_get_level(PIN_HP_DETECT) != 0; }
static inline bool use_speaker(void) {
    if (s_output_mode == 1) return true;             // forced speaker
    if (s_output_mode == 0) return false;            // forced headphone
    return !hp_present();                            // auto: speaker unless a plug is in
}

static void spk(bool on) { gpio_set_level(PIN_AMP_EN, (use_speaker() && on) ? 1 : 0); }

// The I2S rate currently programmed into the hardware. Retune it to a stream's
// real sample rate (disable -> reconfig clock -> enable), but only when it
// actually changes — a no-op call must not glitch steady 44.1kHz playback.
// Called only from the audio task, so s_out_hz needs no lock.
static int s_out_hz = OUT_RATE;

static void set_out_rate(int hz)
{
    if (hz <= 0 || hz == s_out_hz || !s_i2s_ok) return;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hz);
    if (i2s_channel_disable(s_tx) != ESP_OK) return;
    esp_err_t e = i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);   // re-enable even on failure (stays at old rate)
    if (e == ESP_OK) { s_out_hz = hz; ESP_LOGI(TAG, "I2S retuned to %dHz", hz); }
    else             { ESP_LOGE(TAG, "I2S reconfig %dHz failed (%d)", hz, (int)e); }
}

// Re-enable the I2S clock if the idle path parked it. Audio-task only, called
// before any path that writes to I2S (playback, user PCM, UI click).
static void i2s_resume(void)
{
    if (s_i2s_ok && !s_i2s_running) {
        if (i2s_channel_enable(s_tx) == ESP_OK) s_i2s_running = true;
    }
}

bool audio_esp32_pcm_pump(void);   // user-recording playback (defined below)

// --- UI click tick ---------------------------------------------------------
// A short decaying sine mixed straight into the I2S output. The previous no-op
// left menus silent on the device; the sim has always ticked. Two things made
// this non-trivial and are handled here: (1) the speaker amp is normally off
// outside playback, so a click has to switch it on, and (2) toggling the amp
// for every tap would pop it — so the idle path holds the amp on for a short
// "linger" after each tick, meaning a burst of menu taps pops the amp once.
#define CLICK_MAX        1024    // samples; ~23ms @44.1k, caps a ~12ms tick
#define CLICK_LINGER_MS  1200    // keep the amp on this long after the last tick
// Amp/I2S hold across playback stops. When a clip ends under autoplay, the next
// ayah starts within ~a UI frame; cutting the amp and parking the I2S clock in
// that window pops the PAM8302 and hardens the gap at EVERY ayah boundary. So
// end-of-clip / pause / close don't drop the amp — they arm this linger and the
// idle branch keeps feeding silence with the amp on. If nothing follows (user
// really stopped), the linger lapses and the idle path parks as before. Long
// enough to also ride out short Loop-mode inter-ayah pauses.
#define TRANS_LINGER_MS  1500
// Silent buffers to flush before parking the I2S clock at idle. The default DMA
// ring is dma_desc_num(6) * dma_frame_num(240) = 1440 frames; we write 128
// frames/buffer, so 12 buffers (1536 frames) guarantees the last recitation PCM
// is fully clocked out and replaced by zeros before we disable — no stop-click.
#define IDLE_DRAIN_WRITES 12
static int16_t      s_click[CLICK_MAX];
static volatile int s_click_len;   // 0 = nothing queued
static volatile int s_click_pos;
// Idle path holds PIN_AMP_EN on until this tick. Written by the audio task
// (clicks, end-of-clip) and the UI task (pause/close); aligned 32-bit stores
// are atomic on Xtensa, and a torn read would only mistime the linger.
static volatile TickType_t s_amp_off_at;

// Add the queued tick into a stereo buffer (both channels), advancing the
// cursor. Runs on the audio task; the UI task fills s_click under s_mtx.
static void mix_click(int16_t *stereo, int frames)
{
    if (s_click_len <= 0) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int p = s_click_pos, len = s_click_len;
    for (int i = 0; i < frames && p < len; i++, p++) {
        int32_t s = s_click[p];
        stereo[i * 2]     = sat16((int32_t)stereo[i * 2]     + s);
        stereo[i * 2 + 1] = sat16((int32_t)stereo[i * 2 + 1] + s);
    }
    s_click_pos = p;
    if (p >= len) { s_click_len = 0; s_click_pos = 0; }
    xSemaphoreGive(s_mtx);
}

// Apply gain, up-mix mono->stereo, fold in any queued UI tick, and clock `n`
// frames of `ch`-channel PCM out to the I2S DMA. Shared by the normal
// (bypass) path and the time-stretched path. `pcm` for stereo is modified
// in place; mono is written into s_stereo.
static void emit_frames(int16_t *pcm, int n, int ch)
{
    if (n <= 0) return;
    const int32_t g = s_vol_q8;
    int16_t *buf;
    if (ch == 1) {                 // up-mix mono -> stereo, with gain
        for (int i = 0; i < n; i++) {
            int16_t s = sat16((pcm[i] * g) >> 8);
            s_stereo[i * 2] = s; s_stereo[i * 2 + 1] = s;
        }
        buf = s_stereo;
    } else {                       // stereo: gain in place
        for (int i = 0; i < n * 2; i++) pcm[i] = sat16((pcm[i] * g) >> 8);
        buf = pcm;
    }
    mix_click(buf, n);             // fold in a UI tick if one is queued
    size_t wr = 0;
    i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t), &wr, portMAX_DELAY);
}

static void audio_task(void *arg)
{
    (void)arg;
    while (1) {
        // The user's own recording (teacher "hear yourself") takes priority;
        // the scene pauses the clip first, so they never overlap.
        if (audio_esp32_pcm_pump()) continue;

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        HalAudioClip *c = s_cur;
        bool play = s_playing;
        xSemaphoreGive(s_mtx);

        // Flush the stretcher's overlap buffers on a new clip or a seek, so an
        // ayah never starts with tail grains from the previous one. Done here
        // (audio task) since the stretcher is otherwise only touched here.
        if (c != s_ws_clip || s_ws_reset_req) {
            wsola_reset(&s_ws);
            s_ws_clip = c;
            s_ws_reset_req = false;
        }

        if (!(s_i2s_ok && play && c)) {
            if (!s_i2s_ok) { vTaskDelay(pdMS_TO_TICKS(8)); continue; }

            bool click = s_click_len > 0;
            TickType_t now = xTaskGetTickCount();
            bool linger = s_amp_off_at && now < s_amp_off_at;

            if (click || linger) {
                // Active idle: a UI tick is queued, or we're inside the post-click
                // linger window. Feed the I2S DMA with SILENCE (+ any tick) so it
                // never loops stale PCM (the buzz on pause / leaving the reader),
                // and hold the amp on across the linger so a burst of menu taps
                // pops PIN_AMP_EN once, not once per tap.
                i2s_resume();
                int16_t out[256];              // 128 stereo frames (~2.9ms @44.1k)
                memset(out, 0, sizeof(out));
                if (click) {
                    mix_click(out, 128);
                    s_amp_off_at = now + pdMS_TO_TICKS(CLICK_LINGER_MS);
                }
                gpio_set_level(PIN_AMP_EN, use_speaker() ? 1 : 0);
                size_t wr;
                i2s_channel_write(s_tx, out, sizeof(out), &wr, pdMS_TO_TICKS(20));
            } else if (s_i2s_running) {
                // Nothing to play and the linger window has closed. Flush the ring
                // with silence (so the last thing clocked out is zeros — no click
                // on stop), then CUT the amp and STOP the I2S clock. This is what
                // kills the idle drone: a parked device makes no sound and draws no
                // class-D / DMA switching noise. It wakes again via i2s_resume().
                int16_t out[256]; memset(out, 0, sizeof(out)); size_t wr;
                for (int i = 0; i < IDLE_DRAIN_WRITES; i++)
                    i2s_channel_write(s_tx, out, sizeof(out), &wr, pdMS_TO_TICKS(20));
                gpio_set_level(PIN_AMP_EN, 0);
                i2s_channel_disable(s_tx);
                s_i2s_running = false;
            } else {
                // Parked: I2S clock stopped, amp off. Sleep cheaply; the next UI
                // click or playback re-enables the clock via i2s_resume().
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            continue;
        }
        if (c->rd_pos >= c->mp3_len) {   // reached the end
            s_playing = false;
            // Don't cut the amp: autoplay's next ayah starts within ~a UI
            // frame, and the linger keeps the amp powered + I2S clocking
            // silence across the handoff — no pop, no clock restart. A real
            // stop just lets the linger lapse into the normal idle park.
            s_amp_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(TRANS_LINGER_MS);
            continue;
        }

        mp3dec_frame_info_t fi;
        int samples = mp3dec_decode_frame(&s_dec, c->mp3 + c->rd_pos,
                                          (int)(c->mp3_len - c->rd_pos), s_pcm, &fi);
        if (fi.frame_bytes == 0) { c->rd_pos = c->mp3_len; continue; }  // no more frames

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_cur == c) {
            c->rd_pos += fi.frame_bytes;
            if (samples > 0) {
                if (c->hz == 0) c->hz = fi.hz;
                if (c->len_ms == 0 && fi.bitrate_kbps > 0)
                    c->len_ms = (uint32_t)((uint64_t)c->mp3_len * 8 / fi.bitrate_kbps);
                c->played += samples;
            }
        }
        xSemaphoreGive(s_mtx);

        if (samples > 0) {
            i2s_resume();          // wake the clock if the idle path parked it
            set_out_rate(fi.hz);   // I2S clock stays at the stream's real rate —
                                   // WSOLA changes DURATION, not pitch, so this
                                   // is correct at any speed.
            if (rate_is_bypass()) {
                emit_frames(s_pcm, samples, fi.channels);   // normal speed
            } else {
                // Route the decoded frame through the pitch-preserving stretcher.
                if (s_ws.ch != fi.channels) wsola_init(&s_ws, fi.channels);
                wsola_set_rate(&s_ws, s_rate);
                int off = 0;
                while (off < samples) {
                    int acc = wsola_feed(&s_ws, s_pcm + (size_t)off * fi.channels,
                                         samples - off);
                    off += acc;
                    int got;
                    while ((got = wsola_pull(&s_ws, s_ws_out, WS_PULL)) > 0)
                        emit_frames(s_ws_out, got, fi.channels);
                    if (acc == 0) break;   // FIFO full but drained nothing: bail
                }
            }
        }
    }
}

// --- Async clip prefetch ---------------------------------------------------
// The next ayah's whole MP3 is slurped from SD on a low-priority worker while
// the current ayah is still playing, so hal_audio_open() for that path returns
// instantly instead of stalling on a multi-MB SD read at the transition — that
// stall is the audible gap between ayat, and the SD burst also starved the
// reader's glyph reads ("this ayah isn't on the SD card"). One slot; only the
// most recent request is honoured, and a miss just falls back to a sync open.
static SemaphoreHandle_t s_pf_mtx;
static char     s_pf_req[96];    // requested path, "" = nothing pending
static uint8_t *s_pf_buf;        // published ready bytes, NULL = none
static size_t   s_pf_len;
static char     s_pf_path[96];   // the path s_pf_buf holds

// Opener side: take the ready buffer if it matches `rel`. If it holds some OTHER
// path we navigated away from, drop it so a stale 2MB slurp can't linger.
static bool pf_take(const char *rel, uint8_t **buf, size_t *len)
{
    if (!s_pf_mtx) return false;   // opened before audio init: no prefetch yet
    bool hit = false;
    xSemaphoreTake(s_pf_mtx, portMAX_DELAY);
    if (s_pf_buf && strcmp(s_pf_path, rel) == 0) {
        *buf = s_pf_buf; *len = s_pf_len;
        s_pf_buf = NULL; s_pf_path[0] = 0;
        hit = true;
    } else if (s_pf_buf) {
        free(s_pf_buf); s_pf_buf = NULL; s_pf_path[0] = 0;
    }
    xSemaphoreGive(s_pf_mtx);
    return hit;
}

void hal_audio_prefetch(const char *rel)
{
    if (!rel || !s_pf_mtx) return;
    xSemaphoreTake(s_pf_mtx, portMAX_DELAY);
    // Already have it, or already loading it? nothing to do.
    if ((s_pf_buf && strcmp(s_pf_path, rel) == 0) ||
        (s_pf_req[0] && strcmp(s_pf_req, rel) == 0)) {
        xSemaphoreGive(s_pf_mtx);
        return;
    }
    // New target: drop any ready buffer for a different path before we start.
    if (s_pf_buf) { free(s_pf_buf); s_pf_buf = NULL; s_pf_path[0] = 0; }
    strncpy(s_pf_req, rel, sizeof(s_pf_req) - 1);
    s_pf_req[sizeof(s_pf_req) - 1] = 0;
    xSemaphoreGive(s_pf_mtx);
}

static void prefetch_task(void *arg)
{
    (void)arg;
    while (1) {
        char req[96];
        req[0] = 0;
        xSemaphoreTake(s_pf_mtx, portMAX_DELAY);
        if (s_pf_req[0]) { strcpy(req, s_pf_req); s_pf_req[0] = 0; }
        xSemaphoreGive(s_pf_mtx);

        if (!req[0]) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        // Slurp outside the lock — this is the multi-MB SD read we're hiding.
        uint8_t *buf = NULL; size_t len = 0;
        if (!hal_fs_slurp(req, &buf, &len)) continue;   // miss: opener falls back

        xSemaphoreTake(s_pf_mtx, portMAX_DELAY);
        // Publish only if the player still wants this exact clip — a newer
        // request that arrived mid-read supersedes us.
        if (!s_pf_req[0] || strcmp(s_pf_req, req) == 0) {
            if (s_pf_buf) free(s_pf_buf);   // replace any older ready buffer
            s_pf_buf = buf; s_pf_len = len; strcpy(s_pf_path, req);
            s_pf_req[0] = 0;
            buf = NULL;
        }
        xSemaphoreGive(s_pf_mtx);
        if (buf) free(buf);   // superseded — discard
    }
}

void audio_esp32_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_pf_mtx = xSemaphoreCreateMutex();
    mp3dec_init(&s_dec);
    gpio_set_direction(PIN_AMP_EN, GPIO_MODE_OUTPUT);
    // Headphone-detect input. GPIO46 defaults to the UART/strap function; reset it to a
    // plain input and disable internal pulls (the external 100k/20k network sets levels).
    gpio_reset_pin(PIN_HP_DETECT);
    gpio_set_direction(PIN_HP_DETECT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_HP_DETECT, GPIO_FLOATING);
    spk(false);

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel failed"); return; }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCK, .ws = PIN_I2S_WS,
                      .dout = PIN_I2S_DATA, .din = I2S_GPIO_UNUSED },
    };
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) { ESP_LOGE(TAG, "i2s init failed"); return; }
    i2s_channel_enable(s_tx);
    s_i2s_ok = true;
    s_i2s_running = true;
    // Big stack: mp3dec_decode_frame puts an ~18KB scratch struct on the stack.
    xTaskCreatePinnedToCore(audio_task, "audio", 32768, NULL, 5, NULL, 1);
    // Low-priority SD prefetcher on the app core; it only ever blocks on reads.
    xTaskCreatePinnedToCore(prefetch_task, "audio_pf", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "I2S ready (streaming MP3; BCK%d WS%d DOUT%d SPK%d)",
             PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DATA, PIN_AMP_EN);
}

// Wrap already-slurped MP3 bytes in a clip. hz starts at 0 so the decoder fills
// in the stream's REAL rate on the first frame; pos_ms then divides played-
// samples by the true rate (a 22.05kHz file left at 44100 here would report 2x
// position and desync the word highlight). Takes ownership of `buf`.
static HalAudioClip *clip_from_bytes(uint8_t *buf, size_t len)
{
    HalAudioClip *c = calloc(1, sizeof(*c));
    if (!c) { free(buf); return NULL; }
    c->mp3 = buf; c->mp3_len = len; c->rd_pos = 0; c->hz = 0; c->played = 0; c->len_ms = 0;
    return c;
}

// Open is cheap now — just keep the bytes; decoding happens during playback. If
// the prefetcher already slurped this path, take those bytes and skip the read.
HalAudioClip *hal_audio_open(const char *rel)
{
    uint8_t *buf = NULL; size_t len = 0;
    if (pf_take(rel, &buf, &len)) return clip_from_bytes(buf, len);
    if (!hal_fs_slurp(rel, &buf, &len)) { ESP_LOGE(TAG, "no audio: %s", rel); return NULL; }
    return clip_from_bytes(buf, len);
}

void hal_audio_close(HalAudioClip *c)
{
    if (!c) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_cur == c) { s_cur = NULL; s_playing = false; }
    xSemaphoreGive(s_mtx);
    // Ayah transitions close the old clip microseconds before playing the next
    // one — an amp cut here would blip PIN_AMP_EN low at every boundary. Let
    // the linger carry the amp across; a real close parks via the idle path.
    s_amp_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(TRANS_LINGER_MS);
    if (c->mp3) free(c->mp3);
    free(c);
}

void hal_audio_play(HalAudioClip *c)
{
    if (!c) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_cur != c || c->rd_pos >= c->mp3_len) {   // (re)start this clip
        s_cur = c; c->rd_pos = 0; c->played = 0;
        mp3dec_init(&s_dec);
    }
    s_playing = true;
    xSemaphoreGive(s_mtx);
    spk(true);
    ESP_LOGI(TAG, "play: clip=%uB i2s_ok=%d amp_en(GPIO%d)=1", (unsigned)c->mp3_len, s_i2s_ok, PIN_AMP_EN);
}

void hal_audio_pause(HalAudioClip *c)
{
    (void)c;
    s_playing = false;
    // Linger instead of an instant amp cut: pause stops the sound (idle branch
    // feeds silence) without popping the amp, and a quick resume is seamless.
    s_amp_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(TRANS_LINGER_MS);
}
bool hal_audio_is_playing(HalAudioClip *c) { (void)c; return s_playing; }
bool hal_audio_active(void) { return s_playing; }

uint32_t hal_audio_pos_ms(HalAudioClip *c)
{
    if (!c || c->hz == 0) return 0;
    return (uint32_t)((uint64_t)c->played * 1000 / c->hz);
}
uint32_t hal_audio_len_ms(HalAudioClip *c) { return c ? c->len_ms : 0; }

uint32_t hal_audio_latency_ms(HalAudioClip *c)
{
    // `played` counts frames the moment they're decoded and written into the
    // I2S DMA ring; i2s_channel_write blocks (portMAX_DELAY) once the ring is
    // full, so the decode cursor sits ~one DMA depth ahead of what's leaving
    // the pin. Default channel config: dma_desc_num(6) * dma_frame_num(240).
    // pos_ms therefore leads the audible playhead by that depth.
    if (!c || c->hz == 0) return 0;
    return (uint32_t)((uint64_t)(6 * 240) * 1000 / c->hz);
}

void hal_audio_seek_ms(HalAudioClip *c, uint32_t ms)
{
    if (!c) return;
    // Streaming decoder: only a restart is exact; treat any seek as "from start".
    (void)ms;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    c->rd_pos = 0; c->played = 0;
    if (s_cur == c) mp3dec_init(&s_dec);
    xSemaphoreGive(s_mtx);
    s_ws_reset_req = true;   // audio task flushes the stretcher's stale grains
}

void hal_audio_set_rate(HalAudioClip *c, float rate)
{
    (void)c;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    s_rate = rate;              // audio task reads this; 1.0 bypasses WSOLA
    wsola_set_rate(&s_ws, rate);
}
void hal_audio_set_volume(float vol) {   // gain multiplier (1.0 = unity; >1 boosts, may clip)
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 4.0f) vol = 4.0f;
    s_vol_q8 = (int32_t)(vol * 256.0f + 0.5f);
}

void hal_audio_set_output(int mode) {
    s_output_mode = mode;                         // 0 headphone, 1 speaker, 2 auto
    if (!use_speaker()) gpio_set_level(PIN_AMP_EN, 0);   // mute amp now if not on speaker
}

int hal_audio_headphone_present(void) { return hp_present() ? 1 : 0; }

// UI click: render a short decaying-sine tick at the current output rate and
// queue it; the audio task mixes it into the I2S stream (see mix_click) and
// manages the amp. Accent clicks ring brighter, matching the sim.
void hal_audio_click(bool accent)
{
    if (!s_i2s_ok) return;
    int hz = s_out_hz > 0 ? s_out_hz : OUT_RATE;
    int n = hz * 12 / 1000;                 // ~12ms
    if (n > CLICK_MAX) n = CLICK_MAX;
    const float freq = accent ? 1760.0f : 1175.0f;

    // Stop the audio task consuming the buffer while we refill it, render, then
    // publish the length last so it never reads a half-written tick.
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_click_len = 0;
    xSemaphoreGive(s_mtx);
    // Scale the tick by the master volume so lowering the volume lowers the UI
    // clicks too (and 0 mutes them). s_vol_q8: 256 = 1.0x. Cap at 1.0 so the tick
    // never exceeds its tuned 0.16 baseline — we only ever scale it DOWN.
    float g = s_vol_q8 / 256.0f;
    if (g > 1.0f) g = 1.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)hz;
        float v = 0.16f * g * expf(-t * 420.0f) * sinf(6.2831853f * freq * t);
        s_click[i] = sat16((int32_t)(v * 32767.0f));
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_click_pos = 0;
    s_click_len = n;
    xSemaphoreGive(s_mtx);
}

// --- INMP441 I2S microphone (I2S_NUM_1) — Quran Teacher recording ----------
static i2s_chan_handle_t s_rx;
static bool s_mic_ok;

// 90Hz one-pole high-pass on the mic stream: spectral analysis of field
// takes showed 0-100Hz rumble AS LOUD AS THE VOICE (mic self-noise +
// vibration) — it skews the endpointer, the local features, and the ASR.
// alpha = fs/(fs + 2*pi*fc) at 16k/90Hz ~= 0.9659 -> Q15 31652.
static int32_t s_hp_x, s_hp_y;
static uint32_t s_mic_clip;   // saturated samples this session (logged on stop)

bool hal_mic_start(uint32_t hz)
{
    if (s_mic_ok) return true;   // already capturing

    // Create the I2S RX channel ONCE and keep it resident for the life of the
    // process — only enable/disable it per take. The read-along starts+stops
    // the mic many times a session, and repeatedly new/del-ing an I2S channel
    // intermittently failed ("No microphone", solid wiring): the peripheral/DMA
    // isn't guaranteed released before the next i2s_new_channel, and its DMA
    // descriptors want internal RAM that can be momentarily fragmented. Creating
    // once shrinks that failure window to a single per-boot attempt.
    if (!s_rx) {
        i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        if (i2s_new_channel(&cc, NULL, &s_rx) != ESP_OK) {
            s_rx = NULL;
            ESP_LOGE(TAG, "mic: i2s_new_channel(I2S_NUM_1) failed — DMA-cap free=%u internal=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            return false;
        }
        // INMP441 is 24-bit data left-justified in a 32-bit slot; L/R->GND = left.
        i2s_std_config_t sc = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(hz),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_MIC_SCK, .ws = PIN_MIC_WS,
                          .dout = I2S_GPIO_UNUSED, .din = PIN_MIC_SD },
        };
        sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
        if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) {
            i2s_del_channel(s_rx); s_rx = NULL;
            ESP_LOGE(TAG, "mic: i2s init failed — check INMP441 wiring: "
                     "SCK=GPIO%d WS=GPIO%d SD=GPIO%d, L/R->GND, VDD=3V3",
                     PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);
            return false;
        }
    }

    if (i2s_channel_enable(s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "mic: i2s_channel_enable failed");
        return false;
    }
    s_mic_ok = true;
    s_hp_x = s_hp_y = 0;   // reset the high-pass state per session
    ESP_LOGI(TAG, "INMP441 mic on %uHz (SCK%d WS%d SD%d)",
             (unsigned)hz, PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);
    return true;
}

int hal_mic_read(int16_t *buf, int max_samples)
{
    if (!s_mic_ok) return -1;
    static int32_t raw[512];
    int want = max_samples < 512 ? max_samples : 512;
    size_t got = 0;
    if (i2s_channel_read(s_rx, raw, (size_t)want * sizeof(int32_t), &got, 0) != ESP_OK)
        return 0;   // nothing ready yet
    int n = (int)(got / sizeof(int32_t));
    for (int i = 0; i < n; i++) {
        // >>14 = +12dB over the natural 24->16 conversion. The previous >>13
        // (+18dB) pushed close-mic speech past full scale, and the old cast
        // WRAPPED instead of clipping — audible distortion on every loud
        // syllable (user-confirmed by ear on the training WAVs).
        int32_t x = raw[i] >> 14;
        int32_t y = (int32_t)(((int64_t)31652 * (s_hp_y + x - s_hp_x)) >> 15);
        s_hp_x = x;
        s_hp_y = y;
        if (y > 32767 || y < -32768) s_mic_clip++;
        buf[i] = sat16(y);
    }
    return n;
}

void hal_mic_stop(void)
{
    if (!s_mic_ok) return;
    if (s_mic_clip)
        ESP_LOGW(TAG, "mic: %u samples clipped this take (reduce gain?)",
                 (unsigned)s_mic_clip);
    s_mic_clip = 0;
    i2s_channel_disable(s_rx);   // keep the channel resident (see hal_mic_start);
    s_mic_ok = false;            // only disable — never del, so re-start can't race
}

bool hal_mic_active(void) { return s_mic_ok; }

// --- Reference recitation PCM readback (Quran Teacher analysis) ------------
// Full decode of the clip's MP3 to mono s16 with a LOCAL decoder instance —
// playback's s_dec/s_pcm stay untouched, and the clip's mp3 bytes are only
// read, so this is safe even while the clip is playing. Runs on the caller's
// task: qn_main has a 40KB stack precisely for minimp3's ~18KB decode
// scratch. An ayah decodes in well under a second (shown as "ANALYZING...").
uint32_t hal_audio_read_pcm16(HalAudioClip *clip, uint32_t start_ms,
                              int16_t *out, uint32_t max_samples, uint32_t *out_hz)
{
    if (out_hz) *out_hz = 0;
    if (!clip || !clip->mp3 || !max_samples) return 0;

    mp3dec_t *dec = malloc(sizeof(mp3dec_t));
    if (!dec) return 0;
    mp3dec_init(dec);

    int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t pos = 0;
    uint32_t emitted = 0;   // mono samples written to out
    uint64_t seen = 0;      // mono samples decoded so far (for start_ms skip)
    uint64_t skip = 0;
    int hz = 0;

    while (pos < clip->mp3_len && emitted < max_samples) {
        mp3dec_frame_info_t fi;
        int samples = mp3dec_decode_frame(dec, clip->mp3 + pos,
                                          (int)(clip->mp3_len - pos), frame, &fi);
        if (fi.frame_bytes <= 0) break;
        pos += (size_t)fi.frame_bytes;
        if (samples <= 0) continue;
        if (!hz) {
            hz = fi.hz;
            skip = (uint64_t)start_ms * (uint64_t)hz / 1000ull;
        }
        for (int i = 0; i < samples && emitted < max_samples; i++, seen++) {
            if (seen < skip) continue;
            if (fi.channels == 2)
                out[emitted++] = (int16_t)(((int32_t)frame[i * 2] +
                                            frame[i * 2 + 1]) / 2);
            else
                out[emitted++] = frame[i];
        }
    }
    free(dec);
    if (out_hz) *out_hz = (uint32_t)hz;
    return emitted;
}

// --- Raw PCM playback (the user's own recording, "hear yourself") ----------
// A second source for the audio task: mono s16 at s_up_hz, nearest-neighbor
// upsampled to OUT_RATE stereo. The teacher pauses the clip first, so the two
// sources are never active together; the task checks this one first.
static const int16_t *s_up_pcm;
static uint32_t s_up_n, s_up_pos, s_up_hz, s_up_phase;

void hal_pcm_play(const int16_t *pcm, uint32_t n, uint32_t hz)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_up_pcm = pcm;
    s_up_n = n;
    s_up_pos = 0;
    s_up_phase = 0;
    s_up_hz = hz ? hz : 16000;
    xSemaphoreGive(s_mtx);
}

void hal_pcm_stop(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_up_pcm = NULL;
    xSemaphoreGive(s_mtx);
}

bool hal_pcm_is_playing(void) { return s_up_pcm != NULL; }

// Called by the audio task each loop: fills + writes one chunk of the user
// PCM. Returns true if it produced output (skip the mp3 path this iteration).
bool audio_esp32_pcm_pump(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const int16_t *src = s_up_pcm;
    if (!src || !s_i2s_ok) { xSemaphoreGive(s_mtx); return false; }

    int16_t buf[512];   // 256 stereo frames (~5.8ms @44.1k)
    int of = 0;
    while (of < 256 && s_up_pos < s_up_n) {
        int16_t s = sat16(((int32_t)src[s_up_pos] * s_vol_q8) >> 8);
        buf[of * 2] = s;
        buf[of * 2 + 1] = s;
        of++;
        s_up_phase += s_up_hz;
        while (s_up_phase >= OUT_RATE && s_up_pos < s_up_n) {
            s_up_phase -= OUT_RATE;
            s_up_pos++;
        }
    }
    bool done = (s_up_pos >= s_up_n);
    if (done) s_up_pcm = NULL;
    bool clip_playing = s_playing;
    xSemaphoreGive(s_mtx);

    if (of > 0) {
        i2s_resume();             // wake the clock if the idle path parked it
        set_out_rate(OUT_RATE);   // this path upsamples to OUT_RATE stereo
        spk(true);
        size_t wr;
        i2s_channel_write(s_tx, buf, (size_t)of * 4, &wr, pdMS_TO_TICKS(40));
    }
    if (done && !clip_playing)   // linger, don't pop (see TRANS_LINGER_MS)
        s_amp_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(TRANS_LINGER_MS);
    return of > 0;
}
