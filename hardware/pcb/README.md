# QuranNode — mainboard BOM (for costing / quoting)

A first-pass **bill of materials for a single integrated PCB** that replaces the
current breakout-board prototype (ESP32-S3 module + PCM5102 board + TP4056 board +
INMP441 board + nav breakout) with one board. Intended to drop into a quoting tool
(JLCPCB/PCBWay assembly, Digi-Key/Mouser, etc.) for a real per-unit number.

> **Nothing here is verified against a schematic yet** — it's derived from the
> firmware pin map (`pin_config.h`) and the reference designs of each IC. Treat
> passive counts as estimates and **re-check every supplier part number for current
> stock/price** before you rely on a quote. Values marked `[TUNE]` / `VERIFY`
> depend on your final choices.

## Architecture (from the firmware)

Confirmed in `hal/esp32/audio_esp32.c` + `pin_config.h`:

```
                 ┌──────────────────────────────────────────┐
  USB-C ──VBUS──▶│ TP4056 charger ─▶ [cell protect] ─▶ BATT  │
                 │                                    │       │
                 │                       TPS63020 buck-boost ─┼─▶ 3V3 rail
                 └──────────────────────────────────────────┘
   ESP32-S3 ──I2S0──▶ PCM5102A DAC ──┬─▶ 3.5mm jack (headphone mode)
   (WROOM-1)                         └─▶ PAM8302A amp ─▶ speaker  (amp EN gated)
            ──I2S1◀── INMP441 mic
            ──SPI──▶ ST7796S display (+ I2C cap-touch, PWM backlight)
            ──SPI──▶ microSD
            ──GPIO◀─ 5-way nav (UP/DN/L/R/MID) · power switch · BOOT/RESET
```

Key point that drives the BOM: the speaker is **not** on a MAX98357 I²S amp (the
stale comment on line 1 of `audio_esp32.c` is wrong). It's the PCM5102A line-out
feeding a **PAM8302A** analog Class-D amp whose shutdown pin is `PIN_AMP_EN` — so
the BOM needs both the DAC and the amp.

## Pin map (ESP32-S3)

| Function | GPIO | | Function | GPIO |
|---|---|---|---|---|
| DISP CS/MOSI/CLK | 10/11/12 | | I2S BCK/WS/DATA | 4/5/6 |
| DISP DC/RST/BL | 13/14/15 | | AMP_EN (PAM8302 SD) | 18 |
| TOUCH SDA/SCL | 16/17 | | MIC SCK/WS/SD | 7/8/9 |
| SD CS/MOSI/CLK/MISO | 1/2/42/41 | | NAV U/D/L/R/MID | 40/39/38/47/21 |

## What's on the board vs. off the board

**On the PCB (in `BOM.csv`):** MCU, DAC, speaker amp, charger, buck-boost, USB-C,
microSD socket, headphone jack, mic, nav + power + boot/reset switches, all passives
and connectors.

**Off the board (add to device cost, not the PCBA quote):**

| Item | ~@50 | ~@10k |
|---|---|---|
| 3.5" ST7796S 480×320 cap-touch module | $6–10 | $4–6 |
| 28 mm speaker (4/8 Ω) | $1–2 | $0.5–1 |
| EEMB LP603449 1100 mAh cell (JST-PHR-02) | $3–5 | $2–3 |
| Enclosure (this repo's `hardware/enclosure`) | $5–15 print | $1–3 molded + tooling |

## Optional / cost-down levers

- **`U5`+`U6` cell protection (DW01A + FS8205A)** — *omit* if your LP603449 ships
  with a built-in protection PCB (most JST pouch cells do). Saves ~$0.30 + area.
- **Status LEDs `D1–D3`** — cosmetic; drop for cost.
- **Backlight FET `Q1`** — only if the display module's LED pin needs external
  current drive; many modules accept BL as a logic/PWM input.
- **`TPS63020` → cheap LDO/buck** — the buck-boost is ~$1.3–2.8 (one of the priciest
  lines). If you can live with the rail collapsing below ~3.4 V battery (i.e. accept
  less runtime at the low end), a plain 3.3 V buck or even an LDO is far cheaper.
- **ESP32-S3-WROOM-1 flash/PSRAM** — `N16R8` is generous; `N8R2` (8 MB flash / 2 MB
  PSRAM) is cheaper if the firmware + assets fit.

## Turning this into a real quote

1. Import `BOM.csv` into your quoting tool; resolve every `MPN` to an in-stock
   supplier part (the `Supplier PN` column is a *starting guess*).
2. Get **bare-PCB fab** priced separately: 4-layer, ~60 × 145 mm, 1.6 mm, ENIG.
3. Get **assembly** priced: ~120 placements, double-sided, ~30 unique parts — watch
   the per-unique-part loading fees at low volume, they dominate the batch-50 number.
4. Add the off-board table above for a finished-device cost.

Ballpark from these parts (see the chat estimate): **~$40–65/board assembled at 50**
(+~$150–400 one-time NRE), **~$16–26/board at ~10k**.
