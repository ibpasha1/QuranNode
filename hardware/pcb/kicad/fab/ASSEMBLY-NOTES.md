# QuranNode mainboard — FAB + ASSEMBLY (respun 2026-09-24)

Board is **routed and clean: 0 unconnected, 0 electrical DRC** (remaining 12 DRC are
silk-over-copper/edge — cosmetic, JLCPCB auto-clips). Regenerate everything from the
scripts in `../` (see `../HANDOFF.md` §2 for the pipeline).

| File | Upload to JLCPCB as |
|------|---------------------|
| `qurannode_gerbers.zip` | Gerber (4-layer, 1.6 mm, **ENIG**) — bare-board fab |
| `qurannode-cpl.csv` | CPL / pick-and-place — **73 SMT parts, all Top** |
| `qurannode-bom-jlc.csv` | BOM — **29 lines, every part a verified in-stock LCSC #** |

CPL frame matches the gerbers (X = board X, Y = −board Y). Still eyeball pin-1 on
polarised parts in JLCPCB's preview before confirming.

## This respin fixed real bugs (all datasheet-verified)
| Ref | Was | Fix |
|-----|-----|-----|
| U2 PCM5102A | pinout entered **reversed** (19/20 pins wrong) | corrected to SLAS859C; LCSC C107671 |
| U3 PAM8302A | VDD/GND + VO± swapped, pin2(NC) tied GND; SOIC-8 land | corrected pinout; **MSOP-8** land (C113367) |
| U5 TPS63020 | 12-pad DFN land (real part is 14-pad VSON, 0.5 mm) | **custom `TPS63020_DSJ_VSON14` footprint**; C15483 |
| U6 USBLC6 | D+/D− **cross-wired** across the two TVS lines | rewired 1&6=D+, 3&4=D−; both Type-C sides tied |
| MK1 mic | INMP441 (out of stock) | **ICS-43434** (C5656610, in stock); new land |
| L1 inductor | 4×4 land, part is 4.4×4.2 (only 0.3 mm overlap) | **custom `MWSA0402S` footprint**; C408334 |
| U7/U8 protect | DW01A+FS8205 mis-wired + redundant | **removed** (LP603449 has its own PCM); cell− → GND |
| SD_MISO pull-up | placed at power centroid, net unroutable | pinned near J2 in `build_board.py` |

## Hand-populate (excluded from the SMT CPL/BOM — buy + solder separately)
`U1` ESP32 · `J1` USB-C · `J2` µSD · `J3` LCD header · `J4/J5` JST-PH · `J6` 3.5 mm jack ·
`SW1` 5-way nav · `SW2/SW3` RST/BOOT (E-Switch **TL3342** 6 mm land — hand-fit a
TL3342-compatible; the in-stock TS-1187A is 5.1 mm and does not fit).

## Off-board (device cost, not the PCBA quote)
3.5" ST7796S cap-touch module · 28 mm speaker · EEMB LP603449 cell (must ship with its
protection PCB, since the on-board protection was removed) · enclosure.

## Still worth a glance before ordering
- **560 k (C227139)** ~8 k stock — fine for one build, re-check for volume.
- **PCM5102A (C107671)** ~1.6 k stock — buy early.
- The two custom footprints (`TPS63020_DSJ_VSON14`, `MWSA0402S`) are datasheet-derived;
  cross-check against JLCPCB's part preview when you place the order.
