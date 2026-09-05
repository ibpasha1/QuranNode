// hal_sim.c — desktop (SDL2) implementation of the platform seam.
//
// Puts the 480x320 canvas in a resizable window (2x by default), maps the
// keyboard + mouse wheel onto the device's encoder/keys, and serves files from
// ./sdcard (mirroring the on-device /sdcard layout). Audio playback is stubbed
// here in M0 and filled in at M1a (dr_mp3 + SoundTouch).
#include "hal.h"
#include "canvas.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#define SIM_SCALE   2
#define SD_ROOT     "sdcard"

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture  *s_tex;      // ARGB8888, CANVAS_WIDTH x CANVAS_HEIGHT
static uint32_t     *s_argb;     // decode target
static bool          s_running;

// --- Input event queue ---------------------------------------------------
#define EVQ_CAP 64
static InputEvent s_evq[EVQ_CAP];
static int s_ev_head, s_ev_tail;

static void evq_push(InputEventType t)
{
    int next = (s_ev_tail + 1) % EVQ_CAP;
    if (next == s_ev_head) return;   // full: drop
    s_evq[s_ev_tail].type = t;
    s_evq[s_ev_tail].encoder_id = ENC_NAV;
    s_evq[s_ev_tail].pressed = true;
    s_ev_tail = next;
}

static InputEventType map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP:        return INPUT_NAV_UP;
    case SDLK_DOWN:      return INPUT_NAV_DOWN;
    case SDLK_LEFT:      return INPUT_NAV_LEFT;
    case SDLK_RIGHT:     return INPUT_NAV_RIGHT;
    case SDLK_RETURN:    return INPUT_NAV_SELECT;
    case SDLK_SPACE:     return INPUT_BTN_PLAY;
    case SDLK_COMMA:     return INPUT_ENC_CCW;   // rotary: , = left
    case SDLK_PERIOD:    return INPUT_ENC_CW;    // rotary: . = right
    case SDLK_r:         return INPUT_ENC_PUSH;  // encoder click
    case SDLK_BACKSPACE: return INPUT_BTN_BACK;
    case SDLK_b:         return INPUT_BTN_BACK;
    case SDLK_TAB:       return INPUT_BTN_MODE;
    case SDLK_k:         return INPUT_BTN_BOOKMARK;
    case SDLK_m:         return INPUT_BTN_MENU;
    case SDLK_SLASH:     return INPUT_BTN_HELP;
    default:             return INPUT_NONE;
    }
}

// Drain the SDL event queue into our InputEvent queue.
static void sim_pump(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s_running = false;
            break;
        case SDL_KEYDOWN: {
            if (e.key.keysym.sym == SDLK_ESCAPE) { s_running = false; break; }
            // long-press variants with shift held (handy for testing)
            InputEventType t = map_key(e.key.keysym.sym);
            if (t == INPUT_NAV_SELECT && (e.key.keysym.mod & KMOD_SHIFT)) t = INPUT_NAV_SELECT_LONG;
            if (t == INPUT_ENC_PUSH  && (e.key.keysym.mod & KMOD_SHIFT)) t = INPUT_ENC_PUSH_LONG;
            if (t != INPUT_NONE) evq_push(t);
            break;
        }
        case SDL_MOUSEWHEEL:
            evq_push(e.wheel.y > 0 ? INPUT_ENC_CW : INPUT_ENC_CCW);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) evq_push(INPUT_ENC_PUSH);
            break;
        }
    }
}

bool qn_hal_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    s_win = SDL_CreateWindow("QuranNode (sim)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             CANVAS_WIDTH * SIM_SCALE, CANVAS_HEIGHT * SIM_SCALE,
                             SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return false; }
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
    SDL_RenderSetLogicalSize(s_ren, CANVAS_WIDTH, CANVAS_HEIGHT);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, CANVAS_WIDTH, CANVAS_HEIGHT);
    s_argb = malloc((size_t)CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(uint32_t));
    s_running = true;

    printf("QuranNode simulator\n"
           "  Arrows: navigate    Enter: select    Space: play/pause\n"
           "  , / .  or wheel: rotary encoder    R / click: encoder press\n"
           "  Backspace/B: back   Tab: mode   K: bookmark   M: menu   Esc: quit\n");
    return true;
}

void hal_shutdown(void)
{
    if (s_argb) free(s_argb);
    if (s_tex) SDL_DestroyTexture(s_tex);
    if (s_ren) SDL_DestroyRenderer(s_ren);
    if (s_win) SDL_DestroyWindow(s_win);
    SDL_Quit();
}

bool hal_running(void) { return s_running; }

// Decode DSP-Mini's byte-swapped RGB565 (see the RGB565 macro in canvas.h) into
// ARGB8888 for SDL. The stored 16-bit value packs: bits15-13 g[4:2], bits12-8
// b[7:3], bits7-3 r[7:3], bits2-0 g[7:5].
static uint8_t s_brightness = 100;
void hal_set_brightness(uint8_t percent) { s_brightness = percent > 100 ? 100 : percent; }

void hal_display_push(const uint16_t *fb)
{
    uint32_t bri = s_brightness;
    for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT; i++) {
        uint16_t v = fb[i];
        uint32_t r5 = (v >> 3) & 0x1F;
        uint32_t b5 = (v >> 8) & 0x1F;
        uint32_t g6 = ((v & 0x7) << 3) | ((v >> 13) & 0x7);
        uint32_t r = (r5 << 3) | (r5 >> 2);
        uint32_t g = (g6 << 2) | (g6 >> 4);
        uint32_t b = (b5 << 3) | (b5 >> 2);
        if (bri < 100) { r = r * bri / 100; g = g * bri / 100; b = b * bri / 100; }
        s_argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    SDL_UpdateTexture(s_tex, NULL, s_argb, CANVAS_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    SDL_RenderPresent(s_ren);
}

bool hal_input_poll(InputEvent *out)
{
    if (s_ev_head == s_ev_tail) sim_pump();
    if (s_ev_head == s_ev_tail) return false;
    *out = s_evq[s_ev_head];
    s_ev_head = (s_ev_head + 1) % EVQ_CAP;
    return true;
}

uint32_t plat_millis(void) { return SDL_GetTicks(); }

// --- Wall clock (host time) ------------------------------------------------
int64_t hal_wall_clock(void) { return (int64_t)time(NULL); }

int hal_tz_offset_min(void)
{
    time_t t = time(NULL);
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int d = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
    // Correct for the pair straddling midnight (yday differs by ±1 or wraps).
    int dd = lt.tm_yday - gt.tm_yday;
    if (dd == 1 || dd < -1) d += 1440;
    else if (dd == -1 || dd > 1) d -= 1440;
    return d;
}

// --- Filesystem (rooted at ./sdcard) -------------------------------------
static void sd_path(char *dst, size_t n, const char *rel)
{
    snprintf(dst, n, "%s/%s", SD_ROOT, rel);
}

bool hal_fs_slurp(const char *rel, uint8_t **out_buf, size_t *out_len)
{
    char path[512]; sd_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return false; }
    *out_buf = buf; *out_len = got;
    return true;
}

bool hal_fs_exists(const char *rel)
{
    char path[512]; sd_path(path, sizeof(path), rel);
    struct stat st;
    return stat(path, &st) == 0;
}

int hal_fs_list(const char *rel, char names[][64], int max, bool dirs_only)
{
    char path[512]; sd_path(path, sizeof(path), rel);
    DIR *d = opendir(path);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while (n < max && (ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (dirs_only) {
            char full[600]; snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        }
        snprintf(names[n], 64, "%s", ent->d_name);
        n++;
    }
    closedir(d);
    return n;
}

// --- Persistent state (files under ./sdcard/state) -----------------------
bool hal_state_save(const char *name, const void *data, size_t len)
{
    mkdir(SD_ROOT "/state", 0755);
    char path[512]; snprintf(path, sizeof(path), "%s/state/%s", SD_ROOT, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len)
{
    char path[512]; snprintf(path, sizeof(path), "%s/state/%s", SD_ROOT, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    if (out_len) *out_len = n;
    return true;
}

// --- OTA (no real update path in the sim; return a demo URL so the Settings
//     "Update firmware" screen previews correctly) --------------------------
static bool s_sim_ota = false;
void hal_ota_start(void) { s_sim_ota = true; printf("[sim] OTA update mode (no-op)\n"); }
const char *hal_ota_url(void) { return s_sim_ota ? "http://192.168.86.20/" : NULL; }
void hal_ota_pull(void) { s_sim_ota = true; printf("[sim] OTA pull from GitHub (no-op)\n"); }
const char *hal_ota_status(void) { return s_sim_ota ? "Downloading update..." : NULL; }
bool hal_ota_boot_check(void) { return false; }   // no auto-update in the sim
void hal_ota_apply(void) { printf("[sim] OTA apply (no-op)\n"); }
bool hal_recovery_requested(void) { return false; }

// Audio (hal_audio_*) lives in hal/sim/audio_sim.cpp — dr_mp3 decode + SoundTouch
// pitch-preserving time-stretch + SDL playback.
