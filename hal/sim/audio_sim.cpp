// audio_sim.cpp — desktop audio implementation of the HAL audio API.
//
// Decodes a per-ayah MP3 (dr_mp3) into PCM, plays it through an SDL audio device,
// and time-stretches with SoundTouch so playback speed (e.g. 0.85x) changes tempo
// WITHOUT shifting pitch — essential for recitation. Position is reported on the
// ORIGINAL recitation timeline (independent of tempo) so word-highlight timings
// line up regardless of speed.
//
// One clip plays at a time (the reader/library play a single ayah/track), which
// keeps the mixing trivial: the SDL callback pulls source frames through one
// SoundTouch instance.
#include <SDL2/SDL.h>
#include <soundtouch/SoundTouch.h>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"

extern "C" {
#include "hal.h"
}

using namespace soundtouch;

struct HalAudioClip {
    float   *pcm;        // interleaved f32
    uint64_t frames;     // total frames (samples per channel)
    uint32_t rate;
    uint32_t channels;
};

static SoundTouch      g_st;
static SDL_AudioDeviceID g_dev = 0;
static uint32_t        g_dev_rate = 0, g_dev_ch = 0, g_dev_buf = 0;  // buf: device period, frames

static HalAudioClip   *g_cur = nullptr;
static uint64_t        g_src_feed = 0;    // source frames fed into SoundTouch
static double          g_src_played = 0;  // source frames actually heard (playhead)
static float           g_tempo = 1.0f;
static float           g_volume = 1.0f;
static bool            g_playing = false;
static bool            g_ended = false;

// --- SDL callback: pull tempo-adjusted frames out of SoundTouch --------------
static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    memset(stream, 0, len);
    if (!g_playing || !g_cur) return;

    const int ch = (int)g_dev_ch;
    const int out_frames = len / (ch * (int)sizeof(float));
    float *out = (float *)stream;

    int filled = 0;
    while (filled < out_frames) {
        // Ensure SoundTouch has output ready; feed source in chunks as needed.
        if ((int)g_st.numSamples() < (out_frames - filled)) {
            if (g_src_feed < g_cur->frames) {
                uint32_t chunk = 2048;
                if (g_src_feed + chunk > g_cur->frames)
                    chunk = (uint32_t)(g_cur->frames - g_src_feed);
                g_st.putSamples(g_cur->pcm + g_src_feed * g_cur->channels, chunk);
                g_src_feed += chunk;
                if (g_src_feed >= g_cur->frames) g_st.flush();  // drain tail
            }
        }
        uint got = g_st.receiveSamples(out + (size_t)filled * ch, out_frames - filled);
        if (got == 0) {
            if (g_src_feed >= g_cur->frames) {   // source done + ST drained => end
                g_playing = false;
                g_ended = true;
            }
            break;
        }
        filled += (int)got;
    }

    g_src_played += (double)filled * g_tempo;   // advance the original-timeline playhead
    if (g_volume != 1.0f)
        for (int i = 0; i < filled * ch; i++) out[i] *= g_volume;
}

// --- Device management -------------------------------------------------------
static void ensure_device(uint32_t rate, uint32_t ch)
{
    if (g_dev && g_dev_rate == rate && g_dev_ch == ch) return;
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = AUDIO_F32SYS;
    want.channels = (Uint8)ch;
    want.samples = 1024;
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    g_dev_rate = rate;
    g_dev_ch = ch;
    g_dev_buf = have.samples ? have.samples : want.samples;
    g_st.setSampleRate(rate);
    g_st.setChannels(ch);
    g_st.setTempo(g_tempo);
}

static void make_current(HalAudioClip *c)
{
    if (g_cur == c) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_cur = c;
    g_src_feed = 0;
    g_src_played = 0;
    g_ended = false;
    g_st.clear();
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    ensure_device(c->rate, c->channels);
}

// --- HAL audio API -----------------------------------------------------------
extern "C" HalAudioClip *hal_audio_open(const char *rel)
{
    uint8_t *data; size_t len;
    if (!hal_fs_slurp(rel, &data, &len)) return nullptr;

    drmp3_config cfg;
    drmp3_uint64 frames = 0;
    float *pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, len, &cfg, &frames, nullptr);
    free(data);
    if (!pcm) return nullptr;

    HalAudioClip *c = (HalAudioClip *)calloc(1, sizeof(HalAudioClip));
    c->pcm = pcm;
    c->frames = frames;
    c->rate = cfg.sampleRate;
    c->channels = cfg.channels;
    return c;
}

extern "C" void hal_audio_close(HalAudioClip *c)
{
    if (!c) return;
    if (g_cur == c) { if (g_dev) SDL_PauseAudioDevice(g_dev, 1); g_playing = false; g_cur = nullptr; }
    drmp3_free(c->pcm, nullptr);
    free(c);
}

extern "C" void hal_audio_play(HalAudioClip *c)
{
    if (!c) return;
    make_current(c);
    if (g_ended) {   // replay from start if the previous run finished
        SDL_LockAudioDevice(g_dev);
        g_src_feed = 0; g_src_played = 0; g_ended = false; g_st.clear();
        SDL_UnlockAudioDevice(g_dev);
    }
    g_playing = true;
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
}

extern "C" void hal_audio_pause(HalAudioClip *c)
{
    (void)c;
    g_playing = false;
    if (g_dev) SDL_PauseAudioDevice(g_dev, 1);
}

extern "C" bool hal_audio_is_playing(HalAudioClip *c) { (void)c; return g_playing; }

extern "C" uint32_t hal_audio_pos_ms(HalAudioClip *c)
{
    if (!c || c->rate == 0) return 0;
    double ms = g_src_played * 1000.0 / (double)c->rate;
    return (uint32_t)(ms < 0 ? 0 : ms);
}

extern "C" uint32_t hal_audio_len_ms(HalAudioClip *c)
{
    if (!c || c->rate == 0) return 0;
    return (uint32_t)(c->frames * 1000ull / c->rate);
}

extern "C" uint32_t hal_audio_latency_ms(HalAudioClip *c)
{
    // pos_ms is advanced when a period is written into the SDL device buffer
    // (audio_cb), one buffer-period before it is audible; SDL keeps ~2 periods
    // in flight. Convert that output-frame lead to the ORIGINAL recitation
    // timeline (pos_ms reports there), so scale by tempo — a slower tempo means
    // fewer source ms per output period.
    if (!c || g_dev_rate == 0 || g_dev_buf == 0) return 0;
    double out_ms = (double)(2u * g_dev_buf) * 1000.0 / (double)g_dev_rate;
    return (uint32_t)(out_ms * (double)g_tempo);
}

extern "C" void hal_audio_seek_ms(HalAudioClip *c, uint32_t ms)
{
    if (!c) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    uint64_t f = (uint64_t)ms * c->rate / 1000ull;
    if (f > c->frames) f = c->frames;
    g_src_feed = f;
    g_src_played = (double)f;
    g_ended = false;
    g_st.clear();
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

extern "C" void hal_audio_set_rate(HalAudioClip *c, float rate)
{
    (void)c;
    if (rate < 0.25f) rate = 0.25f;
    if (rate > 3.0f) rate = 3.0f;
    g_tempo = rate;
    g_st.setTempo(rate);
}

extern "C" void hal_audio_set_volume(float v) { g_volume = v; }
extern "C" void hal_audio_set_output(int speaker) { (void)speaker; }   // no amp in sim

// --- Raw PCM access ----------------------------------------------------------
extern "C" uint32_t hal_audio_read_pcm16(HalAudioClip *c, uint32_t start_ms,
                                         int16_t *out, uint32_t max_samples,
                                         uint32_t *out_hz)
{
    if (!c || !c->pcm || c->rate == 0) return 0;
    if (out_hz) *out_hz = c->rate;
    uint64_t start = (uint64_t)start_ms * c->rate / 1000ull;
    if (start >= c->frames) return 0;
    uint64_t n = c->frames - start;
    if (n > max_samples) n = max_samples;
    const int ch = (int)c->channels;
    for (uint64_t i = 0; i < n; i++) {
        // Mix channels to mono, clamp to s16.
        float s = 0.f;
        for (int k = 0; k < ch; k++) s += c->pcm[(start + i) * ch + k];
        s /= (float)ch;
        if (s > 1.f) s = 1.f; else if (s < -1.f) s = -1.f;
        out[i] = (int16_t)(s * 32767.f);
    }
    return (uint32_t)n;
}

// --- Microphone (host capture via SDL) ---------------------------------------
static SDL_AudioDeviceID g_mic_dev = 0;

// File "fake mic" (QN_FAKE_MIC=path.wav): stream a WAV in as if it were the
// microphone, so the Quran Teacher's live follower can be exercised in the sim
// WITHOUT host mic permission — and deterministically, for tuning. Any WAV is
// resampled to mono s16 @ the requested rate; a recitation of the current ayah
// gives meaningful verdicts, anything else just drives the UI.
static int16_t *g_fake_pcm = nullptr;
static uint32_t g_fake_n = 0, g_fake_pos = 0, g_fake_hz = 0;
static bool     g_fake_on = false;

static bool load_fake_mic(uint32_t hz)
{
    if (g_fake_pcm && g_fake_hz == hz) return true;
    const char *path = getenv("QN_FAKE_MIC");
    if (!path || !*path) return false;
    SDL_AudioSpec spec; Uint8 *wav = nullptr; Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &wav, &len)) return false;
    SDL_AudioCVT cvt;
    int r = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                              AUDIO_S16SYS, 1, (int)hz);
    if (r < 0) { SDL_FreeWAV(wav); return false; }
    cvt.len = (int)len;
    cvt.buf = (Uint8 *)malloc((size_t)cvt.len * (cvt.len_mult ? cvt.len_mult : 1));
    memcpy(cvt.buf, wav, len);
    SDL_FreeWAV(wav);
    if (r > 0 && SDL_ConvertAudio(&cvt) < 0) { free(cvt.buf); return false; }
    free(g_fake_pcm);
    g_fake_pcm = (int16_t *)cvt.buf;
    g_fake_n = (uint32_t)((r > 0 ? cvt.len_cvt : cvt.len) / sizeof(int16_t));
    g_fake_hz = hz;
    fprintf(stderr, "[mic] FAKE mic from %s -> %u samples @ %u Hz mono\n",
            path, g_fake_n, hz);
    return true;
}

extern "C" bool hal_mic_start(uint32_t hz)
{
    if (getenv("QN_FAKE_MIC") && load_fake_mic(hz)) {
        g_fake_on = true; g_fake_pos = 0;
        return true;
    }
    if (g_mic_dev) return true;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)hz;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = nullptr;   // queue (pull) mode
    // allowed_changes MUST be 0: the OS mic runs at 44.1/48k, and with
    // FREQUENCY_CHANGE allowed SDL hands us that raw rate while the caller
    // assumes `hz` — timestamps 3x fast, silence auto-stop firing mid-
    // recitation, words "not heard". 0 makes SDL resample to `hz` for us.
    g_mic_dev = SDL_OpenAudioDevice(nullptr, 1 /*capture*/, &want, &have, 0);
    if (!g_mic_dev) {
        // Usually macOS mic permission: a bare CLI binary often gets denied
        // silently. Say so, and point at the file fallback.
        fprintf(stderr, "[mic] no capture device: %s — SDL sees %d capture "
                "device(s). On macOS grant your terminal Microphone access in "
                "System Settings > Privacy & Security > Microphone (then restart "
                "the terminal), or run with QN_FAKE_MIC=path.wav to feed a file.\n",
                SDL_GetError(), SDL_GetNumAudioDevices(1));
        return false;
    }
    fprintf(stderr, "[mic] capture open: %d Hz, %d ch\n", have.freq, have.channels);
    SDL_PauseAudioDevice(g_mic_dev, 0);
    return true;
}

extern "C" int hal_mic_read(int16_t *buf, int max_samples)
{
    if (g_fake_on) {
        // Pace ~one 33ms tick of audio per call so the live coloring unfolds in
        // real time; trailing silence lets the endpointer auto-stop the take.
        int chunk = (int)(g_fake_hz * 33 / 1000);
        if (chunk > max_samples) chunk = max_samples;
        uint32_t silence_end = g_fake_n + g_fake_hz * 3;   // 3s tail then done
        if (g_fake_pos >= silence_end) return 0;
        for (int i = 0; i < chunk; i++)
            buf[i] = g_fake_pos + (uint32_t)i < g_fake_n
                       ? g_fake_pcm[g_fake_pos + i] : 0;
        g_fake_pos += (uint32_t)chunk;
        return chunk;
    }
    if (!g_mic_dev) return -1;
    Uint32 avail = SDL_GetQueuedAudioSize(g_mic_dev);
    Uint32 want = (Uint32)max_samples * sizeof(int16_t);
    if (avail > want) avail = want;
    if (avail < sizeof(int16_t)) return 0;
    Uint32 got = SDL_DequeueAudio(g_mic_dev, buf, avail);
    return (int)(got / sizeof(int16_t));
}

extern "C" void hal_mic_stop(void)
{
    if (g_fake_on) { g_fake_on = false; g_fake_pos = 0; return; }
    if (!g_mic_dev) return;
    SDL_CloseAudioDevice(g_mic_dev);
    g_mic_dev = 0;
}

// --- Raw PCM playback (the user's own recording) -----------------------------
static SDL_AudioDeviceID g_pcm_dev = 0;
static uint32_t g_pcm_hz = 0;

extern "C" void hal_pcm_play(const int16_t *pcm, uint32_t n_samples, uint32_t hz)
{
    if (g_pcm_dev && g_pcm_hz != hz) { SDL_CloseAudioDevice(g_pcm_dev); g_pcm_dev = 0; }
    if (!g_pcm_dev) {
        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = (int)hz;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.callback = nullptr;
        g_pcm_dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (!g_pcm_dev) return;
        g_pcm_hz = hz;
        SDL_PauseAudioDevice(g_pcm_dev, 0);
    }
    SDL_ClearQueuedAudio(g_pcm_dev);
    SDL_QueueAudio(g_pcm_dev, pcm, n_samples * sizeof(int16_t));
}

extern "C" void hal_pcm_stop(void)
{
    if (g_pcm_dev) SDL_ClearQueuedAudio(g_pcm_dev);
}

extern "C" bool hal_pcm_is_playing(void)
{
    return g_pcm_dev && SDL_GetQueuedAudioSize(g_pcm_dev) > 0;
}

// --- UI click ----------------------------------------------------------------
// A ~12ms decaying sine tick on its own small queue device, so it never touches
// the recitation path (no locks, no SoundTouch). Accent clicks ring brighter.
static SDL_AudioDeviceID g_click_dev = 0;

extern "C" void hal_audio_click(bool accent)
{
    if (!g_click_dev) {
        SDL_AudioSpec want;
        SDL_zero(want);
        want.freq = 22050;
        want.format = AUDIO_F32SYS;
        want.channels = 1;
        want.samples = 256;
        want.callback = nullptr;   // push (queue) mode
        g_click_dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (!g_click_dev) return;
        SDL_PauseAudioDevice(g_click_dev, 0);
    }
    const int N = 22050 * 12 / 1000;
    float buf[N];
    const float freq = accent ? 1760.0f : 1175.0f;
    for (int i = 0; i < N; i++) {
        float t = (float)i / 22050.0f;
        buf[i] = 0.16f * expf(-t * 420.0f) * sinf(6.2831853f * freq * t);
    }
    // Don't let rapid scrolling pile up latency: skip if a few are queued.
    if (SDL_GetQueuedAudioSize(g_click_dev) < 3 * sizeof(buf))
        SDL_QueueAudio(g_click_dev, buf, sizeof(buf));
}
