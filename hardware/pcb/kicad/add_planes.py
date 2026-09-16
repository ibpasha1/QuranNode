#!/usr/bin/env python3
"""Add GND copper pours (F.Cu + B.Cu) to the routed board so all GND pads tie to the
plane, and refill. Reports unconnected count before/after. KiCad bundled python."""
import os, sys, pcbnew
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_board_outline as G

PCB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qurannode.kicad_pcb")
def mm(v): return pcbnew.FromMM(v)

b = pcbnew.LoadBoard(PCB)
b.BuildConnectivity()
print("unconnected before:", b.GetConnectivity().GetUnconnectedCount(True))

gnd = b.FindNet("GND")
xs = [p for e in G.EDGES for p in (e[0][0], e[1][0])]
ys = [p for e in G.EDGES for p in (e[0][1], e[1][1])]
x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]

for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    z.SetNetCode(gnd.GetNetCode())
    z.SetAssignedPriority(0)
    poly = z.Outline()
    poly.NewOutline()
    for (x, y) in corners:
        poly.Append(mm(x), mm(y))
    b.Add(z)

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
print("unconnected after pours:", b.GetConnectivity().GetUnconnectedCount(True))
pcbnew.SaveBoard(PCB, b)
print("saved with GND pours")
