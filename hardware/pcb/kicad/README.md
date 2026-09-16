# QuranNode mainboard — KiCad project

KiCad 8 project. Two things are pre-generated so you skip grunt work:
- **Board outline + placement** from the enclosure (single source of truth).
- **A connected netlist** (`qurannode.net`, via SKiDL) — import it to drop all
  footprints + a ratsnest onto the board, so you place & route instead of drawing
  the whole schematic first.

| File | What |
|------|------|
| `qurannode.kicad_pro` | KiCad project (open this) |
| `qurannode.kicad_pcb` | Board with **Edge.Cuts outline + placement labels** baked in |
| `qurannode.kicad_sch` | Schematic stub (draw here if you prefer schematic-first) |
| `qurannode.net` | **KiCad netlist** — 79 parts, 70 nets (Pcbnew → Import Netlist) |
| `gen_netlist.py` | SKiDL source for the netlist (edit here, re-run to regenerate) |
| `PINOUT-VERIFY.md` | ⚠️ **IC pin-number checklist — read before routing** |
| `qurannode-outline.dxf` | Board outline as DXF — reliable Edge.Cuts import path |
| `placement.md` | Connector/component X/Y in board coordinates |
| `gen_board_outline.py` | Regenerates the outline/DXF/placement from enclosure params |

Board: **62.8 × 150 mm**, 2 mm corner radius (inner cavity 64 × 151.2 − 0.6 mm/side).

## ⚠️ Read `PINOUT-VERIFY.md` first
The netlist's **connectivity is validated** (no dangling nets) and passive pin numbers
are correct, but **IC package pin numbers are best-effort** and must be checked against
each datasheet/symbol — a wrong number wires a net to the wrong pad. The checklist
tells you exactly what to confirm.

## Fast path: import the netlist
1. Open `qurannode.kicad_pcb` (already has the outline + placement guides). It looks
   like an empty board of the right size — correct; components come from step 2.
2. **Pcbnew → File → Import → Netlist…** → select `qurannode.net` →
   **"Load and Test Netlist"** (reports footprint errors) → **"Update PCB"**. KiCad
   places all 79 footprints (in a pile) with the full ratsnest.
   - Footprint errors on the first pass are normal: fix any string KiCad can't resolve
     to your installed libs / chosen MPN (strings are in `gen_netlist.py`).
3. Verify pinouts per `PINOUT-VERIFY.md`, then **place** parts to the `placement.md`
   positions and **route**.

## Regenerating the netlist (SKiDL)
```sh
python3 -m venv .venv && . .venv/bin/activate && pip install skidl
python3 gen_netlist.py     # -> qurannode.net
```
Edit connections/parts in `gen_netlist.py` (one place), re-run, re-import.
(`.venv/`, SKiDL `*_sklib.py`/`*.erc`/`*.log` are git-ignored.)

## ⚠️ Honest status
- KiCad was **not installed here to validate** these files. The `.kicad_pcb`/`.kicad_sch`
  are best-effort KiCad-8 format. **If either won't open**, use the DXF path below —
  the outline geometry is correct regardless of the s-expression wrapper.
- The schematic is **not** auto-wired. A hand-generated wired schematic can't reliably
  land wires on symbol pins, so I didn't fake one — capture it from `../netlist.md`.

## If the .kicad_pcb won't open (DXF fallback)
1. New KiCad project → open the empty PCB.
2. **File → Import → Graphics…** → `qurannode-outline.dxf` → place on **Edge.Cuts**,
   scale 1:1, mm. That gives you the exact board outline.
3. Use `placement.md` to drop connectors on the right edges/positions.

## Workflow from here
1. **Schematic** (`qurannode.kicad_sch`): draw sheets Power → MCU → Audio → Display →
   SD → Nav from `../netlist.md`. Pull IC symbols from a **verified library**
   (SnapEDA / the JLCPCB library), not by hand — the netlist connects by pin *name*.
2. **Footprints:** assign per `../BOM.csv`; match the exact MPN footprints for the
   hand-soldered connectors. INMP441 needs its **bottom acoustic-port** footprint.
3. **Annotate → ERC → Update PCB from Schematic** — pulls the netlist into
   `qurannode.kicad_pcb`, which already has the outline + placement guides.
4. **Place & route** per `../DESIGN.md` (regulator loop, audio isolation, USB pair,
   WROOM antenna keep-out, mic hole under the lid grille).
5. **Fab/assembly export:** JLCPCB fabrication toolkit plugin → Gerbers + BOM + CPL.
   Mark hand-soldered parts so they're excluded from the SMT placement file.

## Regenerating the outline
`python3 gen_board_outline.py` — re-derives the outline if the enclosure changes.
**This overwrites `qurannode.kicad_pcb`**, so once you've started real layout, don't
rerun it; instead re-import the refreshed `qurannode-outline.dxf` onto Edge.Cuts.
