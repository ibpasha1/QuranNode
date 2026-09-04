#include "st7796s.h"
#include "canvas.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ST7796S";

// PORTRAIT: native panel is 320x480 and we drive it upright (no rotation).
#define ST7796S_PANEL_W  320
#define ST7796S_PANEL_H  480

// The canvas IS the panel resolution (320x480), so source == output: the
// "upscale" below is a 1:1 identity copy (kept generic in case the canvas and
// panel ever differ again).
#define ST7796S_SRC_W    CANVAS_WIDTH   // 320
#define ST7796S_SRC_H    CANVAS_HEIGHT  // 480
#define ST7796S_OUT_W    ST7796S_PANEL_W  // 320
#define ST7796S_OUT_H    ST7796S_PANEL_H  // 480

#define SPI_MAX_CHUNK  32768 // Max bytes per SPI DMA transfer

// Nearest-neighbor source-column lookup for each output column, precomputed once.
static uint16_t s_colmap[ST7796S_OUT_W];

// LEDC backlight configuration
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_FREQ     5000
#define BL_LEDC_RES      LEDC_TIMER_8_BIT

// Scale/bounce band buffer in internal SRAM. We upscale into it one horizontal
// band at a time and DMA from it. Internal SRAM also keeps DMA off the PSRAM
// framebuffer — see the long comment in st7789.c: on the ESP32-S3 flash/PSRAM
// share the SPI0/1 controller, so a display DMA reading PSRAM during an NVS flash
// write can stall permanently. A contiguous 32KB DMA block is often unavailable,
// so the band is sized down until the allocation succeeds (one 480px row is only
// 960 bytes, so even a few rows work fine).
static uint8_t *s_dma_bounce = NULL;
static int s_band_rows = 0;

static esp_err_t st7796s_cmd(ST7796S *dev, uint8_t cmd)
{
    gpio_set_level(dev->pin_dc, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    return spi_device_polling_transmit(dev->spi, &t);
}

static esp_err_t st7796s_data(ST7796S *dev, const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    gpio_set_level(dev->pin_dc, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(dev->spi, &t);
}

static esp_err_t st7796s_cmd_data(ST7796S *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t ret = st7796s_cmd(dev, cmd);
    if (ret != ESP_OK) return ret;
    return st7796s_data(dev, data, len);
}

static void st7796s_backlight_init(ST7796S *dev)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = BL_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = dev->pin_bl,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_conf);
}

esp_err_t st7796s_init(ST7796S *dev, spi_host_device_t host, int freq_hz,
                       int pin_mosi, int pin_clk, int pin_cs,
                       int pin_dc, int pin_rst, int pin_bl)
{
    dev->pin_dc = pin_dc;
    dev->pin_rst = pin_rst;
    dev->pin_cs = pin_cs;
    dev->pin_bl = pin_bl;
    dev->width = ST7796S_OUT_W;
    dev->height = ST7796S_OUT_H;
    dev->brightness = 80;

    // Precompute the nearest-neighbor column map (output col -> source col).
    for (int ox = 0; ox < ST7796S_OUT_W; ox++) {
        s_colmap[ox] = (uint16_t)((ox * ST7796S_SRC_W) / ST7796S_OUT_W);
    }

    // Configure GPIO
    gpio_set_direction(pin_dc, GPIO_MODE_OUTPUT);
    gpio_set_direction(pin_rst, GPIO_MODE_OUTPUT);

    // SPI bus config
    spi_bus_config_t buscfg = {
        .mosi_io_num = pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_CHUNK,
    };

    esp_err_t ret = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = freq_hz,
        .mode = 0,
        .spics_io_num = pin_cs,
        .queue_size = 2,
    };

    ret = spi_bus_add_device(host, &devcfg, &dev->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Allocate the largest scale band that fits in contiguous internal DMA SRAM,
    // shrinking until it succeeds. Cap at the SPI DMA chunk limit.
    static const int kBandTry[] = {34, 24, 16, 8, 4, 2, 1};
    for (unsigned i = 0; i < sizeof(kBandTry) / sizeof(kBandTry[0]); i++) {
        int rows = kBandTry[i];
        size_t sz = (size_t)rows * ST7796S_OUT_W * 2;
        if (sz > SPI_MAX_CHUNK) continue;
        s_dma_bounce = heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_dma_bounce) { s_band_rows = rows; break; }
    }
    if (s_dma_bounce) {
        ESP_LOGI(TAG, "scale band buffer: %d rows (%d bytes) internal SRAM",
                 s_band_rows, s_band_rows * ST7796S_OUT_W * 2);
    } else {
        ESP_LOGE(TAG, "scale band buffer alloc failed — display will be blank");
    }

    // Hardware reset
    gpio_set_level(pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Software reset
    st7796s_cmd(dev, 0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    // Sleep out
    st7796s_cmd(dev, 0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // --- ST7796S initialization sequence ---
    // Command Set Control: unlock the manufacturer "command 2" registers so the
    // power/gamma tuning below takes effect (0xC3 then 0x96 is the ST7796S key).
    st7796s_cmd_data(dev, 0xF0, (uint8_t[]){0xC3}, 1);
    st7796s_cmd_data(dev, 0xF0, (uint8_t[]){0x96}, 1);

    // Memory Access Control: PORTRAIT (320 wide x 480 tall), BGR byte order.
    // The 4 portrait candidates (BGR bit 0x08 always set):
    //   0x08 upright but mirrored   0x48 upright + correct  (MX)
    //   0x88 upside-down            0xC8 upside-down mirror  (MX|MY)
    // 0x48 = MX|BGR — upright and un-mirrored so Arabic reads right-to-left correctly.
    st7796s_cmd_data(dev, 0x36, (uint8_t[]){0x48}, 1);

    // Pixel format: 16-bit RGB565
    st7796s_cmd_data(dev, 0x3A, (uint8_t[]){0x55}, 1);

    // Display Inversion / Frame Rate / power (ST7796S recommended values)
    st7796s_cmd_data(dev, 0xB4, (uint8_t[]){0x01}, 1);            // 1-dot inversion
    st7796s_cmd_data(dev, 0xB6, (uint8_t[]){0x80, 0x02, 0x3B}, 3); // display function ctrl
    st7796s_cmd_data(dev, 0xE8, (uint8_t[]){0x40, 0x8A, 0x00, 0x00,
                                            0x29, 0x19, 0xA5, 0x33}, 8);
    st7796s_cmd_data(dev, 0xC1, (uint8_t[]){0x06}, 1);            // power control 2
    st7796s_cmd_data(dev, 0xC2, (uint8_t[]){0xA7}, 1);            // power control 3
    st7796s_cmd_data(dev, 0xC5, (uint8_t[]){0x18}, 1);            // VCOM control

    // Positive / negative gamma
    st7796s_cmd_data(dev, 0xE0, (uint8_t[]){0xF0, 0x09, 0x0B, 0x06, 0x04,
                                            0x15, 0x2F, 0x54, 0x42, 0x3C,
                                            0x17, 0x14, 0x18, 0x1B}, 14);
    st7796s_cmd_data(dev, 0xE1, (uint8_t[]){0xE0, 0x09, 0x0B, 0x06, 0x04,
                                            0x03, 0x2B, 0x43, 0x42, 0x3B,
                                            0x16, 0x14, 0x17, 0x1B}, 14);

    // Re-lock command 2 registers
    st7796s_cmd_data(dev, 0xF0, (uint8_t[]){0x3C}, 1);
    st7796s_cmd_data(dev, 0xF0, (uint8_t[]){0x69}, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Display inversion ON (3.5" ST7796S IPS panels need this for normal colors —
    // toggle to 0x20 if colors render inverted).
    st7796s_cmd(dev, 0x21);

    st7796s_cmd(dev, 0x13);  // normal display mode on
    st7796s_cmd(dev, 0x29);  // display ON
    vTaskDelay(pdMS_TO_TICKS(20));

    // Initialize backlight
    st7796s_backlight_init(dev);
    st7796s_set_brightness(dev, dev->brightness);

    ESP_LOGI(TAG, "ST7796S initialized (%dx%d src upscaled to %dx%d panel, RGB565)",
             ST7796S_SRC_W, ST7796S_SRC_H, dev->width, dev->height);
    return ESP_OK;
}

esp_err_t st7796s_send_framebuffer(ST7796S *dev, const uint8_t *buf, size_t len)
{
    (void)len;  // source size is fixed at SRC_W*SRC_H; output is the full panel

    // Address the whole panel — the upscaled image fills it edge to edge.
    st7796s_cmd_data(dev, 0x2A, (uint8_t[]){
        0x00, 0x00, (ST7796S_OUT_W - 1) >> 8, (ST7796S_OUT_W - 1) & 0xFF
    }, 4);
    st7796s_cmd_data(dev, 0x2B, (uint8_t[]){
        0x00, 0x00, (ST7796S_OUT_H - 1) >> 8, (ST7796S_OUT_H - 1) & 0xFF
    }, 4);
    st7796s_cmd(dev, 0x2C);  // memory write

    if (!s_dma_bounce || s_band_rows <= 0) {
        ESP_LOGE(TAG, "no scale buffer — cannot upscale");
        return ESP_ERR_NO_MEM;
    }

    // Build the output image one horizontal band at a time into the internal-SRAM
    // band buffer (also keeps DMA off the PSRAM framebuffer — see header), then
    // stream each band. Nearest-neighbor: output row oy samples source row
    // oy*SRC_H/OUT_H; each output column uses the precomputed s_colmap[].
    const uint16_t *src = (const uint16_t *)buf;   // 320x240 RGB565, panel byte order
    uint16_t *band = (uint16_t *)s_dma_bounce;
    const int band_rows = s_band_rows;

    gpio_set_level(dev->pin_dc, 1);
    for (int oy = 0; oy < ST7796S_OUT_H; oy += band_rows) {
        int rows = ST7796S_OUT_H - oy;
        if (rows > band_rows) rows = band_rows;

#if (ST7796S_SRC_W == ST7796S_OUT_W) && (ST7796S_SRC_H == ST7796S_OUT_H)
        // Canvas == panel (320x480): the band is a contiguous slice of the
        // framebuffer, so one bulk copy beats the per-pixel nearest-neighbor loop.
        memcpy(band, src + (size_t)oy * ST7796S_OUT_W,
               (size_t)rows * ST7796S_OUT_W * 2);
#else
        for (int r = 0; r < rows; r++) {
            int sy = ((oy + r) * ST7796S_SRC_H) / ST7796S_OUT_H;
            const uint16_t *srow = src + (size_t)sy * ST7796S_SRC_W;
            uint16_t *orow = band + (size_t)r * ST7796S_OUT_W;
            for (int ox = 0; ox < ST7796S_OUT_W; ox++) {
                orow[ox] = srow[s_colmap[ox]];
            }
        }
#endif

        spi_transaction_t t = {
            .length = (size_t)rows * ST7796S_OUT_W * 2 * 8,
            .tx_buffer = band,
        };
        // Interrupt-driven DMA: the task blocks on a semaphore during the transfer
        // instead of busy-waiting, so the CPU is free for audio and no manual
        // vTaskDelay is needed to feed the watchdog.
        esp_err_t ret = spi_device_transmit(dev->spi, &t);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

esp_err_t st7796s_set_brightness(ST7796S *dev, uint8_t brightness)
{
    dev->brightness = brightness;
    uint32_t duty = (brightness * 255) / 100;
    if (duty > 255) duty = 255;  // clamp to 8-bit LEDC max
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    return ESP_OK;
}

esp_err_t st7796s_display_on(ST7796S *dev)
{
    st7796s_set_brightness(dev, dev->brightness);
    return st7796s_cmd(dev, 0x29);
}

esp_err_t st7796s_display_off(ST7796S *dev)
{
    st7796s_set_brightness(dev, 0);
    return st7796s_cmd(dev, 0x28);
}
