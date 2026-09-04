// main.c — QuranNode ESP32-S3 firmware entry point.
//
// Mirrors the desktop simulator's harness (sim/main.c): bring up the HAL, make
// the 480x320 canvas, init the portable core, then run the render/input loop that
// drives app_input / app_tick / app_render. The core and every scene are shared
// verbatim with the simulator — only the HAL underneath differs.
//
// The loop runs in its OWN task with a generous stack (not app_main's default
// 8KB) because it does Arabic alpha-blitting AND on-demand MP3 decode (minimp3)
// inline — both stack-hungry.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "hal.h"
#include "app.h"
#include "canvas.h"
#include "plat.h"
#include "ota.h"
#include "theme.h"
#include "font.h"

static const char *TAG = "QURANNODE";

// Bump this string to prove an OTA landed: the new value appears in the boot log
// after the device reboots into the freshly-flashed slot.
#ifndef FW_VERSION
#define FW_VERSION "dev6-github"
#endif

#define QN_TASK_STACK 40960   // Arabic render + minimp3 decode headroom

static void qn_task(void *arg)
{
    (void)arg;
    static Canvas canvas;
    if (!canvas_init(&canvas, CANVAS_WIDTH, CANVAS_HEIGHT)) {
        ESP_LOGE(TAG, "canvas alloc failed (need ~300KB PSRAM)");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "canvas ready (%dx%d); app_init...", CANVAS_WIDTH, CANVAS_HEIGHT);

    // Recovery: 5-way center held at power-on → Wi-Fi update mode, bypassing the
    // whole app. Lets a sealed unit be reflashed even if a bad update broke the UI.
    if (hal_recovery_requested()) {
        ESP_LOGW(TAG, "RECOVERY: center held at boot — Wi-Fi update mode + GitHub pull");
        hal_ota_pull();   // auto-fetch latest release; local push URL also live
        while (1) {
            theme_clear(&canvas);
            theme_header(&canvas, "Recovery Mode", THEME_TITLE, NULL, THEME_DIM);
            const char *status = hal_ota_status();
            font_draw_string_centered(&canvas, 170, &font_small, status ? status : "Starting...", THEME_TITLE);
            const char *url = hal_ota_url();
            if (url) {
                font_draw_string_centered(&canvas, 250, &font_tiny, "or push from a PC to", THEME_DIM);
                font_draw_string_centered(&canvas, 272, &font_tiny, url, THEME_DIM);
            }
            hal_display_push(canvas.buf);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    app_init();
    ESP_LOGI(TAG, "entering render loop — free heap %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));

    uint32_t last = plat_millis();
    uint32_t frame = 0;
    int64_t s_render_us = 0, s_push_us = 0;
    const TickType_t period = pdMS_TO_TICKS(33);   // ~30 fps
    TickType_t wake = xTaskGetTickCount();

    while (1) {
        InputEvent ev;
        while (hal_input_poll(&ev)) {
            ESP_LOGI(TAG, "input event type=%d", (int)ev.type);   // temporary: 5-way debug
            app_input(ev);
        }

        uint32_t now = plat_millis();
        app_tick(now - last);
        last = now;

        int64_t t0 = esp_timer_get_time();
        canvas_clear(&canvas, 0);
        app_render(&canvas);
        int64_t t1 = esp_timer_get_time();
        hal_display_push(canvas.buf);
        int64_t t2 = esp_timer_get_time();
        s_render_us += (t1 - t0);
        s_push_us   += (t2 - t1);

        // Heartbeat every ~2 s: loop alive + where the per-frame time goes.
        if (++frame % 60 == 0) {
            ESP_LOGI(TAG, "alive: frame %u, heap %u KB | render=%lldms push=%lldms (avg/60)",
                     (unsigned)frame, (unsigned)(esp_get_free_heap_size() / 1024),
                     s_render_us / 60 / 1000, s_push_us / 60 / 1000);
            s_render_us = 0;
            s_push_us = 0;
        }

        vTaskDelayUntil(&wake, period);
    }
}

void app_main(void)
{
    // Reset reason: 1=POWERON 3=SW 4=PANIC 5/6/7=watchdog 9=BROWNOUT. If pressing
    // a button "reboots" the screen, this shows up as 9 (power) or 4 (crash).
    ESP_LOGW(TAG, "boot — reset reason = %d", (int)esp_reset_reason());
    ESP_LOGW(TAG, "QuranNode firmware version: %s", FW_VERSION);
    ESP_LOGI(TAG, "QuranNode firmware — bring-up");

    if (!qn_hal_init()) {
        ESP_LOGE(TAG, "HAL init failed; halting");
        return;
    }
    ota_mark_valid();   // booted + HAL up OK → confirm this OTA image, cancel rollback

    xTaskCreatePinnedToCore(qn_task, "qn_main", QN_TASK_STACK, NULL, 5, NULL, 1);
}
