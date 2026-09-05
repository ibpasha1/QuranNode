// hal.h — the platform seam.
//
// Everything above this line (core/) is portable C99. Everything below it is
// platform I/O: the SDL desktop simulator (hal/sim) and, later, the ESP32-S3
// firmware (hal/esp32). The core never calls a platform API directly — it goes
// through these functions, so the identical core compiles and runs on both.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "input.h"

// --- Lifecycle -----------------------------------------------------------
// Bring up the platform (window/display, input, audio, filesystem root).
// Returns false on fatal init failure.
bool qn_hal_init(void);
void hal_shutdown(void);

// True until the user closes the window / powers down. The sim main loop runs
// while this is true.
bool hal_running(void);

// --- Display -------------------------------------------------------------
// Present one finished RGB565 frame (CANVAS_WIDTH x CANVAS_HEIGHT). The sim
// upscales into its window; firmware DMAs it to the panel.
void hal_display_push(const uint16_t *framebuffer);

// Panel backlight brightness, 0..100 (%). Firmware drives the LEDC backlight;
// the sim dims its output to preview it.
void hal_set_brightness(uint8_t percent);

// --- Input ---------------------------------------------------------------
// Pump the platform event queue. Non-blocking: returns true and fills *out with
// the next pending InputEvent, or false when the queue is empty. Call in a loop.
bool hal_input_poll(InputEvent *out);

// --- Filesystem (rooted at the SD mount) ---------------------------------
// Paths are relative to the SD root ("/sdcard" on device, "./sdcard" in the sim).
// hal_fs_slurp allocates *out_buf (caller frees) and reads the whole file.
bool hal_fs_slurp(const char *rel_path, uint8_t **out_buf, size_t *out_len);
bool hal_fs_exists(const char *rel_path);

// Directory listing: fills names[] (each up to 63 chars) with entries in dir,
// returns the count (capped at max). dirs_only filters to subdirectories.
int  hal_fs_list(const char *rel_dir, char names[][64], int max, bool dirs_only);

// --- Streaming file access (for large glyph packs) -----------------------
// Random-access reads without slurping the whole file — lets the reader stream
// one ayah at a time so a long surah's pack never has to fit in RAM. Backed by
// the SD card, or the embedded fallback in flash. hal_fs_pread reads `len` bytes
// at byte `offset` into buf; returns bytes read (may be < len at EOF) or <0.
typedef struct HalFile HalFile;
HalFile *hal_fs_open(const char *rel_path);   // NULL if absent
int      hal_fs_pread(HalFile *f, void *buf, size_t len, size_t offset);
void     hal_fs_close(HalFile *f);

// --- Persistent state (small key/blob store) -----------------------------
// Durable little blobs: resume point, bookmarks, saved loop routines, settings.
// Named store (sim: a file under the SD root's state/; esp32: NVS). Returns
// bytes read into buf (up to cap) via *out_len, or false if absent.
bool hal_state_save(const char *name, const void *data, size_t len);
bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len);

// --- Audio (per-file playback; implemented at M1a) -----------------------
// The player feeds the HAL a decoded-file handle abstraction: open a compressed
// audio file, play it, query/seek position in milliseconds, set playback rate
// (time-stretch, pitch-preserving). No-ops in the M0 build.
typedef struct HalAudioClip HalAudioClip;

HalAudioClip *hal_audio_open(const char *rel_path);   // NULL on failure
void   hal_audio_close(HalAudioClip *clip);
void   hal_audio_play(HalAudioClip *clip);            // (re)start playback
void   hal_audio_pause(HalAudioClip *clip);
bool   hal_audio_is_playing(HalAudioClip *clip);
uint32_t hal_audio_pos_ms(HalAudioClip *clip);        // current playhead
uint32_t hal_audio_len_ms(HalAudioClip *clip);        // total duration
void   hal_audio_seek_ms(HalAudioClip *clip, uint32_t ms);
void   hal_audio_set_rate(HalAudioClip *clip, float rate);  // 1.0 = normal, 0.85 = slower
void   hal_audio_set_volume(float vol);               // gain multiplier (1.0 = unity)
void   hal_audio_set_output(int speaker);             // 1 = speaker (amp on when playing), 0 = headphone

// --- Wall clock ----------------------------------------------------------
// Real-world time for the home clock + prayer times: seconds since the Unix
// epoch (UTC), or 0 when unknown (device before its clock is set). The tz
// offset is local-minus-UTC in minutes (sim: from the host; device: config).
int64_t hal_wall_clock(void);
int     hal_tz_offset_min(void);

// --- UI sounds -----------------------------------------------------------
// Short UI tick for menu scrolling / selection. Cheap and rate-safe; `accent`
// marks confirm clicks (slightly brighter tick).
void hal_audio_click(bool accent);

// --- Microphone (Quran Teacher recitation capture) ------------------------
// Mono s16 capture. hal_mic_start opens the device at (approximately) `hz`;
// hal_mic_read drains pending samples into buf (returns count, 0 = none yet,
// -1 = no mic on this platform). Sim: the host microphone via SDL; device:
// stubbed until an I2S mic (e.g. INMP441) is wired in.
bool hal_mic_start(uint32_t hz);
int  hal_mic_read(int16_t *buf, int max_samples);
void hal_mic_stop(void);

// --- Raw PCM access / playback (Quran Teacher analysis + playback) --------
// Read a clip's decoded audio as mono s16 starting at start_ms: fills out[]
// up to max_samples, returns the count and the sample rate via *out_hz.
// Feeds the recitation-similarity features; also used to slice word segments.
uint32_t hal_audio_read_pcm16(HalAudioClip *clip, uint32_t start_ms,
                              int16_t *out, uint32_t max_samples,
                              uint32_t *out_hz);

// Play a raw mono s16 buffer (the user's own recording). Fire-and-forget;
// a second call replaces the first. hal_pcm_stop() cuts it short.
void hal_pcm_play(const int16_t *pcm, uint32_t n_samples, uint32_t hz);
void hal_pcm_stop(void);
bool hal_pcm_is_playing(void);

// --- OTA firmware update -------------------------------------------------
// Bring up Wi-Fi + the HTTP upload server (device only; a no-op/simulated stub
// elsewhere). hal_ota_url() returns "http://<ip>/" once connected, else NULL.
void        hal_ota_start(void);
const char *hal_ota_url(void);

// Pull the latest firmware from the configured GitHub release over HTTPS and
// self-flash (the field-update path). Also brings Wi-Fi up so the local push
// URL still works as a fallback. hal_ota_status() reports progress for the UI.
void        hal_ota_pull(void);
const char *hal_ota_status(void);

// Boot-time auto-update: connect Wi-Fi and compare the latest release version to
// this build. Returns true (Wi-Fi left up) if an update is available — the caller
// then shows a screen and calls hal_ota_apply(). Returns false (Wi-Fi down) if up
// to date or offline. hal_ota_apply() downloads+flashes+reboots (blocking).
bool        hal_ota_boot_check(void);
void        hal_ota_apply(void);

// True if the user is holding the recovery combo at boot (5-way center). Lets a
// sealed unit force Wi-Fi update mode even if the normal UI is broken.
bool        hal_recovery_requested(void);
