// audio_esp32.c — ESP32 audio HAL: streaming MP3 -> I2S (MAX98357A).
//
// Decoding a whole ayah up-front blocks the caller for seconds (the long ayat
// tripped the task watchdog), so instead the audio task decodes ONE MP3 frame at
// a time (minimp3) and writes it straight to I2S — real-time, no freeze, tiny
// memory. hal_audio_open just keeps the file bytes; the decoder runs during
// playback. Position is the count of decoded frames, so the reader's word
// highlight follows along. The bundled ayat are 44.1kHz stereo, so frames go to
// I2S unchanged (mono is up-mixed; other rates play at their native pitch).
#include "hal.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

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

static void audio_task(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        HalAudioClip *c = s_cur;
        bool play = s_playing;
        xSemaphoreGive(s_mtx);

        if (!(s_i2s_ok && play && c)) {
            // Not playing (paused / clip closed / between surahs): keep feeding the
            // I2S DMA with SILENCE. Otherwise the DMA loops the last buffer of PCM
            // and you hear a buzzy glitch on pause and when leaving the reader.
            if (s_i2s_ok) {
                static const int16_t silence[256] = {0};   // ~1.5ms stereo @44.1k
                size_t wr;
                i2s_channel_write(s_tx, silence, sizeof(silence), &wr, pdMS_TO_TICKS(20));
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
    c->mp3 = buf; c->mp3_len = len; c->rd_pos = 0; c->hz = OUT_RATE; c->played = 0; c->len_ms = 0;
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

uint32_t hal_audio_pos_ms(HalAudioClip *c)
{
    if (!c || c->hz == 0) return 0;
    return (uint32_t)((uint64_t)c->played * 1000 / c->hz);
}
uint32_t hal_audio_len_ms(HalAudioClip *c) { return c ? c->len_ms : 0; }

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

// UI click: no-op on device for now. TODO: mix a short tick into the I2S
// stream (needs a tiny mixer stage in the render task; avoid amp pops).
void hal_audio_click(bool accent) { (void)accent; }

// --- INMP441 I2S microphone (I2S_NUM_1) — Quran Teacher recording ----------
static i2s_chan_handle_t s_rx;
static bool s_mic_ok;

bool hal_mic_start(uint32_t hz)
{
    if (s_mic_ok) return true;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &s_rx) != ESP_OK) { s_rx = NULL; return false; }
    // INMP441 is 24-bit data left-justified in a 32-bit slot; L/R->GND = left.
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_MIC_SCK, .ws = PIN_MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = PIN_MIC_SD },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK ||
        i2s_channel_enable(s_rx) != ESP_OK) {
        i2s_del_channel(s_rx); s_rx = NULL; return false;
    }
    s_mic_ok = true;
    ESP_LOGI(TAG, "INMP441 mic started %uHz (SCK%d WS%d SD%d)",
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
    for (int i = 0; i < n; i++)
        buf[i] = (int16_t)(raw[i] >> 13);   // 24-bit MSB-aligned -> int16 + gain (tune >>11..16)
    return n;
}

void hal_mic_stop(void)
{
    if (!s_mic_ok) return;
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = NULL; s_mic_ok = false;
}

// --- Reference recitation PCM readback (still TODO on device) --------------
// The teacher's alignment/compare needs the reference ayah decoded to PCM at a
// given offset. Not yet implemented (streaming decoder is playback-oriented).
uint32_t hal_audio_read_pcm16(HalAudioClip *clip, uint32_t start_ms,
                              int16_t *out, uint32_t max_samples, uint32_t *out_hz)
{ (void)clip; (void)start_ms; (void)out; (void)max_samples; if (out_hz) *out_hz = 0; return 0; }
void hal_pcm_play(const int16_t *pcm, uint32_t n, uint32_t hz) { (void)pcm; (void)n; (void)hz; }
void hal_pcm_stop(void) {}
bool hal_pcm_is_playing(void) { return false; }
