// hal_esp32.c — ESP32-S3 implementation of the platform seam (Phase 1).
//
// Binds the portable core to the DSP-Mini hardware: the ST7796S panel (native
// 480x320 — the reused driver's upscale collapses to an identity copy at our
// canvas size), the 5-way navigation switch, and the microSD card (FAT at
// /sdcard) for glyph packs / audio / timings / saved state. Audio is stubbed here
// and implemented in audio_esp32.c at M2-3.
//
// Note: DSP-Mini's input_5way.h/encoder_i2c.h define their OWN InputEvent enum
// that clashes with core/input.h, so input is read fresh here rather than reusing
// those TUs.
#include "hal.h"
#include "canvas.h"
#include "display_mgr.h"
#include "sd_card.h"
#include "ota.h"
#include "pin_config.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "HAL";

// Implemented in audio_esp32.c (I2S + MP3 decode).
void audio_esp32_init(void);

uint32_t plat_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Wall clock: valid once SNTP (or a future RTC) has set system time; an unset
// clock reads as ~1970 which we report as "unknown" so the UI can say so.
// TODO(wifi): start SNTP after the OTA Wi-Fi bring-up so this becomes real.
int64_t hal_wall_clock(void)
{
    time_t t = time(NULL);
    return t > 1600000000 ? (int64_t)t : 0;
}

// TODO: expose in Settings; US Eastern (EDT) default for now.
int hal_tz_offset_min(void) { return -240; }

// -------------------------------------------------------------------------
// 5-way navigation switch (active-low GPIOs, internal pull-ups, COM->GND). This
// board has a real DOWN. Center short = SELECT, center long-press = BACK.
// -------------------------------------------------------------------------
typedef struct { int pin; InputEventType ev; } NavKey;

static const NavKey NAV_KEYS[] = {
    { PIN_SW_UP,    INPUT_NAV_UP },
    { PIN_SW_DOWN,  INPUT_NAV_DOWN },
    { PIN_SW_LEFT,  INPUT_NAV_LEFT },
    { PIN_SW_RIGHT, INPUT_NAV_RIGHT },
};
#define NAV_N ((int)(sizeof(NAV_KEYS) / sizeof(NAV_KEYS[0])))
#define LONG_PRESS_MS 500
// Hold-to-scroll: after holding a direction REPEAT_DELAY_MS, auto-repeat starting
// at REPEAT_START_MS and accelerating to REPEAT_MIN_MS (fast-scroll long lists).
#define REPEAT_DELAY_MS 350
#define REPEAT_START_MS 180
#define REPEAT_MIN_MS    40

static bool s_nav_down[NAV_N];       // debounced pressed state
static uint32_t s_nav_t0[NAV_N];     // press time (auto-repeat delay)
static uint32_t s_nav_last[NAV_N];   // last repeat emitted
static int      s_nav_reps[NAV_N];   // repeats this hold (acceleration)
static bool s_center_down;
static uint32_t s_center_t0;
static bool s_center_long_sent;

static void input_init(void)
{
    uint64_t mask = 1ULL << PIN_SW_MID;
    for (int i = 0; i < NAV_N; i++)
        if (NAV_KEYS[i].pin >= 0) mask |= 1ULL << NAV_KEYS[i].pin;
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

// Poll the switch; emit at most one event per call (queued in *out).
static bool poll_nav(InputEvent *out)
{
    // Directional keys: emit on press edge, then auto-repeat while held.
    for (int i = 0; i < NAV_N; i++) {
        if (NAV_KEYS[i].pin < 0) continue;
        bool down = gpio_get_level(NAV_KEYS[i].pin) == 0;
        uint32_t now = plat_millis();
        if (down && !s_nav_down[i]) {                 // press edge
            s_nav_down[i] = true; s_nav_t0[i] = now; s_nav_last[i] = now; s_nav_reps[i] = 0;
            out->type = NAV_KEYS[i].ev; out->encoder_id = ENC_NAV; out->pressed = true;
            return true;
        }
        if (down && s_nav_down[i] && now - s_nav_t0[i] >= REPEAT_DELAY_MS) {   // auto-repeat
            uint32_t iv = REPEAT_START_MS - (uint32_t)s_nav_reps[i] * 12;
            if (iv < REPEAT_MIN_MS) iv = REPEAT_MIN_MS;
            if (now - s_nav_last[i] >= iv) {
                s_nav_last[i] = now; s_nav_reps[i]++;
                out->type = NAV_KEYS[i].ev; out->encoder_id = ENC_NAV; out->pressed = true;
                return true;
            }
        }
        if (!down) s_nav_down[i] = false;
    }

    // Center: short press = SELECT (on release), long hold = BACK (while held).
    bool cdown = gpio_get_level(PIN_SW_MID) == 0;
    if (cdown && !s_center_down) {
        s_center_down = true; s_center_t0 = plat_millis(); s_center_long_sent = false;
    } else if (cdown && s_center_down && !s_center_long_sent &&
               plat_millis() - s_center_t0 >= LONG_PRESS_MS) {
        s_center_long_sent = true;
        out->type = INPUT_BTN_BACK; out->encoder_id = ENC_NAV; out->pressed = true;
        return true;
    } else if (!cdown && s_center_down) {
        s_center_down = false;
        if (!s_center_long_sent) {
            out->type = INPUT_NAV_SELECT; out->encoder_id = ENC_NAV; out->pressed = true;
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------------------
// Serial keyboard input — drive the device from `pio device monitor` when the
// physical buttons/encoder aren't wired. Reads the console UART (UART0, the port
// the monitor connects to) and maps keystrokes to the SAME keys as the simulator.
//
//   arrows / WASD : navigate        Enter : select        Space : play/pause
//   , or q : encoder CCW            . or e : encoder CW    r : encoder press
//   Backspace / b : back            Tab : mode(loop)       k : bookmark   m : menu
// -------------------------------------------------------------------------
#define SER_UART UART_NUM_0

static InputEvent s_evq[32];
static int s_evh, s_evt;

static void evq_push(InputEventType t)
{
    int nx = (s_evt + 1) % 32;
    if (nx == s_evh) return;
    s_evq[s_evt].type = t; s_evq[s_evt].encoder_id = ENC_NAV; s_evq[s_evt].pressed = true;
    s_evt = nx;
}

static void serial_init(void)
{
    // Read BOTH serial paths so control works regardless of which USB port the
    // monitor is on: UART0 (the CP2102 "UART" port) and USB-Serial-JTAG (the
    // native "USB" port — what most S3 devkits enumerate for monitoring).
    // Idempotent installs; INVALID_STATE (console already owns it) is fine.
    uart_driver_install(SER_UART, 512, 0, 0, NULL, 0);
    usb_serial_jtag_driver_config_t jc = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&jc);
}

static void parse_bytes(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        // Arrow keys: ESC [ A/B/C/D
        if (c == 0x1B && i + 2 < n && b[i + 1] == '[') {
            switch (b[i + 2]) {
            case 'A': evq_push(INPUT_NAV_UP); break;
            case 'B': evq_push(INPUT_NAV_DOWN); break;
            case 'C': evq_push(INPUT_NAV_RIGHT); break;
            case 'D': evq_push(INPUT_NAV_LEFT); break;
            }
            i += 2; continue;
        }
        switch (c) {
        case 'w': case 'W': evq_push(INPUT_NAV_UP); break;
        case 's': case 'S': evq_push(INPUT_NAV_DOWN); break;
        case 'a': case 'A': evq_push(INPUT_NAV_LEFT); break;
        case 'd': case 'D': evq_push(INPUT_NAV_RIGHT); break;
        case '\r': case '\n': evq_push(INPUT_NAV_SELECT); break;
        case ' ': evq_push(INPUT_BTN_PLAY); break;
        case ',': case 'q': case 'Q': evq_push(INPUT_ENC_CCW); break;
        case '.': case 'e': case 'E': evq_push(INPUT_ENC_CW); break;
        case 'r': case 'R': evq_push(INPUT_ENC_PUSH); break;
        case 0x7F: case 0x08: case 'b': case 'B': evq_push(INPUT_BTN_BACK); break;
        case '\t': evq_push(INPUT_BTN_MODE); break;
        case 'k': case 'K': evq_push(INPUT_BTN_BOOKMARK); break;
        case 'm': case 'M': evq_push(INPUT_BTN_MENU); break;
        case 'l': case 'L': evq_push(INPUT_NAV_LEFT); break;   // alias: Library from Home
        case 'u': case 'U': ota_start(); break;                // TEST: enter Wi-Fi OTA update mode
        default: break;
        }
    }
}

static void serial_pump(void)
{
    uint8_t b[48];
    int n = uart_read_bytes(SER_UART, b, sizeof(b), 0);
    if (n > 0) parse_bytes(b, n);
    n = usb_serial_jtag_read_bytes(b, sizeof(b), 0);
    if (n > 0) parse_bytes(b, n);
}

// Recovery trigger: 5-way center held at boot. input_init() has already set up
// PIN_SW_MID as input w/ pull-up (active-low), so held == level 0.
bool hal_recovery_requested(void)
{
    return gpio_get_level(PIN_SW_MID) == 0;
}

// -------------------------------------------------------------------------
// HAL API
// -------------------------------------------------------------------------
bool qn_hal_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_err_t sdret = sd_card_mount();
    if (sdret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount FAILED (%s) — check card inserted, FAT32, wiring "
                      "(SPI3 MOSI2/CLK42/MISO41/CS1)", esp_err_to_name(sdret));
    } else {
        ESP_LOGI(TAG, "SD mounted at /sdcard");
        ESP_LOGI(TAG, "  packs/reader_lg/1.qgp : %s",
                 hal_fs_exists("packs/reader_lg/1.qgp") ? "FOUND" : "MISSING (copy repo sdcard/ to card)");
        ESP_LOGI(TAG, "  packs/reader_lg/78.qgp (Juz Amma) : %s",
                 hal_fs_exists("packs/reader_lg/78.qgp") ? "FOUND" : "MISSING");
        ESP_LOGI(TAG, "  audio/abdulbasit/78/1.mp3 : %s",
                 hal_fs_exists("audio/abdulbasit/78/1.mp3") ? "FOUND" : "MISSING");
    }

    if (display_mgr_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
        return false;
    }
    display_mgr_set_brightness(90);
    input_init();
    serial_init();
    audio_esp32_init();   // I2S + MP3 decode (defined in audio_esp32.c)
    ESP_LOGI(TAG, "Al-Fatihah bundled in flash — reader + audio work without SD");
    ESP_LOGI(TAG, "HAL ready (480x320 ST7796S; input: serial keys + 5-way; SD + I2S)");
    ESP_LOGI(TAG, "Serial controls: arrows/WASD nav, Enter select, Space play, "
                  ",/. encoder, r press, b back, Tab mode, k bookmark, m menu");
    return true;
}

void hal_shutdown(void) {}
bool hal_running(void) { return true; }

void hal_display_push(const uint16_t *fb)
{
    display_mgr_push_frame((const uint8_t *)fb);
}

void hal_set_brightness(uint8_t percent)
{
    display_mgr_set_brightness(percent);
}

bool hal_input_poll(InputEvent *out)
{
    // Serial keyboard first (dev control path), then the physical 5-way switch.
    if (s_evh == s_evt) serial_pump();
    if (s_evh != s_evt) {
        *out = s_evq[s_evh];
        s_evh = (s_evh + 1) % 32;
        return true;
    }
    return poll_nav(out);
}

// --- Filesystem (rooted at /sdcard) --------------------------------------
static void sd_path(char *dst, size_t n, const char *rel)
{
    snprintf(dst, n, "/sdcard/%s", rel);
}

// --- Al-Fatihah embedded in flash (generated by tools/gen_embedded.py) --------
#include "embedded_assets.h"

// No-SD safety net: just Al-Fatihah at the default Large size (plain + tajweed)
// plus its recitation. Everything else — all sizes, all surahs — comes from SD.
static const struct { const char *path; const uint8_t *s; const size_t *len; } EMBEDDED[] = {
    { "packs/reader_lg/1.qgp",    qn_pack_lg, &qn_pack_lg_len },
    { "packs/reader_lg_tj/1.qgp", qn_pack_lg_tj, &qn_pack_lg_tj_len },
    { "quran/timings/1.qtm",      qn_qtm1,  &qn_qtm1_len },
    { "audio/abdulbasit/1/1.mp3", qn_mp3_1, &qn_mp3_1_len },
    { "audio/abdulbasit/1/2.mp3", qn_mp3_2, &qn_mp3_2_len },
    { "audio/abdulbasit/1/3.mp3", qn_mp3_3, &qn_mp3_3_len },
    { "audio/abdulbasit/1/4.mp3", qn_mp3_4, &qn_mp3_4_len },
    { "audio/abdulbasit/1/5.mp3", qn_mp3_5, &qn_mp3_5_len },
    { "audio/abdulbasit/1/6.mp3", qn_mp3_6, &qn_mp3_6_len },
    { "audio/abdulbasit/1/7.mp3", qn_mp3_7, &qn_mp3_7_len },
};

bool hal_fs_slurp(const char *rel, uint8_t **out_buf, size_t *out_len)
{
    // SD card is the primary content store — try it first so the full library
    // (and any updated content) is authoritative.
    char path[300]; sd_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz >= 0) {
            uint8_t *buf = malloc((size_t)sz);
            if (buf) {
                size_t got = fread(buf, 1, (size_t)sz, f);
                fclose(f);
                if (got == (size_t)sz) { *out_buf = buf; *out_len = got; return true; }
                free(buf);
                return false;
            }
        }
        fclose(f);
    }

    // Fallback: the small Al-Fatihah safety net bundled in flash, so the device
    // still reads + recites even if the SD card is missing or failed. Copied into
    // a heap buffer so the caller's free() matches the SD path's contract.
    for (size_t i = 0; i < sizeof(EMBEDDED) / sizeof(EMBEDDED[0]); i++) {
        if (strcmp(rel, EMBEDDED[i].path) == 0) {
            size_t len = *EMBEDDED[i].len;
            uint8_t *buf = malloc(len);
            if (!buf) return false;
            memcpy(buf, EMBEDDED[i].s, len);
            *out_buf = buf; *out_len = len;
            return true;
        }
    }
    return false;
}

bool hal_fs_exists(const char *rel)
{
    char path[300]; sd_path(path, sizeof(path), rel);
    struct stat st;
    return stat(path, &st) == 0;
}

// --- Streaming random-access reads (SD file, else embedded flash array) ----
struct HalFile { FILE *fp; const uint8_t *mem; size_t mem_len; };

HalFile *hal_fs_open(const char *rel)
{
    char path[300]; sd_path(path, sizeof(path), rel);
    FILE *fp = fopen(path, "rb");
    HalFile *f = calloc(1, sizeof(*f));
    if (!f) { if (fp) fclose(fp); return NULL; }
    if (fp) { f->fp = fp; return f; }
    for (size_t i = 0; i < sizeof(EMBEDDED) / sizeof(EMBEDDED[0]); i++) {
        if (strcmp(rel, EMBEDDED[i].path) == 0) {
            f->mem = EMBEDDED[i].s; f->mem_len = *EMBEDDED[i].len;
            return f;
        }
    }
    free(f);
    return NULL;
}

int hal_fs_pread(HalFile *f, void *buf, size_t len, size_t offset)
{
    if (!f) return -1;
    if (f->fp) {
        if (fseek(f->fp, (long)offset, SEEK_SET) != 0) return -1;
        return (int)fread(buf, 1, len, f->fp);
    }
    if (offset >= f->mem_len) return 0;
    size_t n = len;
    if (offset + n > f->mem_len) n = f->mem_len - offset;
    memcpy(buf, f->mem + offset, n);
    return (int)n;
}

void hal_fs_close(HalFile *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

int hal_fs_list(const char *rel, char names[][64], int max, bool dirs_only)
{
    char path[300]; sd_path(path, sizeof(path), rel);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (dirs_only) {
            char full[380]; snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

// --- Persistent state (files under /sdcard/state) ------------------------
bool hal_state_save(const char *name, const void *data, size_t len)
{
    mkdir("/sdcard/state", 0755);
    char path[300]; snprintf(path, sizeof(path), "/sdcard/state/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len)
{
    char path[300]; snprintf(path, sizeof(path), "/sdcard/state/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    if (out_len) *out_len = n;
    return true;
}
