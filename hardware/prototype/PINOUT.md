# QuranNode custom prototype — wiring / pinout

**MCU:** ESP32‑S3‑N16R8 devkit (the YD‑ESP32‑S3 style board with CH343P USB‑serial + native USB).
16 MB flash, 8 MB **octal** PSRAM.

**Peripherals**
| # | Module | Bus | Notes |
|---|--------|-----|-------|
| 1 | ST7796S 3.5" 320×480 TFT + **capacitive** touch | SPI (display) + I²C (touch) | display write‑only over SPI; touch = I²C |
| 2 | PCM5102A I²S DAC (line/​headphone out) | I²S #0 (out) | replaces the MAX98357A amp — this is line‑level |
| 3 | INMP441 MEMS microphone | I²S #1 (in) | for recitation practice / recording (future) |
| 4 | microSD card | SPI (own bus) | FAT |
| 5 | 5‑way navigation switch | 5× GPIO | joystick only; SET/RST buttons unused |

---

## Master pin map

| ESP32‑S3 GPIO | Function | Goes to |
|:---:|---|---|
| **10** | DISP_CS   | TFT `CS` |
| **11** | DISP_MOSI | TFT `SDA`/`MOSI` |
| **12** | DISP_SCK  | TFT `SCL`/`SCK` |
| **13** | DISP_DC   | TFT `DC`/`RS` |
| **14** | DISP_RST  | TFT `RST` (share with touch RST) |
| **15** | DISP_BL   | TFT `BL`/`LED` (PWM backlight) |
| **16** | TOUCH_SDA | Touch `SDA` |
| **17** | TOUCH_SCL | Touch `SCL` |
| **18** | TOUCH_INT | Touch `INT` (optional; poll if unused) |
| **4**  | DAC_BCK   | PCM5102 `BCK` |
| **5**  | DAC_LRCK  | PCM5102 `LCK`/`LRCK` |
| **6**  | DAC_DIN   | PCM5102 `DIN` |
| **7**  | MIC_SCK   | INMP441 `SCK` (bit clock) |
| **8**  | MIC_WS    | INMP441 `WS` (word select) |
| **9**  | MIC_SD    | INMP441 `SD` (data out) |
| **1**  | SD_CS     | microSD `CS` |
| **2**  | SD_MOSI   | microSD `MOSI`/`DI` |
| **42** | SD_SCK    | microSD `SCK`/`CLK` |
| **41** | SD_MISO   | microSD `MISO`/`DO` |
| **40** | SW_UP     | 5‑way `UP` |
| **39** | SW_DOWN   | 5‑way `DWN` |
| **38** | SW_LEFT   | 5‑way `LFT` |
| **47** | SW_RIGHT  | 5‑way `RHT` |
| **21** | SW_MID    | 5‑way `MID` (center press) |

Power/ground rails are shared (see below). That's **24 signal GPIOs** — every free pin on
the board is used, so there's no spare header (see "reserved pins").

---

## Physical header layout (as silk‑screened)

```
        ┌──────────────────────── TOP ROW ────────────────────────┐
 GND  43   44   1    2    42   41   40   39   38   37 36 35  0  45  48   47   21   20  19  GND
 (—) (TX) (RX) SDcs SDmo SDsck SDmiso UP  DOWN LEFT  ✗  ✗  ✗ (bt)(sp)(RGB) RIGHT MID (USB)(USB)
                                                └ PSRAM ┘

        ┌─────────────────────── BOTTOM ROW ──────────────────────┐
 3V3  3V3  RST  4    5    6    7    15   16   17   18   8    3  46   9    10   11   12   13   14  5V  GND
                DAC  DAC  DAC  MIC  DISP TCH  TCH  TCH  MIC (sp)(sp)MIC  DISP DISP DISP DISP DISP
                BCK  LRCK DIN  SCK  BL   SDA  SCL  INT  WS         SD   CS   MOSI SCK  DC   RST
```
Legend: `✗` PSRAM (do not use) · `(bt)` BOOT strap · `(sp)` strapping pin · `(RGB)` on‑board LED ·
`(USB)` native USB · `(TX/RX)` console UART.

---

## Per‑module wiring

### 1) ST7796S display + capacitive touch
Display (SPI): `CS→10  MOSI→11  SCK→12  DC→13  RST→14  BL→15`, `VCC→3V3`, `GND→GND`.
Touch (I²C, FT6236/GT911): `SDA→16  SCL→17  INT→18  RST→14 (shared) or 3V3`, `VCC→3V3`, `GND→GND`.
> The display's MISO/SDO is **not** connected (we only write to it). Touch is a separate I²C bus.

### 2) PCM5102A DAC  — line/headphone out
`BCK→4  LRCK(LCK)→5  DIN→6`, `VIN→5V`, `GND→GND`.
**Jumpers on the DAC board (important):**
- `SCK → GND` — grounds the master‑clock input so the PCM5102 uses its **internal PLL** (the ESP32 sends no MCLK).
- `XSMT → 3V3` — releases soft‑mute (otherwise the output stays silent).
- `FMT → GND`, `FLT/DEMP/H*L` — leave at defaults (I²S, standard).
> VIN from **5V** gives the on‑board 3.3 V LDO headroom for cleaner audio; its I²S pins are 3.3 V logic regardless. Audio comes out the DAC's own 3.5 mm jack.

### 3) INMP441 microphone (I²S input)
`SCK→7  WS→8  SD→9`, `VDD→3V3` (⚠ **not** 5 V), `GND→GND`, `L/R→GND` (left channel).

### 4) microSD (own SPI bus)
`CS→1  MOSI→2  SCK→42  MISO→41`, `VCC→3V3`, `GND→GND`.

### 5) 5‑way switch
`UP→40  DWN→39  LFT→38  RHT→47  MID→21`, `COM→GND`. Uses the ESP32's internal pull‑ups
(pressed = LOW). Leave `SET`, `RST` unconnected.

---

## Power

- Feed the board **5 V** via USB (CH343P port for flashing+console) or the `5Vin` pin.
- `3V3` (either 3V3 pin) powers the display logic+backlight, INMP441, and microSD.
- `5V` powers the PCM5102 `VIN` (regulates to 3.3 internally).
- All grounds common.
- Rough 3V3 budget: ESP32 ~250 mA, display+backlight ~120 mA, SD ~80 mA peak, mic ~2 mA →
  ~450 mA, within the devkit LDO's ~800 mA. If the backlight browns things out, drive the
  panel's `VCC` from 5 V **only if your module has an on‑board 3.3 regulator** (many 3.5"
  ones do); otherwise keep it on 3V3.

---

## Reserved pins — do NOT wire

| Pins | Why |
|---|---|
| 35, 36, 37 (and 33, 34) | **Octal PSRAM** on N16R8 — using them crashes PSRAM |
| 19, 20 | Native **USB** D‑/D+ (keep for USB‑OTG / flashing) |
| 43, 44 | **UART0** console (TX/RX) — flashing + serial control |
| 0, 45, 46, 3 | **Strapping** pins (boot mode / VDD_SPI / JTAG) |
| 48 | On‑board **RGB LED** (WS2812) |

---

## Firmware changes from the current build

The display driver + pins are unchanged (already 10–15). The audio path changes from
MAX98357A → PCM5102, and SD/touch/mic/5‑way move. Update `firmware/components/qn/drivers/pin_config.h`:

```c
// Display (SPI2) — unchanged
#define PIN_DISP_CS 10, PIN_DISP_MOSI 11, PIN_DISP_CLK 12, PIN_DISP_DC 13, PIN_DISP_RST 14, PIN_DISP_BL 15
// I2S OUT -> PCM5102 (no amp-enable pin; drop PIN_SPK_EN / speaker-enable logic)
#define PIN_I2S_BCK 4, PIN_I2S_WS 5, PIN_I2S_DATA 6
// I2S IN  -> INMP441 (new: I2S_NUM_1)
#define PIN_MIC_SCK 7, PIN_MIC_WS 8, PIN_MIC_SD 9
// SD (SPI3)
#define PIN_SD_CS 1, PIN_SD_MOSI 2, PIN_SD_CLK 42, PIN_SD_MISO 41
// Touch (I2C)
#define PIN_TOUCH_SDA 16, PIN_TOUCH_SCL 17, PIN_TOUCH_INT 18
// 5-way (all active-low, internal pull-ups)
#define PIN_SW_UP 40, PIN_SW_DOWN 39, PIN_SW_LEFT 38, PIN_SW_RIGHT 47, PIN_SW_MID 21
```

Notes:
- **`audio_esp32.c`**: PCM5102 has no shutdown pin, so remove the `PIN_SPK_EN` gpio + `spk()`
  calls; I²S config already uses `mclk = I2S_GPIO_UNUSED` (SCK→GND), which is exactly right.
- **5‑way input**: replace the serial‑key stopgap with a real GPIO poller — now UP/DOWN/LEFT/
  RIGHT/center all exist, so navigation maps cleanly (center = select/play, hold = back).
- **Touch (FT6236) and mic (INMP441)** are new drivers — not in the firmware yet (touch → tap
  targets; mic → the "record & compare" recitation feature).
