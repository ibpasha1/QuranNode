#pragma once
// =============================================================================
// QuranNode custom prototype — pin map
// ESP32-S3-N16R8 + ST7796S(SPI)+capacitive touch(I2C) + PCM5102(I2S out) +
// INMP441(I2S in) + microSD(SPI) + PAM8302 speaker amp + 5-way switch.
// See hardware/prototype/PINOUT.md. Reserved: 35/36/37(PSRAM), 19/20(USB),
// 43/44(console), 0/45/46/3(strap), 48(RGB LED).
// =============================================================================

// --- Display: ST7796S 480x320, SPI2 ---
#define PIN_DISP_CS         10
#define PIN_DISP_MOSI       11
#define PIN_DISP_CLK        12
#define PIN_DISP_DC         13
#define PIN_DISP_RST        14
#define PIN_DISP_BL         15   // backlight (PWM/LEDC)
#define DISP_SPI_HOST       SPI2_HOST
// 80 MHz doubles the frame push vs 40; if you see sparkle/tearing/corruption on
// jumper leads, drop to 60 or back to 40 MHz.
#define DISP_SPI_FREQ_HZ    (80 * 1000 * 1000)

// --- Capacitive touch (I2C) — FT6236 @0x38 / GT911 @0x5D ---
#define PIN_TOUCH_SDA       16
#define PIN_TOUCH_SCL       17
#define TOUCH_I2C_PORT      I2C_NUM_0
#define TOUCH_I2C_FREQ_HZ   400000
// (INT freed for the speaker-amp enable; touch is polled)

// --- I2S OUT: PCM5102 DAC (line/headphone). SCK->GND, XSMT->3V3 on the board ---
#define PIN_I2S_BCK         4
#define PIN_I2S_WS          5    // LRCK
#define PIN_I2S_DATA        6    // DIN
#define SYNTH_SAMPLE_RATE   44100

// --- Speaker amp (PAM8302) enable. HIGH = speaker on, LOW = muted (headphones
//     still play off the DAC's line-out jack). Replaces the MAX98357 SD pin. ---
#define PIN_AMP_EN          18

// --- I2S IN: INMP441 MEMS mic (I2S_NUM_1). L/R->GND ---
#define PIN_MIC_SCK         7
#define PIN_MIC_WS          8
#define PIN_MIC_SD          9

// --- microSD, SPI3 ---
#define PIN_SD_CS           1
#define PIN_SD_MOSI         2
#define PIN_SD_CLK          42
#define PIN_SD_MISO         41
#define SD_SPI_HOST         SPI3_HOST

// --- 5-way navigation switch (active-low, internal pull-ups; COM->GND) ---
#define PIN_SW_UP           40
#define PIN_SW_DOWN         39
#define PIN_SW_LEFT         38
#define PIN_SW_RIGHT        47
#define PIN_SW_MID          21   // center press
