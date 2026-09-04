// main.c — QuranNode desktop simulator harness.
//
// Owns timing and I/O; drives the portable core exactly the way the ESP32
// display task will: pump input -> app_input, advance time -> app_tick, render
// into the canvas -> hal_display_push. Nothing here is product logic.
#include "hal.h"
#include "app.h"
#include "canvas.h"
#include "plat.h"
#include <SDL2/SDL.h>
#include <stdio.h>

int main(void)
{
    if (!qn_hal_init()) return 1;

    Canvas canvas;
    if (!canvas_init(&canvas, CANVAS_WIDTH, CANVAS_HEIGHT)) {
        fprintf(stderr, "canvas_init failed\n");
        hal_shutdown();
        return 1;
    }

    app_init();

    // Test hook: QN_AUTOPLAY drives Home->Reader->Play at boot so the audio path
    // can be smoke-tested headlessly (SDL_AUDIODRIVER=dummy). Harmless otherwise.
    if (getenv("QN_AUTOPLAY")) {
        InputEvent sel = { INPUT_NAV_SELECT, ENC_NAV, true };
        InputEvent play = { INPUT_BTN_PLAY, ENC_NAV, true };
        app_input(sel);    // Home -> Reader
        app_input(play);   // start recitation
    }

    uint32_t last = plat_millis();
    const uint32_t frame_ms = 33;  // ~30 fps, matching the device display task

    while (hal_running()) {
        InputEvent ev;
        while (hal_input_poll(&ev)) app_input(ev);

        uint32_t now = plat_millis();
        uint32_t dt = now - last;
        last = now;
        app_tick(dt);

        canvas_clear(&canvas, 0);   // 0 == THEME_BG (near-black) via RGB565(0,0,0)
        app_render(&canvas);
        hal_display_push(canvas.buf);

        // crude frame pacing on top of vsync
        uint32_t spent = plat_millis() - now;
        if (spent < frame_ms) SDL_Delay(frame_ms - spent);
    }

    canvas_deinit(&canvas);
    hal_shutdown();
    return 0;
}
