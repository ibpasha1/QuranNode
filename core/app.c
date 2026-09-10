#include "app.h"
#include "scene.h"
#include "player.h"
#include "progress.h"
#include "khatm.h"
#include "hifz.h"
#include "prefs.h"
#include "theme.h"
#include "tween.h"
#include "plat.h"
#include "hal.h"

// --- Idle backlight manager --------------------------------------------------
// The backlight (~110mA at full brightness) is the device's single biggest draw
// and it burns the whole time the device sits untouched. Reading is a mostly
// static page and playback follows along by ear, so we dim after a short idle
// and blank the backlight after a longer one. Any input — or active playback —
// restores it. g_prefs.brightness stays the source of truth; the manager only
// overrides the panel duty and never mutates the saved preference.
#define BL_DIM_MS    75000u    // idle -> dim to a faint, still-readable level
#define BL_OFF_MS   300000u    // idle -> backlight off entirely

static uint32_t s_idle_ms;
static uint8_t  s_bl_state;    // 0 = full (user setting), 1 = dimmed, 2 = off

static void backlight_wake(void)
{
    if (s_bl_state != 0) {          // only touch the HAL when actually restoring
        hal_set_brightness(g_prefs.brightness);
        s_bl_state = 0;
    }
    s_idle_ms = 0;
}

static void backlight_service(uint32_t dt_ms)
{
    // Keep lit while actively engaged: the main player, OR audio playing from
    // another path (Recite read-along, Library media), OR the mic capturing
    // (your turn in Recite). A silently-read static page still dims — that's
    // indistinguishable from having walked away.
    if (g_player.playing || hal_audio_active() || hal_mic_active()) {
        backlight_wake();
        return;
    }
    s_idle_ms += dt_ms;
    if (s_bl_state < 2 && s_idle_ms >= BL_OFF_MS) {
        hal_set_brightness(0);
        s_bl_state = 2;
    } else if (s_bl_state < 1 && s_idle_ms >= BL_DIM_MS) {
        uint8_t dim = g_prefs.brightness / 4;   // ~25% of the user's setting...
        if (dim < 8) dim = 8;                    // ...but never pitch black
        hal_set_brightness(dim);
        s_bl_state = 1;
    }
}

void app_init(void)
{
    tween_init();
    progress_init();                        // durable resume point + bookmarks
    khatm_init();                           // reading coverage + daily pace
    hifz_init();                            // memorization: portions + review schedule
    player_init(&g_player, "abdulbasit");   // the shared recitation transport
    prefs_init();                           // speed/font/brightness/tajweed (applies them)
    scene_init();   // starts on the calm Home screen
}

void app_tick(uint32_t dt_ms)
{
    tween_update_all((int)dt_ms);
    khatm_service();     // day rollover + throttled coverage saves
    hifz_service();      // day rollover + debounced memorization saves
    scene_tick(dt_ms);   // e.g. the reader advances its audio playhead + highlight
    backlight_service(dt_ms);   // dim/blank the panel when idle to save the battery
}

void app_input(InputEvent ev)
{
    backlight_wake();             // any input restores full brightness first
    theme_keybar_note(ev.type);   // lights the matching key chip in the keybar
    scene_handle_input(ev);
}

void app_render(Canvas *c)
{
    scene_render(c);
}
