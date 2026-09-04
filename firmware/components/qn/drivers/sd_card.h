#pragma once

#include "esp_err.h"
#include <stdbool.h>

// Mount FAT filesystem from microSD card at "/sdcard".
// Returns ESP_OK on success, ESP_ERR_NOT_FOUND if no card inserted.
esp_err_t sd_card_mount(void);

// Unmount SD card and free SPI bus.
void sd_card_unmount(void);

// Check if SD card is currently mounted.
bool sd_card_is_mounted(void);
