#include "display_mgr.h"
#if defined(DISPLAY_ST7796S)
#include "st7796s.h"
#elif defined(DISPLAY_ST7789)
#include "st7789.h"
#else
#include "ili9341.h"
#endif
#include "canvas.h"
#include "pin_config.h"
#include "psram_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DISPLAY";

#if defined(DISPLAY_ST7796S)
static ST7796S display;
#elif defined(DISPLAY_ST7789)
static ST7789 display;
#else
static ILI9341 display;
#endif

// Double buffer in PSRAM (or single buffer fallback)
static uint8_t *frame_buf_a;
static uint8_t *frame_buf_b;
static uint8_t *back_buffer;
static uint8_t *front_buffer;
static bool s_double_buffered;

esp_err_t display_mgr_init(void)
{
    s_double_buffered = false;

    if (dspmini_has_psram()) {
        frame_buf_a = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_SPIRAM);
        frame_buf_b = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_SPIRAM);

        if (frame_buf_a && frame_buf_b) {
            s_double_buffered = true;
        } else {
            if (frame_buf_a) { free(frame_buf_a); frame_buf_a = NULL; }
            if (frame_buf_b) { free(frame_buf_b); frame_buf_b = NULL; }
        }
    }

    if (!s_double_buffered) {
        // Single-buffer fallback: one buffer from default heap (~150KB)
        frame_buf_a = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_DEFAULT);
        if (!frame_buf_a) {
            ESP_LOGE(TAG, "Failed to allocate display buffer");
            return ESP_ERR_NO_MEM;
        }
        frame_buf_b = NULL;
    }

    memset(frame_buf_a, 0, CANVAS_BUF_SIZE);
    if (s_double_buffered) {
        memset(frame_buf_b, 0, CANVAS_BUF_SIZE);
        back_buffer = frame_buf_a;
        front_buffer = frame_buf_b;
        ESP_LOGI(TAG, "Display: double-buffered (2x %d bytes PSRAM)", CANVAS_BUF_SIZE);
    } else {
        back_buffer = frame_buf_a;
        front_buffer = frame_buf_a;
        ESP_LOGI(TAG, "Display: single-buffered (%d bytes internal)", CANVAS_BUF_SIZE);
    }

#if defined(DISPLAY_ST7796S)
    esp_err_t ret = st7796s_init(&display, DISP_SPI_HOST, DISP_SPI_FREQ_HZ,
                                 PIN_DISP_MOSI, PIN_DISP_CLK, PIN_DISP_CS,
                                 PIN_DISP_DC, PIN_DISP_RST, PIN_DISP_BL);
#elif defined(DISPLAY_ST7789)
    esp_err_t ret = st7789_init(&display, DISP_SPI_HOST, DISP_SPI_FREQ_HZ,
                                 PIN_DISP_MOSI, PIN_DISP_CLK, PIN_DISP_CS,
                                 PIN_DISP_DC, PIN_DISP_RST, PIN_DISP_BL);
#else
    esp_err_t ret = ili9341_init(&display, DISP_SPI_HOST, DISP_SPI_FREQ_HZ,
                                 PIN_DISP_MOSI, PIN_DISP_CLK, PIN_DISP_CS,
                                 PIN_DISP_DC, PIN_DISP_RST, PIN_DISP_BL);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed");
        return ret;
    }

#if defined(DISPLAY_ST7796S)
    ESP_LOGI(TAG, "Display manager initialized (ST7796S 320x240 window on 480x320)");
#elif defined(DISPLAY_ST7789)
    ESP_LOGI(TAG, "Display manager initialized (ST7789 280x240 landscape)");
#else
    ESP_LOGI(TAG, "Display manager initialized (ILI9341 320x240 landscape)");
#endif
    return ESP_OK;
}

void display_mgr_push_frame(const uint8_t *framebuffer)
{
    if (s_double_buffered) {
        memcpy(back_buffer, framebuffer, CANVAS_BUF_SIZE);

        uint8_t *tmp = front_buffer;
        front_buffer = back_buffer;
        back_buffer = tmp;

#if defined(DISPLAY_ST7796S)
        st7796s_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#elif defined(DISPLAY_ST7789)
        st7789_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#else
        ili9341_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#endif
    } else {
        // Single-buffer path: copy directly and send (mild tearing possible)
        memcpy(front_buffer, framebuffer, CANVAS_BUF_SIZE);
#if defined(DISPLAY_ST7796S)
        st7796s_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#elif defined(DISPLAY_ST7789)
        st7789_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#else
        ili9341_send_framebuffer(&display, front_buffer, CANVAS_BUF_SIZE);
#endif
    }
}

void display_mgr_set_brightness(uint8_t brightness)
{
#if defined(DISPLAY_ST7796S)
    st7796s_set_brightness(&display, brightness);
#elif defined(DISPLAY_ST7789)
    st7789_set_brightness(&display, brightness);
#else
    ili9341_set_brightness(&display, brightness);
#endif
}

void display_mgr_on(void)
{
#if defined(DISPLAY_ST7796S)
    st7796s_display_on(&display);
#elif defined(DISPLAY_ST7789)
    st7789_display_on(&display);
#else
    ili9341_display_on(&display);
#endif
}

void display_mgr_off(void)
{
#if defined(DISPLAY_ST7796S)
    st7796s_display_off(&display);
#elif defined(DISPLAY_ST7789)
    st7789_display_off(&display);
#else
    ili9341_display_off(&display);
#endif
}
