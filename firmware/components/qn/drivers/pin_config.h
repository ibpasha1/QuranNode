#pragma once
// =============================================================================
// QuranNode custom prototype — pin map
// ESP32-S3-N16R8 + ST7796S(SPI)+capacitive touch(I2C) + PCM5102(I2S out) +
// INMP441(I2S in) + microSD(SPI) + PAM8302 speaker amp + 5-way switch.
// See hardware/prototype/PINOUT.md. Reserved: 35/36/37(PSRAM), 19/20(USB),
// 43/44(console), 0/45/46(strap), 48(RGB LED). GPIO3 = battery ADC (see below).
// =============================================================================

// --- Display: ST7796S 480x320, SPI2 ---
#define PIN_DISP_CS         10
#define PIN_DISP_MOSI       11
#define PIN_DISP_CLK        12
#define PIN_DISP_DC         13
#define PIN_DISP_RST        14
#define PIN_DISP_BL         15   // backlight (PWM/LEDC)
#define DISP_SPI_HOST       SPI2_HOST
// 40 MHz is the confirmed-stable rate. 80 MHz (the old value) caused moving
// colour-line corruption drawn OVER an otherwise-correct image — it's beyond the
// ST7796S write spec and marginal over dupont leads. 40 MHz cured it completely.
// Bumping toward 60 trades margin for a faster frame push; retest for sparkle if
// you do.
#define DISP_SPI_FREQ_HZ    (40 * 1000 * 1000)

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

// --- Headphone-insert detect: jack tip-switch -> GPIO46 (100k pull-up; 20k tip
//     pull-down). Plug inserted -> switch opens -> HIGH; no plug -> LOW. GPIO46 is a
//     strapping pin but ignored in normal boot (GPIO0 high) and only resistor-pulled. ---
#define PIN_HP_DETECT       46

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

// --- Battery voltage sense: VSYS -> 100k/100k divider -> GPIO3 + 100nF filter.
//     ADC reads VBAT/2 when powered; Vbat = raw_volts * BAT_ADC_DIVIDER. Use ADC1
//     (ADC2 conflicts with Wi-Fi). GPIO3 is a strapping pin — the ~2V from the
//     divider at boot is benign for the default JTAG-source strap. ---
#define PIN_BAT_ADC         3    // ADC1_CH2
#define BAT_ADC_DIVIDER     2.0f

// --- Soft-latch power hold. Press-and-hold the centre D-pad (PIN_SW_MID) turns on
//     Q_PWR (P-FET) via hardware; drive PIN_PWR_HOLD HIGH *as early as possible* in
//     boot to latch power on so the user can release the button. Drive it LOW to
//     power the device off in software. (Replaces the mechanical on/off switch.) ---
#define PIN_PWR_HOLD        48   // -> Q_HOLD gate; HIGH = stay on, LOW = shut down
