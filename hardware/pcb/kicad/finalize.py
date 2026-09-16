#!/usr/bin/env python3
"""Import the FreeRouting .ses, add F/B GND copper pours (thermal-connect GND SMD
pads), refill all zones (inner GND/3V3 planes + outer GND), and report connectivity.
No fanout via-in-pads (those bridged neighbours). KiCad bundled python."""
import os, sys, pcbnew
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_board_outline as G
PCB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qurannode.kicad_pcb")
SES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qurannode.ses")
def mm(v): return pcbnew.FromMM(v)

b = pcbnew.LoadBoard(PCB)
try: pcbnew.ImportSpecctraSES(b, SES)
except TypeError: pcbnew.ImportSpecctraSES(SES)

xs=[p for e in G.EDGES for p in (e[0][0],e[1][0])]; ys=[p for e in G.EDGES for p in (e[0][1],e[1][1])]
I=0.4   # inset the pour from the board edge (copper-edge clearance)
x0,x1,y0,y1=min(xs)+I,max(xs)-I,min(ys)+I,max(ys)-I
cor=[(x0,y0),(x1,y0),(x1,y1),(x0,y1)]
gnd=b.FindNet("GND")
for L in (pcbnew.F_Cu, pcbnew.B_Cu):
    z=pcbnew.ZONE(b); z.SetLayer(L); z.SetNetCode(gnd.GetNetCode())
    po=z.Outline(); po.NewOutline()
    for (x,y) in cor: po.Append(mm(x),mm(y))
    b.Add(z)
for z in b.Zones():                       # solid pad connection -> no starved thermals
    try: z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
    except Exception: pass
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.BuildConnectivity()
un=b.GetConnectivity().GetUnconnectedCount(True)
tr=len([t for t in b.GetTracks() if t.Type()==pcbnew.PCB_TRACE_T])
vi=len([t for t in b.GetTracks() if t.Type()==pcbnew.PCB_VIA_T])
print(f"tracks {tr}  vias {vi}  unconnected {un}")
pcbnew.SaveBoard(PCB, b)
