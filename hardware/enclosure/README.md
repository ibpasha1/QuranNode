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

- **Envelope:** 68.8 × 184.8 × **15.8 mm** (Rev E: taller, I/O consolidated so the
  middle is a clean battery zone).
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
  lid top via a stepped rabbet. **Window 56 × 86.5**, module recess **59 × 91**
  (> glass → retaining lip), active 49 × 74. The wider Rev D shell gives the recess
  real ~1.5 mm edge walls (they were paper-thin before).
- **D-pad** below the screen (MEASURED board **41 × 25 mm landscape**): the 5-way
  switch **knob pokes through a round Ø9.5 hole** (`NAV_D`) centred at (32, 42) —
  press the edges to tilt N/E/S/W + press. The board mounts **FLAT against the lid**
  (component side up, switch through the hole, board resting on its own tactiles;
  short Ø2.8 × 1.5 pegs just locate it) — saves chin depth. **SET/RST covered.**
- **Mic** — INMP441 (14 × 14 mm), earpiece-style 7-hole grille in the gap between
  the screen and the D-pad; board on two locating pegs, port toward the front.

**BACK (tray) — I/O consolidated to free a middle battery zone:**
- **TOP:** ESP32-S3 (28×63, vertical, left) · 28 mm **speaker** (back grille, right)
  · **microSD** (slot out the **top edge**, middle) · **TP4056 charger** (USB-C out
  the **top edge**, right) · **power switch** (7×7×16, side-mounted, plunger out the
  **upper-left edge**, cradle ribs — glue to retain).
- **BOTTOM:** **PCM5102 DAC** only, mounted **landscape** (32 × 17) — the 3.5 mm jack
  is on the board's **long edge**, so it points down out the **bottom edge** (toward
  the left), **raised 1.5 mm** (`PORT_HP zrel=4.7`).
- **MIDDLE:** the **battery** (left) beside the **TPS63020 buck-boost regulator**
  (26 × 18, right, under the charger — clean power path charger→cell→regulator→rails).
  ESP32 USB-C internal.

## Vertical stack (~15.8 mm)

The front/back split is what keeps it thin: screen + D-pad ride under the lid,
everything else sits on the tray floor, so most of the footprint is a single
component layer per side.

| Layer | mm |
|-------|----|
| Back skin (floor) | 1.4 |
| Interior cavity (boards / battery / speaker) | 13.0 |
| Front skin (lid) | 1.4 |

## Battery

Sized for the **EEMB LP603449** — 51 × 34.5 × **6.3 mm**, 1100 mAh, JST-PHR-02.
Its thinness is what keeps the whole device at 15.8 mm: at 6.3 mm it sits on the
floor *behind* the screen module (which only intrudes 4.3 mm) with room to spare,
and its 51 mm length lets it span the chin, centred under the D-pad. **No corral**
— tape it to the floor anywhere there's room; the tray stays generic.

> Capacity note: a thick 2000 mAh brick (e.g. EEMB LP103454, 10.6 mm) does **not**
> fit this envelope — 10.6 mm nearly fills the cavity, and nothing can then sit
> behind the screen. Fitting one forces ~17 mm thick and a wider (~69 mm) shell.
> The thin 1100 mAh cell was the right call for a slim reader; ~7–10 h active.

## Fastening — internal snap-fit

Six discrete nibs on the lid lip click into pockets in the tray wall (two each on
the top/bottom edges, one each side). Clean exterior, no screws. Tune `SNAP_PROJ`
(default 0.8 mm) to your printer/material; PETG/ABS/ASA flex better than PLA.

## Reprint scope

**Rev C touches both parts** (screen window + covered buttons on the lid; speaker,
DAC/jack, ports and +1 mm thickness on the tray) — so reprint **tray + lid** this
round. After this, control/mic tweaks stay **lid-only** (D-pad, navcap, mic all
mount to the lid; battery is taped to the floor).

## ⚠️ Verify before a final print

1. **Screen window** — **56 × 86.5**; `SCR_MOD_W/H` (59 × 91) is the module recess —
   **verify against your real module PCB**; make it ≥ the PCB with a small lip.
   Confirm `SCR_BODY_T` (module depth) and ribbon reach. Touch (cap) lines are not
   yet routed. If still tight, bump `SCR_GLASS_W/H` and/or `SCR_MOD_W/H`.
2. **Mic** — INMP441 is top-ported; mount the board port-toward-the-lid under the
   grille at (30, 48). Verify the round board's real mounting-hole spots vs `MIC_PEGS`.
3. **Nav knob hole / flat D-pad** — `NAV_D` (9.5 mm) must clear the switch knob and
   let it tilt. Board mounts **flat against the lid** — solder **wires / a flat or
   right-angle header** (no tall pins) so it truly lays flat and nothing juts into
   the cavity. Nav/board offsets are eyeballed — test-fit first.
4. **Port heights** (`zrel` per port; default floor+3.2, **HP = 4.7**). Verify
   against real connectors. I/O is now split: **USB-C + microSD out the TOP edge**,
   **headphone out the BOTTOM edge** (raised 1.5 mm).
5. **Battery** — EEMB LP603449 (51×34.5×6.3, JST-PHR-02); confirm the charger's
   B+/B- match the connector polarity. The middle zone now has slack — a **bigger
   cell fits** if you resize `BATT`.
6. **Speaker behind screen** — top, back-firing under the screen module. Module
   (4.3 mm) + speaker (5.5 mm) both fit the 13 mm cavity; confirm your driver depth.
7. **Power switch** — `PWR_*` (7×7 body, 6.5 mm plunger window, cradle ribs) are
   estimates; verify the real plunger size/shape and body depth, and glue the body
   into the cradle. The 6 pins face into the cavity — wire them (no PCB).
8. **Snap force** — `SNAP_PROJ` is a starting value; tune to your print.
