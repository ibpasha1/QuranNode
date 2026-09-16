#!/usr/bin/env python3
"""Build a placed KiCad board from qurannode.net using KiCad's pcbnew engine, and
export a Specctra .dsn for FreeRouting. Run with KiCad's bundled python:

  $KPY build_board.py

Outline + placement anchors come from gen_board_outline (enclosure single source of
truth). Passives are placed at the centroid of the anchors they connect to.
"""
import os, re, sys, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew
import gen_board_outline as G

HERE = os.path.dirname(os.path.abspath(__file__))
FPDIR = "/Volumes/KiCad/KiCad/KiCad.app/Contents/SharedSupport/footprints"
NET = os.path.join(HERE, "qurannode.net")
OUT = os.path.join(HERE, "qurannode.kicad_pcb")
DSN = os.path.join(HERE, "qurannode.dsn")

def mm(v): return pcbnew.FromMM(float(v))
def V(x, y): return pcbnew.VECTOR2I(mm(x), mm(y))

# ---------------------------------------------------------------- parse netlist
src = open(NET).read()
comp_blocks = re.split(r'\(comp\s+\(ref', src)[1:]
comps = {}                                   # ref -> (value, footprint)
for b in comp_blocks:
    ref = re.search(r'^\s*"([^"]+)"', b).group(1)
    val = re.search(r'\(value "([^"]*)"\)', b)
    fp  = re.search(r'\(footprint "([^"]+)"\)', b)
    comps[ref] = (val.group(1) if val else "", fp.group(1) if fp else "")
netblk = src[src.index('(nets'):]
net_chunks = re.split(r'\(net\s*\n\s*\(code', netblk)[1:]
nets = {}                                    # netname -> [(ref, pin)]
ref_nets = {}                                # ref -> set(netnames)
for ch in net_chunks:
    nm = re.search(r'\(name "([^"]+)"\)', ch).group(1)
    nodes = re.findall(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', ch)
    nets[nm] = nodes
    for ref, pin in nodes:
        ref_nets.setdefault(ref, set()).add(nm)
print(f"parsed {len(comps)} comps, {len(nets)} nets")

# ---------------------------------------------------------------- anchors (CADed enclosure)
import math
GU = {lbl.split()[0]: xy for lbl, xy in G.GUIDES}   # "U1"->(x,y), "MK1"->, "J3" n/a
P = G.P
HDR_OFFSET = 49.4                                    # mm, LCD header row -> glass centre
J3_POS = G.d2b(P.SCR_CX, P.SCR_CY - HDR_OFFSET)      # LCD header aligned to the screen window
def clamp(x, y): return (min(max(x,3),G.BW-3), min(max(y,4),G.BH-4))

# Main parts at their CADed ENCLOSURE positions (respect the case); small ICs + all
# passives cluster near the nets they connect to. NOTE: SKiDL refs vs enclosure labels
# -- SKiDL assigns SW1=nav, SW2=power (SW3/SW4 = reset/boot); the enclosure GUIDES
# label nav "SW4" / power "SW3", so map explicitly.
REF_ANCHOR = {r: GU[r] for r in ("J1","J2","J4","J5","J6","U1","U2","U4","MK1") if r in GU}
REF_ANCHOR["J3"]  = J3_POS
REF_ANCHOR["SW1"] = GU.get("SW4")   # nav 5-way -> nav enclosure spot (below LCD).
# (Mechanical power switch removed -> soft-latch; SW2/SW3 = reset/boot, cluster by net.)

# load footprints up front so placement uses real sizes (no overlaps). Rotate the LCD
# header 90 deg so its 14-pin row runs HORIZONTAL (module mounts portrait, centred).
fp_objs = {}
for ref,(val,fpid) in comps.items():
    lib,name = fpid.split(":",1)
    libdir = os.path.join(HERE,"qurannode.pretty") if lib=="qurannode" else f"{FPDIR}/{lib}.pretty"
    fp = pcbnew.FootprintLoad(libdir, name)
    if fp is None: print("  !! missing fp", fpid); continue
    fp.SetReference(ref); fp.SetValue(val)
    if "PinHeader_1x14" in name: fp.SetOrientationDegrees(90)
    fp_objs[ref] = fp

def half_ext(fp):
    # use pad EDGES (position +/- size/2), not centres, so big-pad parts (inductor)
    # don't overlap; small margin is then enough -> keeps routing fast.
    ps=list(fp.Pads())
    xmin=min(pcbnew.ToMM(p.GetPosition().x)-pcbnew.ToMM(p.GetSize().x)/2 for p in ps)
    xmax=max(pcbnew.ToMM(p.GetPosition().x)+pcbnew.ToMM(p.GetSize().x)/2 for p in ps)
    ymin=min(pcbnew.ToMM(p.GetPosition().y)-pcbnew.ToMM(p.GetSize().y)/2 for p in ps)
    ymax=max(pcbnew.ToMM(p.GetPosition().y)+pcbnew.ToMM(p.GetSize().y)/2 for p in ps)
    return ((xmax-xmin)/2+0.5, (ymax-ymin)/2+0.5)

GRID, occ = 1.0, set()
def _cells(cx,cy,hw,hh):
    return [(gx,gy) for gx in range(int((cx-hw)//GRID),int((cx+hw)//GRID)+1)
                     for gy in range(int((cy-hh)//GRID),int((cy+hh)//GRID)+1)]
def place(cx,cy,hw,hh):
    cx,cy=clamp(cx,cy)
    cand=[(cx,cy)]+[(cx+r*0.9*math.cos(math.radians(a)),cy+r*0.9*math.sin(math.radians(a)))
                    for r in range(1,95) for a in range(0,360,18)]
    for nx,ny in cand:
        nx,ny=clamp(nx,ny); cells=_cells(nx,ny,hw,hh)
        if all(c not in occ for c in cells): occ.update(cells); return (nx,ny)
    occ.update(_cells(cx,cy,hw,hh)); return (cx,cy)

pos = {}
for ref in REF_ANCHOR:                               # anchors first, at their enclosure spots
    if ref in fp_objs: hw,hh=half_ext(fp_objs[ref]); pos[ref]=place(*REF_ANCHOR[ref], hw, hh)
anchor_xy = dict(pos)
for ref in comps:                                    # rest: centroid of connected anchors
    if ref in pos or ref not in fp_objs: continue
    axy=[anchor_xy[o] for nm in ref_nets.get(ref,()) for o,_ in nets[nm] if o in anchor_xy]
    cx,cy=(sum(p[0] for p in axy)/len(axy),sum(p[1] for p in axy)/len(axy)) if axy else (G.BW/2,G.BH/2)
    hw,hh=half_ext(fp_objs[ref]); pos[ref]=place(cx,cy,hw,hh)
print(f"placed {len(pos)} parts (size-aware); J3 header rotated horizontal")

# ---------------------------------------------------------------- build board
board = pcbnew.NewBoard(OUT)
board.SetCopperLayerCount(4)
# JLCPCB-friendly design rules (so autoroute vias/tracks don't flag against defaults)
try:
    ds = board.GetDesignSettings()
    ds.m_ViasMinSize      = pcbnew.FromMM(0.3)
    ds.m_MinThroughDrill  = pcbnew.FromMM(0.2)
    ds.m_TrackMinWidth    = pcbnew.FromMM(0.15)
    ds.m_MinClearance     = pcbnew.FromMM(0.1)
    for attr,val in (("m_HoleClearance",0.15),("m_HoleToHoleMin",0.15),
                     ("m_CopperEdgeClearance",0.1),("m_MinThroughDrill",0.15)):
        if hasattr(ds,attr): setattr(ds,attr,pcbnew.FromMM(val))
except Exception as e:
    print("  (design-rule set skipped:", e, ")")
# outline
for (x1,y1),(x2,y2) in G.EDGES:
    s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(x1,y1)); s.SetEnd(V(x2,y2))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(mm(0.1)); board.Add(s)
# LCD shield outline (55x97) on silk -- module sits above the board, glass up
lcd = pcbnew.PCB_SHAPE(board); lcd.SetShape(pcbnew.SHAPE_T_RECT)
lcd.SetStart(V(3.9,2)); lcd.SetEnd(V(58.9,99))
lcd.SetLayer(pcbnew.F_SilkS); lcd.SetWidth(mm(0.15)); board.Add(lcd)
# place footprints so each part's pad-array CENTRE lands on its computed position
for ref, fp in fp_objs.items():
    fp.SetPosition(V(*pos[ref]))
    ps=list(fp.Pads()); xs=[p.GetPosition().x for p in ps]; ys=[p.GetPosition().y for p in ps]
    c=pcbnew.VECTOR2I((min(xs)+max(xs))//2, (min(ys)+max(ys))//2)
    fp.Move(pcbnew.VECTOR2I(V(*pos[ref]).x - c.x, V(*pos[ref]).y - c.y))
    board.Add(fp)
    try: fp.Reference().SetLayer(pcbnew.F_Fab)   # refs off silk -> no silk-over-copper
    except Exception: pass
# nets
netmap = {}
for nm in nets:
    ni = pcbnew.NETINFO_ITEM(board, nm); board.Add(ni); netmap[nm] = ni
assigned = 0
for nm, nodes in nets.items():
    for ref, pin in nodes:
        fp = fp_objs.get(ref)
        if not fp: continue
        for pad in fp.Pads():
            if pad.GetNumber() == pin:
                pad.SetNet(netmap[nm]); assigned += 1
print(f"assigned {assigned} pad-net connections")

# inner power planes: In1.Cu = GND, In2.Cu = +3V3, so every GND/3V3 pad vias to a
# solid plane and the router only has to do signals + VSYS/VBUS on the outer layers.
xs = [p for e in G.EDGES for p in (e[0][0], e[1][0])]
ys = [p for e in G.EDGES for p in (e[0][1], e[1][1])]
corners = [(min(xs),min(ys)),(max(xs),min(ys)),(max(xs),max(ys)),(min(xs),max(ys))]
for layer, netname in ((pcbnew.In1_Cu,"GND"), (pcbnew.In2_Cu,"+3V3")):
    z = pcbnew.ZONE(board); z.SetLayer(layer); z.SetNetCode(netmap[netname].GetNetCode())
    poly = z.Outline(); poly.NewOutline()
    for (x,y) in corners: poly.Append(mm(x), mm(y))
    board.Add(z)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
print("added GND (In1) + 3V3 (In2) planes")

pcbnew.SaveBoard(OUT, board)
print("saved", os.path.basename(OUT))
try:
    ok = pcbnew.ExportSpecctraDSN(board, DSN)
    print("DSN export:", ok, os.path.basename(DSN))
except TypeError:
    pcbnew.ExportSpecctraDSN(DSN)   # some builds take (filename) on the loaded board
    print("DSN export (1-arg):", os.path.basename(DSN))
