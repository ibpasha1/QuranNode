#!/usr/bin/env python3
"""Pipeline step (e2), run AFTER finalize.py: close the last three +3V3 gaps
that FreeRouting cannot see. FR assumes the In2 +3V3 plane is solid, but GND
via fences split it into islands in the power corridor, and the INMP441's tiny
0.3 mm 3V3 pad gets no fanout.

The fixes below are POSITION-SPECIFIC to the committed qurannode.ses (build +
FR -mt 1 + finalize are deterministic, so a rebuild reproduces the same
coordinates). If you change the netlist, placement, or re-route, re-derive
them: run kicad-cli DRC, look at the unconnected pairs, and pick bridge paths
clear of other-net vias/holes on the target layer (obstacle dump recipe in
HANDOFF §4). Verify with kicad-cli DRC afterwards — that is the ground truth.

Fixes (all +3V3, verified 0 unconnected / 0 electrical DRC faults):
  1. B.Cu bridge between plane vias (36.36,110.46)->(42.46,110.32), elbow at
     (38.30,109.70) to clear the GND via at (38.26,110.45): joins the In2
     island x37.9..45.7 back to the main plane.
  2. B.Cu bridge (15.25,105.01)->(16.40,102.12) between plane vias: joins the
     In2 island x16.1..19.8 back to the main plane.
  3. MK1 (INMP441) pad 3 fanout: F.Cu stub south from inside the pad
     (9.40,106.30)->(9.40,107.60) + via to the plane. Stub starts 0.11 mm off
     pad-centre so it keeps >=0.25 mm from the mic port NPTH at (9.40,105.51);
     south is the only clear direction (port N, MIC_SD W, GND E).
Idempotent: skips anything already present.
"""
import os, pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, "qurannode.kicad_pcb")
W, VIA_D, VIA_DRILL = 0.25, 0.6, 0.3

def mm(v): return pcbnew.FromMM(v)

b = pcbnew.LoadBoard(PCB)
net = b.FindNet("+3V3")

TRACKS = [  # (x1,y1,x2,y2, layer)
    (36.36, 110.46, 38.30, 109.70, pcbnew.B_Cu),   # island bridge 1a
    (38.30, 109.70, 42.46, 110.32, pcbnew.B_Cu),   # island bridge 1b
    (15.25, 105.01, 16.40, 102.12, pcbnew.B_Cu),   # island bridge 2
    (9.40,  106.30,  9.40, 107.60, pcbnew.F_Cu),   # MK1.3 fanout stub
]
VIAS = [(9.40, 107.60)]                             # MK1.3 fanout via

have_t = set()
have_v = set()
for t in b.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T:
        have_v.add((t.GetStart().x, t.GetStart().y))
    else:
        have_t.add((t.GetStart().x, t.GetStart().y, t.GetEnd().x, t.GetEnd().y, t.GetLayer()))

added = 0
for x1, y1, x2, y2, lay in TRACKS:
    if (mm(x1), mm(y1), mm(x2), mm(y2), lay) in have_t: continue
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1))); t.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2)))
    t.SetWidth(mm(W)); t.SetLayer(lay); t.SetNet(net); b.Add(t); added += 1
for x, y in VIAS:
    if (mm(x), mm(y)) in have_v: continue
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
    v.SetWidth(mm(VIA_D)); v.SetDrill(mm(VIA_DRILL)); v.SetNet(net); b.Add(v); added += 1

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(PCB, b)
print(f"heal_plane: added {added} items (tracks+vias); run kicad-cli DRC to confirm 0 unconnected")
