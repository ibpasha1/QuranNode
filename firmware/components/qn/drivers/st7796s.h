#pragma once

#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

// ST7796S — 3.5" 320x480 SPI TFT controller.
// Driven in landscape (480x320). For first bring-up the existing 320x240 UI
// framebuffer is rendered centered in an 80,40-offset window on the panel;
// full 480x320 layout is a follow-up.
typedef struct {
    spi_device_handle_t spi;
    int pin_dc;
    int pin_rst;
    int pin_cs;
    int pin_bl;
    uint16_t width;   // framebuffer width  (window, not panel)
    uint16_t height;  // framebuffer height (window, not panel)
    uint8_t brightness;
} ST7796S;

esp_err_t st7796s_init(ST7796S *dev, spi_host_device_t host, int freq_hz,
                       int pin_mosi, int pin_clk, int pin_cs,
                       int pin_dc, int pin_rst, int pin_bl);
esp_err_t st7796s_send_framebuffer(ST7796S *dev, const uint8_t *buf, size_t len);
esp_err_t st7796s_set_brightness(ST7796S *dev, uint8_t brightness);
esp_err_t st7796s_display_on(ST7796S *dev);
esp_err_t st7796s_display_off(ST7796S *dev);
