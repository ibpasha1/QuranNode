# QuranNode mainboard — HANDOFF (routing DONE, board is fab-ready)

A **custom KiCad PCB** for the QuranNode, built by a headless
SKiDL→pcbnew→FreeRouting pipeline. **Routing is COMPLETE: 0 unconnected,
0 electrical DRC faults.** Gerbers + drill are exported in `fab/`. What's left
before ordering is the §6 verify checklist (IC pinouts, land patterns, enclosure
reconciliation). Everything is reproducible from scripts here.

---

## 0. TL;DR status (2026-09-16)
- `qurannode.kicad_pcb` — 4-layer, 62.9 × 150 mm, ~88 parts, GND(In1)/3V3(In2) planes
  + F/B GND pours. **0 unconnected. 0 shorts/clearance/crossing faults.**
- Remaining DRC (31 total) is **all non-blocking**: 12 drill-out-of-range (WROOM
  thermal-pad stitching drills, footprint-inherent), 5 hole-clearance (USB-C mounting
  NPTHs, INMP441 port hole — footprint-inherent), 2 copper-edge (J2 microSD shield
  pads near the card-slot edge; ≥0.3 mm, within JLC capability), 12 silk (auto-clipped).
- `fab/qurannode_gerbers.zip` — Gerbers (11 layers) + Excellon drill, kicad-cli export.
- What changed since the 19-unrouted state (all in scripts, reproducible):
  1. **J2 microSD + J6 jack were placed with pads OFF the board** and openings facing
     INTO it (clamp only bounded footprint centres). Both now rotated 180° with an
     extent-aware clamp → all copper on-board, card/plug openings face the edges.
  2. **Parts were being placed UNDER the ESP32 module** (occupancy grid used pad
     extents; the WROOM antenna end is padless). Occupancy is now pads ∪ courtyard
     → R27 escaped from under U1, all 22 courtyard overlaps gone, and the previously
     unroutable DISP_BL/BL_G/SD nets routed on the next FR pass.
  3. **In2 +3V3 plane fill tightened** (clearance 0.2, min width 0.15 — set in
     build_board) so the plane survives via fences instead of splitting into islands.
  4. **`heal_plane.py` — new final pipeline step (e2)**: closes the 3 gaps FR can't
     see (2 plane-island bridges on B.Cu + the INMP441 3V3 pad fanout).

---

## 1. ⚠️ Environment (reconstruct this FIRST — nothing works without it)

**KiCad is NOT installed** (the `brew --cask kicad` install needs a sudo password we
can't give headless). Instead we **mount the cached KiCad 10 DMG** and use its bundled
tools directly. On a fresh session:

```sh
# 1. Mount KiCad (if /Volumes/KiCad is gone, e.g. after reboot)
DMG=$(ls ~/Library/Caches/Homebrew/downloads/*kicad-unified*.dmg | tail -1)
hdiutil attach "$DMG" -nobrowse -readonly        # -> /Volumes/KiCad

# Canonical paths (export these):
KPY=/Volumes/KiCad/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
CLI=/Volumes/KiCad/KiCad/KiCad.app/Contents/MacOS/kicad-cli
FPDIR=/Volumes/KiCad/KiCad/KiCad.app/Contents/SharedSupport/footprints
```
- **`pcbnew` only imports from `$KPY`** (KiCad's bundled Python 3.9), NOT system python.
- **SKiDL** lives in a venv: `python3 -m venv .venv && . .venv/bin/activate && pip install skidl`
  (git-ignored; only needed to regenerate the netlist).
- **FreeRouting**: `tools/freerouting-1.9.0.jar` (needs Java 17; git-ignored — re-download
  from github.com/freerouting/freerouting/releases/tag/v1.9.0 if missing). v2.x needs Java 25.
- **Rendering**: `brew install librsvg` → `rsvg-convert`.
- All Bash that touches network/`/Volumes` needs `dangerouslyDisableSandbox: true`.

---

## 2. The pipeline (regenerate the whole board)

Run in order, **from this dir**. Each stage overwrites the next stage's input.

```sh
# (a) netlist  — SKiDL, needs .venv active
. .venv/bin/activate && python3 gen_netlist.py     # -> qurannode.net
deactivate

# (b) place + planes + DSN  — KiCad python
"$KPY" build_board.py        # -> qurannode.kicad_pcb (PLACED, unrouted) + qurannode.dsn

# (c) autoroute — FreeRouting (background; takes 1-3 min)
rm -f qurannode.ses fr.log
nohup java -jar tools/freerouting-1.9.0.jar -de qurannode.dsn -do qurannode.ses -mp 10 -mt 1 > fr.log 2>&1 &

# (d) *** WAIT until FR fully exits AND qurannode.ses exists *** (see gotcha #1)
#     poll: until ! pgrep -f freerouting-1.9.0 >/dev/null && [ -s qurannode.ses ]; do sleep 3; done

# (e) import routes + GND pours + fill  — KiCad python
"$KPY" finalize.py           # -> qurannode.kicad_pcb (ROUTED)

# (e2) close the 3 gaps FreeRouting can't see (plane islands + INMP441 fanout)
"$KPY" heal_plane.py         # coords match the COMMITTED qurannode.ses (now
                             # force-added to git — FR output can vary across
                             # versions, so skip (c) and reuse it when possible)

# (f) DRC — expect 0 unconnected, 0 electrical faults, 31 non-blocking
"$CLI" pcb drc --output drc.json --format json --severity-all qurannode.kicad_pcb

# (g) fab outputs
mkdir -p fab
"$CLI" pcb export gerbers --layers F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,F.Paste,B.Paste,Edge.Cuts -o fab/ qurannode.kicad_pcb
"$CLI" pcb export drill --format excellon --excellon-units mm --generate-map --map-format gerberx2 -o fab/ qurannode.kicad_pcb
```

---

## 3. File map
| File | Role |
|------|------|
| `gen_netlist.py` | SKiDL: every part + connection → `qurannode.net`. **Edit circuits here.** |
| `build_board.py` | pcbnew: parse netlist, enclosure-aligned + size-aware placement, In1/In2 planes, JLC design rules, refs→Fab, export DSN |
| `finalize.py` | pcbnew: import `.ses`, add F/B GND pours, solid zone fill, report unconnected |
| `gen_board_outline.py` | board outline + placement anchors from enclosure `params.py` (imported by build_board) |
| `qurannode.pretty/NAV_5WAY_SMD.kicad_mod` | custom SMD 5-way switch footprint `[VERIFY land pattern]` |
| `qurannode.kicad_pcb/.pro/.sch` | the KiCad project (open the `.pro`) |
| `../netlist.md` | human-readable schematic (all nets, incl. battery-sense + soft-latch) |
| `../BOM.csv`, `../DESIGN.md`, `PINOUT-VERIFY.md`, `placement.md` | parts, workflow, verify checklist |

---

## 4. Remaining work (priority order)

1. ~~Route the last traces~~ **DONE — 0 unconnected, 0 electrical DRC faults.**
2. **Work §6 verify checklist** (IC pinouts, land patterns, enclosure) — the only
   gate left before ordering.
3. **BOM + CPL for JLC SMT** (gerbers already in `fab/`): needs LCSC part numbers;
   use the JLCPCB fab plugin in the GUI, or hand-write CPL from `placement.md`.
   Mark hand-soldered parts (USB-C J1, microSD J2, JSTs J4/J5, jack J6, ESP32
   module U1) as DNP-for-SMT.
4. **Silk cleanup (optional):** refs are on F.Fab (no silk). If you want silk refs,
   re-place them off copper in the GUI.

If a re-route ever leaves new unconnected items, the debugging recipe that worked:
DRC json → for each pair, dump copper/holes within ~3 mm of both endpoints (all
layers — FR routes signals on In1/In2 too!) → bridge plane islands via same-net
vias on B.Cu/In2, fan out stranded pads with a stub+via NEXT to the pad (never
in-pad), keep 0.15 copper / 0.25 hole-to-copper (+margin), re-DRC. Update the
coordinates in `heal_plane.py`.

## 5. Firmware TODO (in `firmware/components/qn/drivers/pin_config.h`)
- `PIN_PWR_HOLD` (GPIO48): **drive HIGH at the very top of `app_main`** or the soft-latch
  drops power when the user releases the center button. Drive LOW to power off.
- `PIN_BAT_ADC` (GPIO3): read with **ADC1** oneshot + `adc_cali` (~11 dB atten), then
  `volts * BAT_ADC_DIVIDER` (2.0). ADC2 is unusable with Wi-Fi.

## 6. Verify before fab (see `PINOUT-VERIFY.md` for the full IC checklist)
- **IC pin numbers** are best-effort from memory — check each against the real symbol.
  🔴 highest risk: **PCM5102A**, the **DW01A/FS8205A** protection topology.
- **`NAV_5WAY_SMD` land pattern** — built to a 9.9 mm measurement; match your real part.
- **LCD header Y** (`HDR_OFFSET=49.4` in build_board) — the header-to-glass distance is
  estimated; measure your module's pin-1-to-glass to align the screen to the lid window.
- **Enclosure not reconciled:** the LCD is now **board-mounted** (was lid-mounted in
  `hardware/enclosure`). Screen rabbet + port edges + removed power switch need rework.
  Also new since the J2/J6 placement fix: the microSD opening sits at the board top
  edge (card travels over the top strip; slot in the case wall must line up), and the
  jack J6 moved up to y≈144.4 with its barrel overhanging the bottom edge by ~4 mm —
  check barrel length vs wall thickness so the plug seats.

## 7. Gotchas / pitfalls (these bit us)
1. **RACE: never run `finalize.py` while FreeRouting is still running** or before
   `qurannode.ses` exists — you'll import 0 tracks and clobber the board to unrouted.
   Always confirm `! pgrep -f freerouting-1.9.0` AND `[ -s qurannode.ses ]` first.
2. **Don't rebuild between route and finalize** — `build_board.py` overwrites the
   placed board and re-exports the DSN; the existing `.ses` then won't match.
3. **Placement is deterministic** (no RNG), so a rebuild reproduces identical positions
   — that's why a `.ses` from one build imports cleanly into another.
4. **No fanout via-in-pads** — an earlier attempt to via-stitch every GND/3V3 pad to the
   planes bridged neighbor pads and created 31 shorts. GND SMD pads connect via the F/B
   pours; don't re-add via-in-pad fanout.
5. **`half_ext` uses pad EDGES** (pos ± size/2), not centers — needed so big-pad parts
   (the inductor) don't overlap. Don't revert to centers.

## 8. Design recap
ESP32-S3-WROOM-1 · PCM5102A DAC → 3.5 mm jack + PAM8302A amp → speaker · INMP441 mic ·
ST7796S 3.5" LCD on a **horizontal 14-pin header** (module solders on portrait,
enclosure-aligned) · microSD · TP4056 charger + TPS63020 buck-boost · USB-C.
**Battery sense** on GPIO3. **Soft-latch power**: center D-pad turns on (P-FET + Schottky),
ESP32 holds via GPIO48, software shutdown; mechanical switch removed.
Pin map: `firmware/.../pin_config.h`. Schematic: `../netlist.md`.
