// dump_frame.c — headless renderer for verification / golden images.
//
// Links the real portable core (app + scenes + arabic_text + gfx) against a
// tiny no-window HAL, drives a scripted sequence of input events, then writes the
// final frame as a PPM (decoding the panel's byte-swapped RGB565 to RGB888). Lets
// us eyeball what the device would actually draw — no SDL window, CI-friendly.
//
//   quran-dump <out.ppm> [script]
//   script chars: >=ENC_CW  <=ENC_CCW  s=SELECT  p=PLAY  b=BACK  m=MENU
//
#include "hal.h"
#include "app.h"
#include "canvas.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

// --- minimal headless HAL ------------------------------------------------
#define SD_ROOT "sdcard"
static uint32_t s_clock = 0;
uint32_t plat_millis(void) { return s_clock; }

bool hal_init(void) { return true; }
void hal_shutdown(void) {}
bool hal_running(void) { return true; }
void hal_display_push(const uint16_t *fb) { (void)fb; }
void hal_set_brightness(uint8_t percent) { (void)percent; }
bool hal_input_poll(InputEvent *o) { (void)o; return false; }

bool hal_fs_slurp(const char *rel, uint8_t **out, size_t *len)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", SD_ROOT, rel);
    FILE *f = fopen(path, "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    uint8_t *b = malloc((size_t)sz);
    size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    if (got != (size_t)sz) { free(b); return false; }
    *out = b; *len = got; return true;
}
bool hal_fs_exists(const char *rel)
{ char p[512]; snprintf(p, sizeof(p), "%s/%s", SD_ROOT, rel); struct stat st; return stat(p,&st)==0; }
int hal_fs_list(const char *rel, char names[][64], int max, bool dirs_only)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", SD_ROOT, rel);
    DIR *dir = opendir(path); if (!dir) return 0;
    int n = 0; struct dirent *e;
    while (n < max && (e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        char full[600]; snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (dirs_only && !S_ISDIR(st.st_mode)) continue;
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(dir); return n;
}

bool hal_state_save(const char *name, const void *data, size_t len)
{
    mkdir(SD_ROOT "/state", 0755);
    char p[512]; snprintf(p, sizeof(p), "%s/state/%s", SD_ROOT, name);
    FILE *f = fopen(p, "wb"); if (!f) return false;
    size_t w = fwrite(data, 1, len, f); fclose(f); return w == len;
}
bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len)
{
    char p[512]; snprintf(p, sizeof(p), "%s/state/%s", SD_ROOT, name);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    size_t n = fread(buf, 1, cap, f); fclose(f); if (out_len) *out_len = n; return true;
}

// Virtual audio clip: no decoding, but a simulated playhead that advances with
// app_tick while "playing" — enough to verify word-sync highlight headlessly.
struct HalAudioClip { int _u; };
static struct HalAudioClip s_clip;
static bool s_aud_playing = false;
static double s_aud_pos = 0;      // ms
static float s_aud_rate = 1.0f;
static const uint32_t S_AUD_LEN = 100000;  // long enough to avoid auto-advance

HalAudioClip *hal_audio_open(const char *r){ (void)r; s_aud_pos = 0; return &s_clip; }
void hal_audio_close(HalAudioClip *c){ (void)c; s_aud_playing = false; }
void hal_audio_play(HalAudioClip *c){ (void)c; s_aud_playing = true; }
void hal_audio_pause(HalAudioClip *c){ (void)c; s_aud_playing = false; }
bool hal_audio_is_playing(HalAudioClip *c){ (void)c; return s_aud_playing; }
uint32_t hal_audio_pos_ms(HalAudioClip *c){ (void)c; return (uint32_t)s_aud_pos; }
uint32_t hal_audio_len_ms(HalAudioClip *c){ (void)c; return S_AUD_LEN; }
void hal_audio_seek_ms(HalAudioClip *c, uint32_t m){ (void)c; s_aud_pos = m; }
void hal_audio_set_rate(HalAudioClip *c, float r){ (void)c; s_aud_rate = r; }
void hal_audio_set_volume(float v){ (void)v; }
void hal_audio_set_output(int speaker){ (void)speaker; }
static bool s_dump_ota;
void hal_ota_start(void){ s_dump_ota = true; }
const char *hal_ota_url(void){ return s_dump_ota ? "http://192.168.86.20/" : NULL; }
void hal_ota_pull(void){ s_dump_ota = true; }
const char *hal_ota_status(void){ return s_dump_ota ? "Downloading update..." : NULL; }
bool hal_recovery_requested(void){ return false; }
// Called by the driver between ticks to advance the virtual playhead.
static void aud_advance(uint32_t dt_ms){ if (s_aud_playing) s_aud_pos += (double)dt_ms * s_aud_rate; }

// --- driver --------------------------------------------------------------
static InputEvent ev(InputEventType t){ InputEvent e = { t, ENC_NAV, true }; return e; }

static void write_ppm(const char *path, const uint16_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", CANVAS_WIDTH, CANVAS_HEIGHT);
    for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT; i++) {
        uint16_t v = fb[i];
        uint32_t r5 = (v >> 3) & 0x1F, b5 = (v >> 8) & 0x1F;
        uint32_t g6 = ((v & 0x7) << 3) | ((v >> 13) & 0x7);
        unsigned char rgb[3] = {
            (unsigned char)((r5 << 3) | (r5 >> 2)),
            (unsigned char)((g6 << 2) | (g6 >> 4)),
            (unsigned char)((b5 << 3) | (b5 >> 2)),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "frame.ppm";
    const char *script = argc > 2 ? argv[2] : "";
    uint32_t advance_ms = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;

    Canvas c;
    canvas_init(&c, CANVAS_WIDTH, CANVAS_HEIGHT);
    app_init();

    for (const char *p = script; *p; p++) {
        switch (*p) {
        case '>': app_input(ev(INPUT_ENC_CW));    break;
        case '<': app_input(ev(INPUT_ENC_CCW));   break;
        case 's': app_input(ev(INPUT_NAV_SELECT));break;
        case 'p': app_input(ev(INPUT_BTN_PLAY));  break;
        case 'b': app_input(ev(INPUT_BTN_BACK));  break;
        case 'm': app_input(ev(INPUT_BTN_MENU));  break;
        case 'o': app_input(ev(INPUT_BTN_MODE));  break;
        case 'v': app_input(ev(INPUT_NAV_DOWN));  break;
        case 'u': app_input(ev(INPUT_NAV_UP));    break;
        case 'k': app_input(ev(INPUT_BTN_BOOKMARK)); break;
        case 'l': app_input(ev(INPUT_NAV_LEFT));  break;
        case 'h': app_input(ev(INPUT_NAV_RIGHT)); break;
        }
    }

    // Simulate playback time in 33ms steps so the virtual playhead + active-word
    // highlight advance exactly as they would on-device.
    uint32_t elapsed = 0;
    do {
        uint32_t dt = 33;
        s_clock += dt;
        aud_advance(dt);
        app_tick(dt);
        elapsed += dt;
    } while (elapsed < advance_ms);

    canvas_clear(&c, 0);
    app_render(&c);
    write_ppm(out, c.buf);
    canvas_deinit(&c);
    printf("wrote %s (script='%s' advance=%ums pos=%ums)\n",
           out, script, advance_ms, hal_audio_pos_ms(&s_clip));
    return 0;
}
