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
// Best-effort async preload: pull this clip's bytes off the SD card on a
// background worker so a later hal_audio_open(rel_path) for the SAME path
// returns instantly instead of stalling on a multi-MB read — which is what
// gaps the audio (and starves neighbouring reads) at an ayah transition. Only
// the most recent request is kept; a miss just falls back to a synchronous open.
void   hal_audio_prefetch(const char *rel_path);
void   hal_audio_close(HalAudioClip *clip);
void   hal_audio_play(HalAudioClip *clip);            // (re)start playback
void   hal_audio_pause(HalAudioClip *clip);
bool   hal_audio_is_playing(HalAudioClip *clip);
// Clip-agnostic "is any recitation audio playing right now" — for the idle
// dimmer to stay lit during playback started outside the main player (Recite
// read-along, Library media), which hal_audio_is_playing(a specific clip) misses.
bool   hal_audio_active(void);
bool   hal_mic_active(void);   // is the mic currently capturing (recite/your turn)
uint32_t hal_audio_pos_ms(HalAudioClip *clip);        // current playhead
uint32_t hal_audio_len_ms(HalAudioClip *clip);        // total duration
// Output latency: how far pos_ms LEADS the sound actually leaving the speaker,
// i.e. the depth of the buffer between "handed to the output" and "audible"
// (SDL device buffer in the sim, I2S DMA on device). Subtract it from pos_ms
// before a word-highlight lookup so the highlight tracks what's heard, not
// what's queued. 0 if unknown/irrelevant.
uint32_t hal_audio_latency_ms(HalAudioClip *clip);
void   hal_audio_seek_ms(HalAudioClip *clip, uint32_t ms);
void   hal_audio_set_rate(HalAudioClip *clip, float rate);  // 1.0 = normal, 0.85 = slower
void   hal_audio_set_volume(float vol);               // gain multiplier (1.0 = unity)
void   hal_audio_set_output(int mode);                // 0 = headphone, 1 = speaker, 2 = auto (follow jack)
int    hal_audio_headphone_present(void);             // 1 = a plug is inserted (jack-detect), 0 = not / no detect

// --- Wall clock ----------------------------------------------------------
// Real-world time for the home clock + prayer times: seconds since the Unix
// epoch (UTC), or 0 when unknown (device before its clock is set). The tz
// offset is local-minus-UTC in minutes (sim: from the host; device: config).
int64_t hal_wall_clock(void);
int     hal_tz_offset_min(void);

// How much to trust hal_wall_clock(). The device has no RTC, so between boots
// it can only restore the last epoch it saw and carry it forward on the
// monotonic timer — good enough to bucket reading by day, but not to the
// minute. Callers that show a wall time (or gate daily goals) check this.
typedef enum {
    QN_CLOCK_UNKNOWN = 0,   // never synced: hal_wall_clock() returns 0
    QN_CLOCK_RESTORED,      // carried forward from the last known epoch
    QN_CLOCK_SYNCED,        // set by SNTP this session (or the host, in the sim)
} QnClockSource;

QnClockSource hal_clock_source(void);
void          hal_clock_persist(void);   // save the current epoch (no-op in sim)

// Manually set the wall clock to `epoch` (seconds since the Unix epoch, UTC).
// For the offline "set the time by hand" path when there's no NTP. Marks the
// clock authoritative (SYNCED) and persists it. In the sim it shifts the
// reported time by a stored delta (the host clock isn't touched).
void          hal_clock_set(int64_t epoch);

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

// --- Remote recitation scoring (Quran Teacher V2; docs/TEACHER_V2.md) ------
// POST a WAV take to the configured scoring server and parse its per-word
// CSV verdicts. verdict values follow ReciteVerdict order: 0=GOOD 1=UNSURE
// 2=MISMATCH 3=MISSING 4=UNCLEAR. Returns the number of words scored, or 0
// when unavailable (no server configured / offline / timeout / bad reply) —
// the caller falls back to the local engine. Blocks up to ~12s.
typedef struct {
    uint8_t  verdict;
    float    score;
    uint32_t start_ms, end_ms;   // 0,0 until the server sends timestamps (M3)
} RemoteWord;

int hal_score_remote(const uint8_t *wav, uint32_t wav_len, int surah, int ayah,
                     RemoteWord *out, int max_words);

// Register an in-RAM blob for the OTA web server to list at /takes and serve
// at /takes/<idx> (training-take download when the SD card is unreliable).
// The memory stays owned by the caller and must outlive the registration;
// data=NULL clears the slot. No-op off-device.
void        hal_serve_blob(int idx, const char *name, const void *data, size_t len);

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

// Background boot check (used by the splash instead of a blocking boot_check):
// hal_ota_check_start() spawns a one-shot Wi-Fi probe that compares the latest
// release to this build and grabs NTP time, then drops Wi-Fi. Non-blocking and
// idempotent; an instant no-op offline. hal_ota_update_available() reports true
// once a newer release was seen, so Home can badge it and the user can pull it
// from Settings > Update firmware (hal_ota_pull).
void        hal_ota_check_start(void);
bool        hal_ota_update_available(void);

// True if the user is holding the recovery combo at boot (5-way center). Lets a
// sealed unit force Wi-Fi update mode even if the normal UI is broken.
bool        hal_recovery_requested(void);

// --- Wi-Fi provisioning (SoftAP + captive-portal onboarding) --------------
// First-run flow: the device raises its own open access point and a captive
// portal; the user scans the on-screen QR to join it, picks their home network
// and types the password. Creds are saved to NVS and used on every boot after
// (hal_wifi_have_creds); the build-flag secrets.ini stays a dev fallback. All
// no-ops in the sim.
typedef enum {
    QN_PROV_IDLE = 0,     // not provisioning
    QN_PROV_AP,           // setup AP up, waiting for the user's credentials
    QN_PROV_CONNECTING,   // got credentials, joining the home network
    QN_PROV_CONNECTED,    // joined; creds saved
    QN_PROV_FAILED,       // last join attempt failed (bad password / not found)
} QnProvState;

bool         hal_wifi_have_creds(void);       // are Wi-Fi credentials stored?
void         hal_wifi_provision_start(void);  // raise the setup AP + portal
void         hal_wifi_provision_stop(void);   // tear it down
const char  *hal_wifi_ap_ssid(void);          // setup AP SSID, for the QR (NULL if down)
const char  *hal_wifi_ap_pass(void);          // setup AP password, for the QR (NULL/"" = open)
const char  *hal_wifi_sta_ssid(void);         // the network being joined (NULL if none)
QnProvState  hal_wifi_prov_state(void);        // poll for the setup scene

// --- Classroom sync (ESP-NOW peer broadcast) ------------------------------
// "Follow the Sheikh": a leader device broadcasts its reading position and every
// follower in radio range converges on it — pages turn together, the same word
// highlights. Connectionless (ESP-NOW: no router, no pairing). The leader keeps
// ASSERTING an idempotent position (on change + a heartbeat), so a follower that
// just joined or dropped a packet resyncs on the next beacon. Followers keep the
// highest seq and ignore stale / duplicate / out-of-order packets.
//
// In the sim there's no radio: hal_mesh_* is backed by a shared file under the SD
// root's state/, so two simulator instances on one machine genuinely follow each
// other. All calls are cheap no-ops until hal_mesh_start().
enum { QN_SYNC_MAGIC = 0x51, QN_SYNC_VERSION = 1 };   // 'Q'
enum { QN_SYNC_PLAYING = 1u << 0 };                   // flags: leader is playing

typedef struct {
    uint8_t  magic;      // QN_SYNC_MAGIC — stray/foreign packets are dropped
    uint8_t  version;    // QN_SYNC_VERSION
    uint8_t  surah;      // 1..114
    uint8_t  word;       // highlighted glyph-word index, 0xFF = none
    uint16_t ayah;       // 1..
    uint16_t pin;        // circle PIN so followers can tell two circles apart
    uint8_t  flags;      // QN_SYNC_PLAYING
    uint8_t  _pad;
    uint32_t seq;        // monotonic per leader; followers keep the highest
} QnSyncMsg;

void hal_mesh_start(void);                    // bring ESP-NOW up (idempotent)
void hal_mesh_stop(void);                     // tear it down
void hal_mesh_broadcast(const QnSyncMsg *m);  // leader: assert current position
bool hal_mesh_poll(QnSyncMsg *out);           // follower: newest msg since last poll
