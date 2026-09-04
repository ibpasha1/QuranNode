#pragma once
#include <stdbool.h>

// Over-the-air firmware update (ESP32 only). See ota_esp32.c.

// Bring up Wi-Fi (STA) + the HTTP upload server. Non-blocking: returns once
// started; the IP arrives asynchronously (poll ota_ip()). Safe to call twice.
bool ota_start(void);

// Device IP once Wi-Fi is up, else NULL. Show it on screen so the user knows
// where to point the browser.
const char *ota_ip(void);

// Cancel the post-OTA rollback after a successful boot. Call once at startup.
void ota_mark_valid(void);
