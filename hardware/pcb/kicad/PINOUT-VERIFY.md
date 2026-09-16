# ⚠️ Verify IC pin numbers before routing/fab

`gen_netlist.py` → `qurannode.net` has **correct connectivity** (validated: 79 parts,
70 nets, no dangling nets) and **correct passive pin numbers**. But the netlist wires
nets to package **pad numbers**, and the IC pad→function maps below are **best-effort
from memory** — a wrong number silently connects a net to the wrong pad.

**Do this:** when you place each IC's real symbol (from SnapEDA / the JLCPCB library),
compare its pad→function to the table below. Where they differ, fix the pin `num=` in
`gen_netlist.py` and re-run, **or** just fix the connection in the schematic/PCB.

Confidence: 🟢 fairly sure · 🟡 check carefully · 🔴 low — verify every pin.

## 🟢 ESP32-S3-WROOM-1  (standard Espressif module numbering)
1 GND · 2 3V3 · 3 EN · 4 IO4 · 5 IO5 · 6 IO6 · 7 IO7 · 8 IO15 · 9 IO16 · 10 IO17 ·
11 IO18 · 12 IO8 · 13 IO19 · 14 IO20 · 15 IO3 · 16 GND · 17 IO46 · 18 IO9 · 19 IO10 ·
20 IO11 · 21 IO12 · 22 IO13 · 23 IO14 · 24 IO21 · 25 IO47 · 26 IO48 · 27 IO45 · 28 IO0 ·
29 IO35 · 30 IO36 · 31 IO37 · 32 IO38 · 33 IO39 · 34 IO40 · 35 IO41 · 36 IO42 ·
37 RXD0(IO44) · 38 TXD0(IO43) · 39 IO2 · 40 IO1 · 41 GND(thermal pad).
USB uses IO19=D− / IO20=D+ (fixed).

## 🟢 TP4056 (ESOP-8)
1 TEMP · 2 PROG · 3 GND · 4 VCC · 5 BAT · 6 STDBY · 7 CHRG · 8 CE · 9 EP(GND).

## 🔴 PCM5102A (TSSOP-20) — VERIFY EVERY PIN
Used: 1 FLT · 2 DEMP · 3 XSMT · 4 FMT · 5 DGND · 6 DVDD · 7 CPVDD · 8 CAPP · 9 CAPM ·
10 VNEG · 11 AGND · 12 OUTL · 13 AVDD · 14 OUTR · 15 LRCK · 16 BCK · 17 DIN · 18 SCK ·
19 GND · 20 LDOO. **Cross-check against the TI datasheet — this map is uncertain.**

## 🟡 PAM8302A (SOIC-8)
Used: 1 SD · 2 GND · 3 IN+ · 4 IN− · 5 VO− · 6 PGND · 7 VDD · 8 VO+. Verify VO±/VDD/PGND.

## 🟡 TPS63020 (VSON-12)
Used: 1 VINA · 2 EN · 3 PS/SYNC · 4 GND · 5 FB · 6 PG · 7-8 VOUT · 9 L2 · 10 L1 ·
11-12 VIN · 13 GND/EP. Verify VOUT/VIN/L1/L2 grouping.

## 🟡 USBLC6-2SC6 (SOT-23-6)
Used: 1 I/O1 · 2 GND · 3 I/O2 · 4 I/O3 · 5 VBUS · 6 I/O4. D+/D− pass 1↔6 / 3↔4;
confirm which I/O pair is the "connector side" vs "chip side".

## 🟡 INMP441 (LGA-6)
Used: 1 SCK · 2 SD · 3 VDD · 4 GND · 5 L/R · 6 WS. Verify against the module land pattern.

## 🔴 DW01A + FS8205A (cell protection — OPTIONAL)
The `B−` / CS / gate topology is drawn approximately. If you keep this block, follow a
known-good DW01A+8205 reference exactly, or **omit it** (most LP603449 cells ship with
a protection PCB — then delete both parts and tie the cell `−` straight to GND).

## Connectors / switches (footprint pad names)
The netlist uses generic pad labels (USB-C A1/A4/…, microSD 1-8, display header 1-14,
JST 1-2, jack 1-5, nav 1-6). **Match them to the exact footprint you choose** — pad
numbering varies by MPN (especially the 3.5 mm jack and the 5-way nav switch).
