#!/usr/bin/env python3
"""Generate the QuranNode mainboard OUTLINE + placement guide for KiCad, straight
from the enclosure's single source of truth (hardware/enclosure/params.py).

Outputs (in this folder):
  - qurannode-outline.dxf   : board Edge.Cuts outline (import into KiCad reliably)
  - qurannode.kicad_pcb     : best-effort KiCad 8 PCB with the outline + placement
                              labels baked in (open directly; if the format version
                              mismatches your KiCad, use the DXF instead).
                              WARNING: regenerating OVERWRITES this file -- once you
                              start real layout, don't rerun; re-import the DXF instead.
  - placement.md            : connector / component positions in board coords

Board frame: origin (0,0) = TOP-LEFT of the board, X right, Y DOWN (KiCad native).
Device frame (enclosure): X right 0..IW, Y UP 0..IH.  Mapping:
    kx = deviceX - BOARD_CLR ;  ky = (IH - BOARD_CLR) - deviceY
"""
import os, sys, math, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "enclosure"))
import params as P

BOARD_CLR = 0.6                       # board inset from the inner cavity wall (per side)
R_IN      = P.R_OUT - P.WALL          # inner cavity corner radius
BW = round(P.IW - 2*BOARD_CLR, 3)     # board width
BH = round(P.IH - 2*BOARD_CLR, 3)     # board height
BR = round(max(0.8, R_IN - BOARD_CLR), 3)   # board corner radius

def d2b(x, y):
    """device (X right, Y up) -> board (X right, Y down, origin top-left), clamped to board"""
    bx = min(max(x - BOARD_CLR, 0.0), BW)
    by = min(max((P.IH - BOARD_CLR) - y, 0.0), BH)
    return (round(bx, 3), round(by, 3))

def rounded_rect_pts(w, h, r, seg=6):
    corners = [                                   # (cx, cy, a0, a1)  clockwise, y-down
        (w-r, r,   -90,   0),                     # top-right
        (w-r, h-r,   0,  90),                     # bottom-right
        (r,   h-r,  90, 180),                     # bottom-left
        (r,   r,   180, 270),                     # top-left
    ]
    pts = []
    for cx, cy, a0, a1 in corners:
        for i in range(seg+1):
            a = math.radians(a0 + (a1-a0)*i/seg)
            pts.append((round(cx + r*math.cos(a), 3), round(cy + r*math.sin(a), 3)))
    return pts

OUT = rounded_rect_pts(BW, BH, BR)
EDGES = list(zip(OUT, OUT[1:] + OUT[:1]))         # closed loop

# ---- connector / component placement guides (device coords -> board coords) -------
def port_xy(pd):
    e, pos = pd["edge"], pd["pos"]
    if e == "B": return (pos, 0.0)
    if e == "T": return (pos, P.IH)
    if e == "L": return (0.0, pos)
    return (P.IW, pos)

GUIDES = [
    ("J1 USB-C (bottom edge)",  d2b(*port_xy(P.PORT_USB))),
    ("J6 3.5mm jack (bottom)",  d2b(*port_xy(P.PORT_HP))),
    ("J2 microSD (top edge)",   d2b(*port_xy(P.PORT_SD))),
    ("SW3 power (left edge)",   d2b(0.0, P.PWR_POS)),
    ("U1 ESP32-S3",             d2b(P.ESP32["cx"], P.ESP32["cy"])),
    ("MK1 mic (=lid grille!)",  d2b(P.MIC_CX, P.MIC_CY)),
    ("SW4 nav",                 d2b(P.NAV_CX, P.NAV_CY)),
    ("J4 speaker",              d2b(P.SPK_CX, P.SPK_CY)),
    ("U4 TP4056 charger",       d2b(P.CHG["cx"], P.CHG["cy"])),
    ("U2 DAC / J6",             d2b(P.DAC["cx"], P.DAC["cy"])),
    ("J5 battery",              d2b(P.BATT["cx"], P.BATT["cy"])),
]

# keep-outs: the battery footprint sits on the floor over the board (tape-mount)
BATT_KEEPOUT = (d2b(P.BATT["cx"]+P.BATT["w"]/2, P.BATT["cy"]+P.BATT["h"]/2),
                d2b(P.BATT["cx"]-P.BATT["w"]/2, P.BATT["cy"]-P.BATT["h"]/2))

# ---------------------------------------------------------------- DXF (robust) ------
def write_dxf(path):
    L = ["0","SECTION","2","ENTITIES"]
    for (x1,y1),(x2,y2) in EDGES:
        # DXF is Y-up; flip Y so the shape matches the KiCad Y-down frame visually
        L += ["0","LINE","8","Edge.Cuts",
              "10",f"{x1}","20",f"{-y1}","30","0",
              "11",f"{x2}","21",f"{-y2}","31","0"]
    L += ["0","ENDSEC","0","EOF"]
    open(path,"w").write("\n".join(L)+"\n")

# ---------------------------------------------------------------- KiCad PCB ---------
PCB_HEAD = '''(kicad_pcb
\t(version 20240108)
\t(generator "qurannode_outline_gen")
\t(generator_version "8.0")
\t(general (thickness 1.6))
\t(paper "A4")
\t(layers
\t\t(0 "F.Cu" signal)
\t\t(1 "In1.Cu" signal)
\t\t(2 "In2.Cu" signal)
\t\t(31 "B.Cu" signal)
\t\t(32 "B.Adhes" user "B.Adhesive")
\t\t(33 "F.Adhes" user "F.Adhesive")
\t\t(34 "B.Paste" user)
\t\t(35 "F.Paste" user)
\t\t(36 "B.SilkS" user "B.Silkscreen")
\t\t(37 "F.SilkS" user "F.Silkscreen")
\t\t(38 "B.Mask" user)
\t\t(39 "F.Mask" user)
\t\t(40 "Dwgs.User" user "User.Drawings")
\t\t(41 "Cmts.User" user "User.Comments")
\t\t(42 "Eco1.User" user "User.Eco1")
\t\t(43 "Eco2.User" user "User.Eco2")
\t\t(44 "Edge.Cuts" user)
\t\t(45 "Margin" user)
\t\t(46 "B.CrtYd" user "B.Courtyard")
\t\t(47 "F.CrtYd" user "F.Courtyard")
\t\t(48 "B.Fab" user)
\t\t(49 "F.Fab" user)
\t)
\t(setup (pad_to_mask_clearance 0))
\t(net 0 "")
'''

def uid(): return str(uuid.uuid4())

def write_pcb(path):
    body = [PCB_HEAD]
    for (x1,y1),(x2,y2) in EDGES:
        body.append(
            f'\t(gr_line (start {x1} {y1}) (end {x2} {y2}) '
            f'(stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{uid()}"))')
    for label,(x,y) in GUIDES:
        body.append(
            f'\t(gr_text "{label}" (at {x} {y}) (layer "Cmts.User") (uuid "{uid()}")\n'
            f'\t\t(effects (font (size 1 1) (thickness 0.15))))')
    (a,b) = BATT_KEEPOUT
    body.append(
        f'\t(gr_text "battery keep-out (tape-mount over board)" '
        f'(at {b[0]} {b[1]}) (layer "Cmts.User") (uuid "{uid()}")\n'
        f'\t\t(effects (font (size 1 1) (thickness 0.15)) (justify left)))')
    body.append(")\n")
    open(path,"w").write("\n".join(body))

# ---------------------------------------------------------------- placement.md ------
def write_placement(path):
    lines = [f"# Board outline + placement (board frame, mm; origin top-left, Y down)",
             "",
             f"- Board: **{BW} x {BH} mm**, corner radius {BR} mm "
             f"(inner cavity {P.IW} x {P.IH} minus {BOARD_CLR} mm/side).",
             f"- Overall device thickness {round(P.TOTAL_D,2)} mm; cavity depth {P.INNER_D} mm.",
             "",
             "| Ref | Board X | Board Y | Note |",
             "|-----|--------:|--------:|------|"]
    for label,(x,y) in GUIDES:
        lines.append(f"| {label} | {x} | {y} | |")
    (a,b) = BATT_KEEPOUT
    lines += ["",
              f"**Battery keep-out** (tape-mounted cell over the board): "
              f"X {b[0]}..{a[0]}, Y {a[1]}..{b[1]} mm — avoid tall parts here.",
              "",
              "Ports are on the enclosure edges: **USB-C + headphone on the BOTTOM, "
              "microSD on the TOP, power switch on the LEFT.** Place those connectors "
              "hard against the matching board edge at the X/Y above."]
    open(path,"w").write("\n".join(lines)+"\n")

if __name__ == "__main__":
    write_dxf(os.path.join(HERE, "qurannode-outline.dxf"))
    write_pcb(os.path.join(HERE, "qurannode.kicad_pcb"))
    write_placement(os.path.join(HERE, "placement.md"))
    print(f"board {BW} x {BH} mm, r={BR}  ({len(EDGES)} edge segments)")
    print("wrote qurannode-outline.dxf, qurannode.kicad_pcb, placement.md")
