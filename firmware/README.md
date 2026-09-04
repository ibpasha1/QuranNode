# QuranNode firmware (ESP32-S3)

Runs the **same portable core** as the desktop simulator on real hardware — only
the HAL underneath differs (`hal/esp32/` instead of `hal/sim/`). Target rig: the
DSP-Mini **DevKitC-1 + 3.5" ST7796S** (the `devkit_st7796s` setup), reusing
DSP-Mini's `pin_config.h`. Rendered **native 480×320**.

## Build & flash

```sh
cd firmware
pio run -e devkit_st7796s                 # build
pio run -e devkit_st7796s -t upload       # flash (board on USB)
pio device monitor                        # serial log @115200
```

## Wiring (DSP-Mini pin map — `components/qn/drivers/pin_config.h`)

| Function | Pins |
|---|---|
| Display SPI2 (ST7796S) | MOSI 11 · SCLK 12 · CS 10 · DC 13 · RST 14 · BL 15 |
| microSD SPI3 | MOSI 38 · SCLK 39 · MISO 48 · CS 40 |
| 5-way switch (active-low) | UP 3 · LEFT 5 · RIGHT 6 · CENTER 2 (DOWN n/a — GPIO4 is mic) |
| I2S audio (MAX98357A) | BCLK 8 · LRCLK 9 · DIN 18 · SD/EN 7 *(Phase 2)* |
| Encoder (MCP23017 @0x20, I2C 16/17) | *optional, Phase 2* |

Display SPI runs at 40 MHz; if you see tearing/noise on long jumper wires, drop
`DISP_SPI_FREQ_HZ` to 30 MHz in `pin_config.h`.

## Bundled Al-Fatihah (no SD card needed)

For on-bench testing, Surah Al-Fatihah is **embedded in flash** — the glyph pack,
word timings, and all 7 ayah MP3s (via `board_build.embed_files` in
`platformio.ini`). `hal_fs_slurp` serves these from flash and falls back to the SD
card for everything else. So the reader renders Arabic and audio plays with **no
card inserted**. To go SD-only later, drop the `board_build.embed_files` lines and
the `EMBEDDED[]` table in `hal/esp32/hal_esp32.c`.

## SD card layout (for content beyond Al-Fatihah)

Copy the repo's `sdcard/` contents to the card root:

```
/sdcard/packs/reader_lg.qgp          Arabic glyph pack (from tools/shape_quran.py)
/sdcard/quran/timings/1.qtm          word timings (from tools/fetch_sample.py)
/sdcard/audio/abdulbasit/1/*.mp3     recitation (Phase 2)
/sdcard/audio/library/**             library tracks (Phase 2)
/sdcard/state/                       created at runtime (resume, bookmarks)
```

## Controls

### Serial monitor (no buttons needed)

When the physical buttons/encoder aren't wired, drive the device from
`pio device monitor` — type these keys (same mapping as the simulator):

| Key | Action |
|---|---|
| arrows / W A S D | navigate |
| Enter | select |
| Space | play / pause |
| `,` or `q` / `.` or `e` | encoder CCW / CW (scroll, speed) |
| `r` | encoder press |
| Backspace / `b` | back |
| Tab | mode (open Ayah Loop from reader) |
| `k` | bookmark · `m` | menu · `l` | Library (from Home) |

Input is read from the console **UART0** — the port `pio device monitor` connects
to (the DevKitC-1 "UART"/CP2102 port you also flash over). If you monitor the other
"USB" port instead, keystrokes won't arrive.

### Physical 5-way switch (when wired)

| Input | Action |
|---|---|
| UP / RIGHT / LEFT | navigate |
| CENTER (short) | select / play-pause |
| CENTER (long-press) | back |

An encoder (turn = scroll/speed, press = select) comes in Phase 2. If you wire one
to the MCP23017 @0x20 it will self-detect.

## Status

- **Phase 1 (this build):** display + 5-way input + SD (reader/nav/library/loop UI
  on the real panel, reading glyph packs + saved state from SD). **No audio yet.**
- **Phase 2 (M2-3):** MP3→I2S recitation playback with word-sync highlight.

## How it maps to the simulator

`main/main.c` is the hardware twin of `sim/main.c`: `hal_init()` →
`canvas_init()` → `app_init()` → loop `{ hal_input_poll→app_input; app_tick;
app_render→hal_display_push }`. Every scene, the Arabic renderer, the player, and
the loop engine are the exact same `core/` sources compiled for xtensa.
