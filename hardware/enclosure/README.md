# QuranNode — handheld enclosure

A **slim, portrait, snap-fit two-part clamshell** for the ESP32-S3 Quran reader,
built around a 3.5" 480×320 ST7796S TFT. FreeCAD-native and 3D-printable.

Screen border dimensions were lifted from the DSP-Mini `lid.stl`
(`~/Documents/projects/DSP-Mini/Hardware/carrier/enclosure/experimental`), which
targets the identical module — see `params.py` `SCR_*`.

| File | What |
|------|------|
| `params.py` | **Single source of truth** — every dimension + the component layout |
| `freecad_export.py` | **Canonical builder** — FreeCAD B-rep → STEP + STL (run headless) |
| `gen_panelplan.py` | Fast Pillow FRONT+BACK layout preview (`panelplan.png`) |
| `render_iso.py` | Shaded z-buffer renders (`render_front.png`, `render_exploded.png`) |
| `qurannode_enclosure.FCStd` | Open/edit in FreeCAD (Tray + Lid) |
| `*.step` (`qurannode_enclosure`/`tray`/`lid`) | STEP for CAD/CAM |
| `tray.stl` / `lid.stl` | Meshes for slicing |

## Overall

- **Envelope:** 68.8 × 156.0 × **17.8 mm** (Rev F: **shorter** — the ~29 mm the Rev E
  middle battery zone cost is reclaimed by re-packing into a bit more thickness:
  battery rotated **landscape** into the chin, TPS regulator moved behind the screen).
- **Two printed parts:** `tray` (back — boards, battery, speaker) and `lid` (front
  — screen + D-pad). No visible fasteners; internal snap-fit. **Only the 5-way
  switch knob is exposed (round hole); SET/RST are covered.**

## Regenerate

```sh
cd hardware/enclosure
/Applications/FreeCAD.app/Contents/Resources/bin/freecadcmd freecad_export.py  # -> STEP + STL
python3 gen_panelplan.py     # -> panelplan.png  (fast layout check)
python3 render_iso.py        # -> render_front.png + render_exploded.png
```

Edit dimensions/layout **only in `params.py`**, then rerun all three.

## Layout (portrait, X=width, Y=height from bottom, Z=through-thickness)

**FRONT (lid):**
- **3.5" ST7796S** screen, **flush-mounted**: cover glass sits coplanar with the
  lid top via a stepped rabbet. Module PCB/glass **55.6 × 91** (MEASURED — PCB and
  glass are the same width), **window 54 × 88** (kept *under* the module width so the
  inner lip overlaps the glass ~0.8 mm/side and retains it — a 57 mm window was wider
  than the module and wouldn't hold it). Active lit area 49 × 74 sits inset inside
  the 55.6 glass, fully clear of the window. Centred high at Y=104.
- **D-pad** below the screen (MEASURED board **40.7 × 24.8 mm landscape**), centred at
  (32, 27). **Integrated into the lid**: the 5-way switch is a **9.9 mm square, 3.9 mm
  tall** body (~centred on the board), and the NAV opening is a matching **square
  10.4 mm** (`NAV_SW` + clearance) so the body noses **up through the lid** and the
  actuator protrudes — snug around the square body (no sloppy round gap). The PCB seats
  near the inner face, cavity intrusion **~2.7 mm** (`T_DPAD`). Board mounts **FLAT
  against the lid** (component side up), centred by the switch body in the opening and
  **taped** (no locating pegs). **SET/RST covered.**
- **Mic** — INMP441 (14 × 14 mm), earpiece-style 7-hole grille at the **far left**,
  just above the D-pad's top-left corner (centred at X=10, Y=48); board **taped**
  under the grille, port toward the front (no locating pegs). (Can't sit directly
  *beside* the D-pad — the 41 mm board leaves only ~13 mm of left margin, narrower
  than the mic board.)

**BACK (tray) — Rev F re-pack for a shorter shell:**
- **BEHIND SCREEN (top):** ESP32-S3 (28×63, vertical, left) · 28 mm **speaker**
  (back grille, **TOP-right, Y≈135** — right up under the top edge) · **TPS63020
  buck-boost** (26×18, right, Y≈90) · **microSD** (slot out the **top edge**, left of
  the speaker) · **power switch** (7×7×16, side-mounted, plunger out the **left edge**
  at Y=140, cradle ribs — glue to retain).
- **CHIN (bottom):** the **battery** rotated **landscape** (51 × 34.5, Y≈39) — its
  34.5 mm length (vs 51 mm portrait) is what shortens the shell — sits above a
  **bottom row of two boards side by side**: **PCM5102 DAC** (landscape 32 × 17,
  bottom-left, 3.5 mm jack out the **bottom edge** toward the left, **raised 1.5 mm**
  `PORT_HP zrel=4.7`) and the **TP4056 charger** (26 × 17, bottom-right, **USB-C out
  the bottom edge**). The bottom being off the floor / half-height leaves the whole
  top-right free for the raised speaker. ESP32 USB-C internal.

## Vertical stack (~17.8 mm)

The front/back split still keeps it slim, but Rev F leans on a bit more depth: the
screen module intrudes 4.3 mm from the lid and the boards/battery sit ~5–6 mm off
the tray floor, so behind-screen items stack in Z instead of spreading in length.

| Layer | mm |
|-------|----|
| Back skin (floor) | 1.4 |
| Interior cavity (boards / battery / speaker) | 15.0 |
| Front skin (lid) | 1.4 |

## Battery

Sized for the **EEMB LP603449** — 51 × 34.5 × **6.3 mm**, 1100 mAh, JST-PHR-02.
Rev F mounts it **landscape** (51 mm across the width, 34.5 mm of length) in the
chin above the DAC: the shorter 34.5 mm span is exactly what let the shell drop to
156 mm. At 6.3 mm it sits ~6 mm off the floor with the D-pad/mic on the lid in
front of it (5.5 + 6.3 < 15 mm cavity). **No corral** — tape it to the floor
anywhere there's room; watch the JST tail clears the DAC / screen-module edge.

> Capacity note: a thick 2000 mAh brick (e.g. EEMB LP103454, 10.6 mm) does **not**
> fit this envelope — 10.6 mm nearly fills the cavity, and nothing can then sit
> behind the screen. Fitting one forces ~17 mm thick and a wider (~69 mm) shell.
> The thin 1100 mAh cell was the right call for a slim reader; ~7–10 h active.

## Fastening — internal snap-fit

**Eleven** discrete nibs on the lid lip click into pockets in the tray wall —
**3 bottom · 2 top · 3 each side** — for a firm, even close (Rev F: was 6). Each
nib is **10 wide × 2.0 tall, projecting 1.0 mm** (up from 8 × 1.5 × 0.8); the
1.35 mm pocket still leaves ~1 mm of the 2.4 mm wall. Clean exterior, no screws.
Positions dodge the ports and the side power switch. Tune `SNAP_PROJ` to your
printer/material; PETG/ABS/ASA flex better than PLA. If the close is too stiff to
open by hand, back `SNAP_PROJ`/`SNAP_H` down slightly or drop a side tab.

## Reprint scope

**Rev F changes the shell itself** (shorter `IH`, deeper `INNER_D`) plus the screen
window, control positions, and the whole tray component layout — so reprint **tray
+ lid**. Everything downstream (D-pad, navcap, mic mount to the lid; battery/boards
tape to the floor) stays as before, just at the new coordinates.

## ⚠️ Verify before a final print

1. **Screen window** — module PCB/glass **`SCR_MOD_W/H` = 55.6 × 91** (MEASURED);
   **window `SCR_GLASS_W/H` = 54 × 88 is deliberately < the module width** so the lip
   retains it (~0.8 mm overlap/side). Keep the window *under* `SCR_MOD_W` but *over*
   the active `SCR_ACT_W` (49) — verify your panel's real active width if it differs.
   Confirm `SCR_BODY_T` (module depth) and ribbon reach. Touch (cap) lines not routed.
2. **Mic** — INMP441 is top-ported; mount the board port-toward-the-lid under the
   grille at (10, 48), taped to the lid (no pegs). Check the 14 mm board clears the
   left wall (~2 mm) and the D-pad board below it.
3. **Nav opening / integrated D-pad** — square `NAV_SW` (9.9 mm) + `NAV_SW_CLR`
   (0.25/side) = 10.4 mm opening clears the measured square switch body so it noses up
   through the lid and the actuator protrudes and still tilts. `NAV_SW_H` (3.9 mm) sets
   how far the switch reaches — with the actuator ~1 mm proud the PCB seats ~1 mm below
   the inner face (`T_DPAD` ≈ 2.7). Tune `NAV_SW_CLR` to your print (0.25 mm is snug).
   Solder **wires / a flat or right-angle header** (no tall pins) so the board
   lays flat. Verify the actuator dia and that it tilts freely in the square opening.
4. **Port heights** (`zrel` per port; default floor+3.2, **HP = 4.7**). Verify
   against real connectors. Rev F I/O: **microSD out the TOP edge**; **USB-C +
   headphone both out the BOTTOM edge** (HP left @ X11, USB-C right @ X46, raised
   1.5 mm) — confirm the two bottom connectors + snap tabs (@ X28, X57) don't clash.
5. **Battery** — EEMB LP603449 (51×34.5×6.3, JST-PHR-02), now **landscape** in the
   chin (`BATT` w=51, h=34.5); confirm the charger's B+/B- match the connector
   polarity and that the JST tail exits toward a gap (DAC side or a wall), not into
   the screen-module footprint.
6. **Speaker behind screen** — back-firing under the screen module, **top-right at
   Y≈135**, right under the top edge (the charger moved to the bottom row, freeing
   this corner). Module (4.3 mm) + speaker (5.5 mm) both fit the 15 mm cavity;
   confirm your driver depth and that the speaker clears the microSD (≈2 mm X-gap)
   and the top wall (≈2.2 mm).
7. **Power switch** — `PWR_*` (7×7 body, 6.5 mm plunger window, cradle ribs) are
   estimates; verify the real plunger size/shape and body depth, and glue the body
   into the cradle. The 6 pins face into the cavity — wire them (no PCB).
8. **Snap force** — `SNAP_PROJ` is a starting value; tune to your print.
