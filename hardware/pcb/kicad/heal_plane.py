#!/usr/bin/env python3
"""Pipeline step (e2), run AFTER finalize.py: close the +3V3 gaps FreeRouting cannot
see. FR treats the In2 +3V3 plane as solid, but GND via-fences split it into islands
in the power corridor, so IC 3V3 fanout stubs (F.Cu stub + via to In2) land on stranded
plane islands and read as unconnected.

Each stranded stub has a +3V3 via to B.Cu, so we join the islands with short B.Cu
bridges between those vias (B.Cu is a GND pour that simply clears around the +3V3 track).

POSITION-SPECIFIC to the committed qurannode.ses (build + FR -mt 1 + finalize are
deterministic). If you change the netlist/placement/route, re-derive: run kicad-cli DRC,
read the unconnected +3V3 pairs' coords (`pos` in the JSON), snap each to the nearest
+3V3 via, and list the via-to-via pairs below. Verify with kicad-cli DRC afterwards.
"""
import os, pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PCB  = os.path.join(HERE, "qurannode.kicad_pcb")
W    = 0.3                                    # bridge width (power net)

def mm(v): return pcbnew.FromMM(v)

# B.Cu bridges as polylines through existing +3V3 vias (auto-derived from the DRC
# unconnected pairs + union-find, snapped to nearest +3V3 via). Bridge 3 has an elbow
# waypoint to detour two vias @ x~19.04, y137.3/138.0.
BRIDGES = [
    [(11.058, 103.357), (15.541, 104.821)],                       # audio row island -> main
    [(20.322, 103.637), (15.541, 104.821)],                       # audio row island -> main
    [(17.839, 141.225), (18.000, 138.000), (19.171, 136.018)],    # amp island (elbow past x19 vias)
    [(18.020, 110.010), (15.961, 108.223)],                       # sliver island -> main
]

b   = pcbnew.LoadBoard(PCB)
net = b.FindNet("+3V3")

have = set()
for t in b.GetTracks():
    if t.Type() != pcbnew.PCB_VIA_T:
        have.add((t.GetStart().x, t.GetStart().y, t.GetEnd().x, t.GetEnd().y, t.GetLayer()))

added = 0
for poly in BRIDGES:
    for (x1, y1), (x2, y2) in zip(poly, poly[1:]):
        key = (mm(x1), mm(y1), mm(x2), mm(y2), pcbnew.B_Cu)
        if key in have: continue
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(pcbnew.VECTOR2I(mm(x1), mm(y1)))
        t.SetEnd(pcbnew.VECTOR2I(mm(x2), mm(y2)))
        t.SetWidth(mm(W)); t.SetLayer(pcbnew.B_Cu); t.SetNet(net)
        b.Add(t); added += 1

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(PCB, b)
print(f"heal_plane: added {added} B.Cu +3V3 bridges; run kicad-cli DRC to confirm 0 unconnected")
