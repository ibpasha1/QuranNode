# QuranNode mainboard — KiCad + JLCPCB design notes

Workflow: **KiCad** schematic/layout, **JLCPCB** for fab + SMT of the fine-pitch
ICs/passives/mic (80 placements), **hand-solder** the module, connectors, switches,
and jack (11 parts). See `netlist.md` for the schematic and `BOM.csv` for parts.

## Assembly split (why)
- **JLCPCB SMT:** everything hard to hand-solder — TSSOP/QFN/SON ICs, 0402/0603
  passives, and the **INMP441** (bottom-ported LGA, *must* be reflowed).
- **Hand:** ESP32-S3 module (castellated — solderable by hand), USB-C, microSD,
  headphone jack, JST connectors, power/nav switches. `SW1/SW2` (SMD reset/boot
  tactiles) are marked hand but are trivial for JLCPCB if you'd rather.

## Parts / library
- Passives + common parts (`DW01A`, `FS8205A`, `USBLC6`, `2N7002`, LEDs) are
  typically **JLCPCB Basic** (no per-part loading fee). Most ICs (`PCM5102A`,
  `PAM8302A`, `TPS63020`, `INMP441`) are **Extended** (~one-time reel/loading fee each).
- **Every LCSC `Cxxxxx` in `BOM.csv` is a starting guess — confirm live stock** in
  JLCPCB's parts search before ordering; substitute equivalents if out of stock.
- Use the **KiCad → JLCPCB workflow**: assign an `LCSC` field to each SMT symbol, then
  export with the *JLCPCB fabrication toolkit* plugin (BOM + CPL/pick-and-place in
  JLC's format). Hand parts: leave `LCSC` blank / mark DNP-for-SMT so they're excluded
  from the placement file.

## KiCad symbols & footprints
- **ESP32-S3-WROOM-1:** use the official `RF_Module:ESP32-S3-WROOM-1` symbol +
  footprint (has the castellated pads + thermal pad).
- **ICs:** stock KiCad symbols exist for generic TSSOP-20 / SOT-23-6 / SON; for
  `PCM5102A`, `TPS63020`, `PAM8302A`, `INMP441` pull symbols from the **SnapEDA/
  Ultra Librarian** or the JLCPCB/EasyEDA library (import to KiCad) so the pinout is
  correct — don't hand-map from memory.
- **INMP441 footprint:** must include the **bottom acoustic port opening** + solder-mask
  keepout; add a plated/unplated hole through the board under the port.
- **USB-C 16P (TYPE-C-31-M-12):** use the matching KiCad footprint; tie the two CC
  pins to separate 5.1 k Rd (don't short them).
- **microSD / JST-PH / TRS jack / switches:** grab footprints from the specific MPN's
  datasheet-matched library part.

## Stackup & fab (JLCPCB)
- **4-layer**, 1.6 mm, `Sig / GND / PWR / Sig`. Board ~**60 × 145 mm** to fit the tray
  (`hardware/enclosure`). ENIG finish (better for the fine-pitch + solderability).
- Order form: 4-layer, 1 oz outer, ENIG, and **enable SMT assembly** (top side; add
  bottom if you place there). Impedance control not needed (no high-speed except USB
  FS + SPI — keep those short instead).

## Layout guidelines (in priority order)
1. **Power in-out first:** USB-C → TP4056 → cell → TPS63020 → +3V3, kept on one edge.
   TPS: tight loop VIN caps ↔ inductor ↔ VOUT caps; solid GND under it; FB divider node
   away from the inductor's switching node (short FB trace, no coupling).
2. **Decoupling** 0.1 µF caps hard against each IC supply pin; 10/22 µF bulk nearby.
3. **Audio:** keep `OUTL/OUTR` analog runs short; DAC AGND/DGND join at one point;
   PAM8302 output + speaker leads short (class-D EMI). Route audio away from the
   switching regulator and its inductor.
4. **USB D+/D−:** short, tight-coupled pair through the USBLC6 placed *at the connector*.
5. **ESP32 antenna keep-out:** the WROOM-1 antenna end must overhang a board edge with
   **no copper/ground under the antenna** (follow the module's keep-out drawing).
6. **microSD / SPI display:** group near the MCU; series-term not needed at these
   lengths; keep `DISP_CLK`/`SD_CLK` short.
7. **Enclosure alignment:** the mic acoustic hole must land under the lid grille
   (`MIC_CX/CY` in enclosure `params.py`); connectors on the correct edges — **USB-C +
   headphone on the BOTTOM edge, microSD on the TOP edge** (per the Rev F tray).

## Open items to resolve before layout
- **Touch INT/RST:** no GPIO is allocated (`netlist.md`) — either free two ESP32 GPIOs
  or tie T_RST to `DISP_RST` and poll without an interrupt.
- **TP4056 TEMP:** confirm the disable divider vs. a real 10 k NTC for your cell.
- **Cell protection (U5/U6):** keep only if the LP603449 lacks a built-in PCM.
- **Display connector `J3`:** finalize header vs FPC + exact pinout from your module.
- **Board outline:** import the enclosure's inner cavity (64 × 151 mm minus standoffs)
  as the KiCad `Edge.Cuts`, then place connectors to the tray's port positions.

## Suggested next step
Draw the schematic sheet-by-sheet in this order — **Power → MCU → Audio → Display →
SD → USB/Nav** — from `netlist.md`, assign LCSC #s as you go, run ERC, then start
placement against the enclosure outline.
