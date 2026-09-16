# QuranNode mainboard — schematic / netlist

Everything you need to capture the schematic in KiCad: every block, pin, net, and
reference-design passive. Connections are given **by pin *name*** (the KiCad symbol
labels its pins by name/GPIO), so you wire symbol-pin-to-net without needing package
pin numbers. GPIOs match the firmware `pin_config.h`.

> Verify each IC's supply/charge-pump passives against its datasheet "typical
> application" figure — values below are the standard app-circuit values.

## Power tree

```
USB-C VBUS ─► TP4056 (VCC) ──charge──► VBAT ─► [cell protect]* ─► BATT (LP603449)
                                         │
                            Q_PWR (P-FET) ┴─► VSYS ─► TPS63020 ─► +3V3
                                                 └─► PAM8302 VDD (speaker amp)
```

**Nets:** `VBUS` (5 V in) · `VBAT` (cell +, always tied to charger) · `VSYS`
(battery after the soft-latch) · `+3V3` (main rail) · `GND`.

### Soft-latch power (no mechanical switch)
Press-and-hold the **centre D-pad** turns on; the ESP32 latches itself and can power
down in software. Charging still works when off (charger sits on `VBAT`, ahead of Q_PWR).

| Part | Connection |
|---|---|
| **Q_PWR** P-FET (AO3401A) | S=`VBAT`, D=`VSYS`, G=`PWR_G` |
| **R_G** 100 k | `VBAT` → `PWR_G` (holds Q_PWR OFF) |
| **D_BTN** Schottky (BAT54) | anode=`PWR_G`, cathode=`NAV_MID` — press pulls `PWR_G` low → ON |
| **Q_HOLD** N-FET (2N7002) | D=`PWR_G`, S=`GND`, G=`HOLD_G` |
| **R_HOLD** 10 k / **R_HOLD_PD** 100 k | `PWR_HOLD` →10k→ `HOLD_G` →100k→ `GND` |
| **PWR_HOLD** | ESP32 **GPIO48** — drive HIGH early in boot to stay on; LOW = power off |

`NAV_MID` still reads on **GPIO21** (the diode isolates the two roles), so firmware can
do long-press-to-shutdown. ⚠ No hardware force-off — rely on a firmware watchdog, or
swap the discrete latch for a push-button controller IC (TPS3705/MAX16150) if needed.

### TP4056 (U4) — Li-ion charger
| Pin | Net / part |
|---|---|
| VCC | `VBUS`; 10 µF → GND (C_in) |
| BAT | `VBAT`; 10 µF → GND |
| PROG | R_prog **1.5 kΩ** → GND  (I_chg ≈ 1200/Rprog ≈ **0.8 A**; use 1.2 k for ~1 A) |
| TEMP | mid-rail via divider **10 k VCC→TEMP + 10 k TEMP→GND** (disables NTC qual) [VERIFY] |
| CE | `VBUS` (enable) |
| CHRG | LED_red → 1 k → `VBUS` (optional status) |
| STDBY | LED_grn → 1 k → `VBUS` (optional) |
| GND | `GND` |

### Cell protection (U5 DW01A + U6 FS8205A) — *OPTIONAL*
Omit if the LP603449 ships with a protection PCB (most JST pouch cells do). Standard
topology if kept: `VBAT → DW01 VCC` (via 100 Ω + 100 nF→GND); DW01 `OD`/`OC` → FS8205
gates; FS8205 drain pair in series with the cell's `B−`; `CS` → DW01 through the pack.
Protected negative becomes the pack `P−`.

### TPS63020 (U7) — buck-boost 3.3 V
| Pin | Net / part |
|---|---|
| VIN ×2 | `VSYS`; 2× 10 µF → GND |
| VOUT ×2 | `+3V3`; 2× 22 µF → GND |
| L1 / L2 | inductor **1.5 µH** (Isat ≥ 3 A) between the two pins |
| FB | divider: `+3V3` —[**560 k**]— FB —[**100 k**]— `GND`  → 3.30 V |
| EN | `VIN` (or 100 k pull-up to VIN) — on whenever VSYS present |
| PS/SYNC | `GND` (power-save enabled; tie VIN for forced-PWM/lower ripple) |
| PG | 100 k → `+3V3` (optional; route to a spare GPIO to monitor) |
| GND / PGND / pad | `GND` |

### Battery voltage sense
`VSYS` —[**100 k**]— `VBAT_SENSE` —[**100 k**]— `GND`, plus **100 nF** `VBAT_SENSE`→GND;
`VBAT_SENSE` → **ESP32 GPIO3** (ADC1_CH2). Reads **VBAT/2** when powered (×2 in
firmware, `BAT_ADC_DIVIDER`). Tapped off `VSYS` so it only draws (~21 µA) when the
device is on. ADC1 required — ADC2 is unusable with Wi-Fi. GPIO3 is a strapping pin
(benign here). Firmware: `PIN_BAT_ADC` in `pin_config.h`.

## ESP32-S3-WROOM-1 (U1)

| Pin | Net |
|---|---|
| 3V3 | `+3V3`; **22 µF + 0.1 µF** → GND at the pin |
| GND + thermal pad | `GND` |
| EN | 10 k → `+3V3`, 1 µF → GND (RC), **SW1 (RESET)** → GND |
| IO0 | 10 k → `+3V3`, **SW2 (BOOT)** → GND |
| GPIO19 (USB D−) | `USB_DM` (→ USBLC6 → USB-C) |
| GPIO20 (USB D+) | `USB_DP` (→ USBLC6 → USB-C) |
| GPIO4 / 5 / 6 | `I2S_BCK` / `I2S_WS` / `I2S_DATA`  (→ DAC) |
| GPIO7 / 8 / 9 | `MIC_SCK` / `MIC_WS` / `MIC_SD`  (→ INMP441) |
| GPIO10/11/12/13/14/15 | `DISP_CS`/`DISP_MOSI`/`DISP_CLK`/`DISP_DC`/`DISP_RST`/`DISP_BL` |
| GPIO16 / 17 | `TOUCH_SDA` / `TOUCH_SCL` |
| GPIO1 / 2 / 42 / 41 | `SD_CS` / `SD_MOSI` / `SD_CLK` / `SD_MISO` |
| GPIO40/39/38/47/21 | `NAV_UP`/`NAV_DN`/`NAV_L`/`NAV_R`/`NAV_MID` |
| GPIO18 | `AMP_EN` (→ PAM8302 SD) |
| GPIO43 / 44 (TXD0/RXD0) | debug header/test-pads (optional) |

Strapping pins left at default: GPIO0 (boot, handled), GPIO3/45/46 unused. Flash +
PSRAM are internal to the module.

## Audio OUT — PCM5102A DAC (U2) → jack + PAM8302A amp (U3)

### PCM5102A (U2)
| Pin | Net / part |
|---|---|
| AVDD / DVDD / CPVDD | `+3V3`; **0.1 µF at each** + one **10 µF** bulk |
| LDOO | **1 µF** → GND |
| VNEG / charge-pump | per datasheet (typ **2.2 µF**) |
| SCK | `GND` (use internal PLL from BCK — no MCLK needed) |
| BCK / LRCK / DIN | `I2S_BCK` / `I2S_WS` / `I2S_DATA` |
| FMT | `GND` (I²S) |
| FLT / DEMP | `GND` |
| XSMT | `+3V3` via **10 k** (unmute) |
| OUTL / OUTR | → **100 Ω** series → **2.2 µF** DC-block → jack L/R; **also** → amp input |
| AGND / DGND | `GND` |

### PAM8302A (U3) — mono Class-D, speaker
| Pin | Net / part |
|---|---|
| VDD | `VSYS`; 0.1 µF + 10 µF → GND |
| SD | `AMP_EN`; **100 k → GND** (default OFF) |
| IN+ | `OUTL` + `OUTR` summed via **2× 20 k** → **0.47 µF** input cap → IN+ (gain-set) |
| IN− | ground-ref via matching 0.47 µF |
| VO+ / VO− | → speaker `J4` (optional **2× ferrite 600 Ω + 1 nF** for EMI) |
| GND / PGND | `GND` |

> Amp runs off `VSYS` (battery, up to 4.2 V) for loudness, gated by `AMP_EN`.
> Headphone path is DAC line-out only (amp off in headphone mode — firmware logic).

## Audio IN — INMP441 (MK1)
| Pin | Net |
|---|---|
| VDD | `+3V3`; 0.1 µF (+1 µF) → GND |
| GND | `GND` |
| SCK / WS / SD | `MIC_SCK` / `MIC_WS` / `MIC_SD` |
| L/R | `GND` (left slot — firmware uses `I2S_STD_SLOT_LEFT`) |
| CHIPEN | `VDD` |

Bottom-ported LGA → **reflow only (JLCPCB SMT)**; needs an acoustic hole in the PCB
under the port, aligned to the lid mic grille.

## Display — ST7796S SPI + cap-touch (J3 header)
| Module pin | Net |
|---|---|
| VCC / GND | `+3V3` / `GND` (0.1 µF + 10 µF) |
| CS / SCK / SDA(MOSI) | `DISP_CS` / `DISP_CLK` / `DISP_MOSI` |
| DC(RS) / RST | `DISP_DC` / `DISP_RST` |
| LED (backlight) | via FET **Q1 2N7002**: `DISP_BL` →100 Ω→ gate, 10 k gate→GND; drain→LED−; LED+→`+3V3` |
| T_SDA / T_SCL | `TOUCH_SDA` / `TOUCH_SCL`; **4.7 k pull-ups → +3V3** |
| T_INT / T_RST | ⚠ no GPIO assigned — tie T_RST to `DISP_RST` and poll (no INT), or free 2 GPIOs |
| SDO/MISO | NC (no display reads) |

## microSD (J2, SPI mode)
| Pin | Net |
|---|---|
| VDD | `+3V3`; 0.1 µF + 10 µF |
| CS | `SD_CS`; 10 k → +3V3 |
| DI (MOSI) | `SD_MOSI`; 10 k → +3V3 |
| DO (MISO) | `SD_MISO`; 10 k → +3V3 |
| CLK | `SD_CLK` |
| DAT1 / DAT2 | 10 k → +3V3 each (unused in SPI) |
| CD (card detect) | NC (or a spare GPIO) |
| GND / shield | `GND` |

## USB-C (J1) + ESD (U8 USBLC6-2SC6)
| Pin | Net |
|---|---|
| VBUS | `VBUS`; 0.1 µF → GND |
| GND / shield | `GND` (shield via 1 MΩ ∥ 4.7 nF optional) |
| CC1 / CC2 | **5.1 k → GND each** (Rd sink) |
| D+ / D− | → USBLC6 I/O pins → `USB_DP` / `USB_DM` |
| SBU1 / SBU2 | NC |
| USBLC6 VBUS pin | `VBUS`; GND pin `GND` |

## Nav switch (SW4, 5-way + center)
`NAV_UP/DN/L/R/MID` → 5 GPIOs; **common → GND**. Use ESP32 internal pull-ups
(configured as inputs). Optional: 5× 10 k → +3V3 and 5× 100 nF → GND (debounce).

## Decoupling summary (place at each IC's supply pin)
- 0.1 µF: every VDD/AVDD/DVDD/CPVDD/VCC pin (ESP32, DAC×3, PAM, SD, display, USB, mic, charger)
- 10 µF bulk: +3V3 near ESP32/DAC/SD/display; TP4056 VCC & BAT; TPS VIN ×2; PAM VDD; USB VBUS
- 22 µF: TPS VOUT ×2
- ESP32 3V3: 22 µF + 0.1 µF
