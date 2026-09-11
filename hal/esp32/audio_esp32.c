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

// Decode scratch (static, not on the audio task's stack).
static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int16_t s_stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

// Software volume/gain, Q8 fixed point (256 = 1.0x). Default ~1.8x because the
// recitation MP3s sit ~6 dB below full scale and line-out into a speaker is quiet.
// Above ~2x the loud peaks clip (harsh) — a real amp is the fix for more volume.
static int32_t s_vol_q8 = 460;
static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

// Output routing: the PAM8302 speaker amp (PIN_AMP_EN) is only enabled during
// playback when in Speaker mode; Headphone mode keeps it off (DAC line-out only).
static bool s_output_speaker = false;

static void spk(bool on) { gpio_set_level(PIN_AMP_EN, (s_output_speaker && on) ? 1 : 0); }

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
static int16_t      s_click[CLICK_MAX];
static volatile int s_click_len;   // 0 = nothing queued
static volatile int s_click_pos;
static TickType_t   s_amp_off_at;  // idle path holds PIN_AMP_EN on until this tick

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

        if (!(s_i2s_ok && play && c)) {
            // Not playing (paused / clip closed / between surahs): keep feeding the
            // I2S DMA with SILENCE. Otherwise the DMA loops the last buffer of PCM
            // and you hear a buzzy glitch on pause and when leaving the reader.
            // Any pending UI click tick is folded into that silence.
            if (s_i2s_ok) {
                int16_t out[256];              // 128 stereo frames (~2.9ms @44.1k)
                memset(out, 0, sizeof(out));
                if (s_click_len > 0) {
                    mix_click(out, 128);
                    s_amp_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(CLICK_LINGER_MS);
                }
                // Hold the amp on through the tick and its linger window so a run
                // of menu taps pops PIN_AMP_EN once, not once per click.
                bool amp = s_output_speaker && s_amp_off_at &&
                           xTaskGetTickCount() < s_amp_off_at;
                gpio_set_level(PIN_AMP_EN, amp ? 1 : 0);
                size_t wr;
                i2s_channel_write(s_tx, out, sizeof(out), &wr, pdMS_TO_TICKS(20));
            } else {
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            continue;
        }
        if (c->rd_pos >= c->mp3_len) {   // reached the end
            s_playing = false; spk(false);
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
            set_out_rate(fi.hz);   // match the I2S clock to this stream's rate
            const int32_t g = s_vol_q8;
            int16_t *buf;
            if (fi.channels == 1) {   // up-mix mono -> stereo, with gain
                for (int i = 0; i < samples; i++) {
                    int16_t s = sat16((s_pcm[i] * g) >> 8);
                    s_stereo[i*2] = s; s_stereo[i*2+1] = s;
                }
                buf = s_stereo;
            } else {                  // stereo, apply gain in place
                for (int i = 0; i < samples * 2; i++) s_pcm[i] = sat16((s_pcm[i] * g) >> 8);
                buf = s_pcm;
            }
            mix_click(buf, samples);   // fold in a UI tick if one is queued
            size_t bytes = (size_t)samples * 2 * sizeof(int16_t);
            size_t wr = 0;
            i2s_channel_write(s_tx, buf, bytes, &wr, portMAX_DELAY);
            // Temporary audio-path debug: prove decode+I2S are actually running.
            static uint32_t nfr = 0;
            if ((nfr++ % 40) == 0) {   // ~1s
                int16_t peak = 0;
                for (int i = 0; i < samples; i++) { int16_t a = s_pcm[i] < 0 ? -s_pcm[i] : s_pcm[i]; if (a > peak) peak = a; }
                ESP_LOGI(TAG, "audio: %uHz ch%d frame=%dB wrote=%u/%u peak=%d",
                         (unsigned)fi.hz, fi.channels, fi.frame_bytes, (unsigned)wr, (unsigned)bytes, (int)peak);
            }
        }
    }
}

void audio_esp32_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    mp3dec_init(&s_dec);
    gpio_set_direction(PIN_AMP_EN, GPIO_MODE_OUTPUT);
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
    // Big stack: mp3dec_decode_frame puts an ~18KB scratch struct on the stack.
    xTaskCreatePinnedToCore(audio_task, "audio", 32768, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "I2S ready (streaming MP3; BCK%d WS%d DOUT%d SPK%d)",
             PIN_I2S_BCK, PIN_I2S_WS, PIN_I2S_DATA, PIN_AMP_EN);
}

// Open is cheap now — just keep the bytes; decoding happens during playback.
HalAudioClip *hal_audio_open(const char *rel)
{
    uint8_t *buf; size_t len;
    if (!hal_fs_slurp(rel, &buf, &len)) { ESP_LOGE(TAG, "no audio: %s", rel); return NULL; }
    HalAudioClip *c = calloc(1, sizeof(*c));
    // hz starts at 0 so the decoder fills in the stream's REAL rate on the first
    // frame; pos_ms then divides played-samples by the true rate (a 22.05kHz file
    // left at 44100 here would report 2x position and desync the word highlight).
    c->mp3 = buf; c->mp3_len = len; c->rd_pos = 0; c->hz = 0; c->played = 0; c->len_ms = 0;
    return c;
}

void hal_audio_close(HalAudioClip *c)
{
    if (!c) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_cur == c) { s_cur = NULL; s_playing = false; }
    xSemaphoreGive(s_mtx);
    spk(false);
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

void hal_audio_pause(HalAudioClip *c) { (void)c; s_playing = false; spk(false); }
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
}

void hal_audio_set_rate(HalAudioClip *c, float rate) { (void)c; (void)rate; }  // time-stretch: TODO
void hal_audio_set_volume(float vol) {   // gain multiplier (1.0 = unity; >1 boosts, may clip)
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 4.0f) vol = 4.0f;
    s_vol_q8 = (int32_t)(vol * 256.0f + 0.5f);
}

void hal_audio_set_output(int speaker) {
    s_output_speaker = (speaker != 0);
    if (!s_output_speaker) gpio_set_level(PIN_AMP_EN, 0);   // mute amp in headphone mode
}

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
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)hz;
        float v = 0.16f * expf(-t * 420.0f) * sinf(6.2831853f * freq * t);
        s_click[i] = (int16_t)(v * 32767.0f);
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
        set_out_rate(OUT_RATE);   // this path upsamples to OUT_RATE stereo
        spk(true);
        size_t wr;
        i2s_channel_write(s_tx, buf, (size_t)of * 4, &wr, pdMS_TO_TICKS(40));
    }
    if (done && !clip_playing) spk(false);
    return of > 0;
}
