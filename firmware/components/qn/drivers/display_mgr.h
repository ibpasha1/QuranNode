#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t display_mgr_init(void);
void display_mgr_push_frame(const uint8_t *framebuffer);
void display_mgr_set_brightness(uint8_t brightness);
void display_mgr_on(void);
void display_mgr_off(void);
