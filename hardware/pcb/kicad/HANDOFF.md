# QuranNode mainboard — HANDOFF (finish the PCB)

You're picking up a **custom KiCad PCB** for the QuranNode, built by a headless
SKiDL→pcbnew→FreeRouting pipeline. The board is **electrically clean (0 shorts/
clearance/mask-bridge DRC faults) and ~94% autorouted**. Your job: **finish the last
~19 traces**, then it's fab-ready. Everything is reproducible from scripts here.

---

## 0. TL;DR status
- `qurannode.kicad_pcb` — 4-layer, 62.9 × 150 mm, ~88 parts, GND(In1)/3V3(In2) planes
  + F/B GND pours. **Opens in KiCad. 0 electrical DRC faults. 19 nets unrouted.**
- Remaining DRC (117 total) is **all non-blocking**: ~25 footprint-inherent (edge
  connectors, ESP32 thermal-pad drills, USB-C/mic mounting holes — JLCPCB fabs fine)
  + ~92 cosmetic (silk-over-pad which JLC auto-clips, tight courtyards).
- **Finish line = route 19 traces** (KiCad interactive router, ~15 min) → DRC clean → fab.

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

# (f) DRC
"$CLI" pcb drc --output drc.json --format json --severity-all qurannode.kicad_pcb
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

1. **Route the last ~19 traces.** Open `qurannode.kicad_pcb` in KiCad → interactive
   router. Or try more FreeRouting passes (`-mp 30`) — it plateaus around here headless.
   The board is dense but has room; the planes handle GND/3V3.
2. **Confirm DRC has 0 electrical faults** after routing (shorts/clearance/mask-bridge).
   Ignore/accept the footprint-inherent + cosmetic ones (see §0), or add DRC exclusions.
3. **Silk cleanup (optional):** refs are on F.Fab (no silk). If you want silk refs,
   re-place them off copper in the GUI.
4. **Export for fab:** JLCPCB fab plugin → Gerbers + BOM + CPL. Mark hand-soldered parts
   (USB-C J1, microSD J2, JSTs J4/J5, jack J6, ESP32 module U1) as DNP-for-SMT.

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
