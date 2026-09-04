#!/usr/bin/env freecadcmd
# ============================================================================
#  QuranNode -- handheld ESP32-S3 Quran reader ENCLOSURE -> FreeCAD B-rep + STEP
# ----------------------------------------------------------------------------
#  Slim SNAP-FIT two-part clamshell, portrait, built around a 3.5" 480x320
#  ST7796S TFT (screen dims lifted from the DSP-Mini lid). Two printed parts:
#    tray (back, holds boards+battery+speaker) and lid (front, screen+D-pad).
#  Front/back component split keeps it thin: screen+dpad ride under the lid,
#  boards+battery+speaker sit on the tray floor -- coplanar, one layer each side.
#
#  Run headless:
#    /Applications/FreeCAD.app/Contents/Resources/bin/freecadcmd freecad_export.py
#
#  Coordinate system == params.py: X 0..IW, Y 0..IH (bottom-up), Z 0=floor -> +Z lid.
# ============================================================================
import os, sys
import FreeCAD as App
import Part
from FreeCAD import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import params as P

# ----------------------------------------------------------------- shell detail params
R_OUT   = P.R_OUT
R_IN    = max(0.1, R_OUT - P.WALL)
LID_UNDER = P.FLOOR_T + P.INNER_D        # lid underside height
TOTAL     = LID_UNDER + P.TOP_T          # overall thickness
OX, OY  = -P.WALL, -P.WALL                # outer min corner
OW, OL  = P.OW, P.OL
LIP_H, LIP_GAP = 3.0, P.CLR              # lid lip depth + side clearance to wall
TOP_FILLET, BOT_FILLET = 0.8, 1.0   # top < TOP_T (1.4) or the round-over degenerates

# snap-fit tabs (edge code: B=bottom Y0, T=top YIH, L=left X0, R=right XIW)
SNAP_W, SNAP_PROJ, SNAP_H = 8.0, 0.8, 1.5
SNAP_TABS = [("B",12.0),("B",52.0),("T",12.0),("T",41.0),("L",100.0),("R",100.0)]  # clear of ports

Z_LEDGE = TOTAL - P.SCR_GLASS_STACK      # module-PCB seat (glass ends up flush w/ top)

# ----------------------------------------------------------------- helpers
def rrect(sx, sy, sz, r, at=(0,0,0)):
    parts = [Part.makeBox(sx-2*r, sy, sz, Vector(r,0,0)),
             Part.makeBox(sx, sy-2*r, sz, Vector(0,r,0))]
    for cx in (r, sx-r):
        for cy in (r, sy-r):
            parts.append(Part.makeCylinder(r, sz, Vector(cx,cy,0)))
    s = parts[0]
    for p in parts[1:]: s = s.fuse(p)
    s.translate(Vector(*at))
    return s.removeSplitter()

def ring(sx, sy, sz, w, r, at=(0,0,0)):
    outer = rrect(sx, sy, sz, r)
    inner = rrect(sx-2*w, sy-2*w, sz+2, max(0.1, r-w), (w, w, -1))
    s = outer.cut(inner); s.translate(Vector(*at))
    return s

def cyl(d, h, at):  return Part.makeCylinder(d/2.0, h, Vector(*at))

def horiz_face_at(shape, zt):
    cands = [f for f in shape.Faces
             if f.BoundBox.ZLength < 1e-3 and abs(f.BoundBox.ZMin - zt) < 1e-3]
    return max(cands, key=lambda f: f.Area) if cands else None

def fillet_perimeter(shape, zt, r):
    if r <= 0: return shape
    f = horiz_face_at(shape, zt)
    if not f:
        print("  ! fillet: no face at z=%.2f" % zt); return shape
    try:
        return shape.makeFillet(r, f.OuterWire.Edges).removeSplitter()
    except Exception as e:
        print("  ! fillet failed z=%.2f: %s" % (zt, e)); return shape

# ----------------------------------------------------------------- edge ports
def port_solid(pd):
    e, pos, w, h = pd["edge"], pd["pos"], pd["w"], pd["h"]
    zc = P.FLOOR_T + pd.get("zrel", 3.2)      # connector centre-line above floor (HP raised) [VERIFY]
    thru = P.WALL + 8
    round_ = (w == h)
    if e in ("B","T"):
        y0 = -P.WALL-2 if e=="B" else P.IH-4
        if round_: return cyl(w, thru, (pos, y0, zc)).rotated(Vector(pos,y0,zc),Vector(1,0,0),-90)
        return Part.makeBox(w, thru, h, Vector(pos-w/2, y0, zc-h/2))
    else:
        x0 = -P.WALL-2 if e=="L" else P.IW-4
        if round_: return cyl(w, thru, (x0, pos, zc)).rotated(Vector(x0,pos,zc),Vector(0,1,0),90)
        return Part.makeBox(thru, w, h, Vector(x0, pos-w/2, zc-h/2))

# ----------------------------------------------------------------- speaker grille (back / tray floor)
def speaker_grille():
    gx, gy = P.SPK_CX, P.SPK_CY
    rr = P.SPK_GRILLE_D/2.0
    slots, y = [], -rr + P.SPK_SLOT_W
    n = int(P.SPK_GRILLE_D // P.SPK_PITCH)
    for i in range(n):
        yy = (i-(n-1)/2.0)*P.SPK_PITCH
        half2 = rr*rr - yy*yy
        if half2 <= 1.0: continue
        half = half2**0.5
        L = 2*half - P.SPK_SLOT_W
        z0 = -1.0; hz = P.FLOOR_T + 2.0
        s = Part.makeBox(L, P.SPK_SLOT_W, hz, Vector(gx-L/2, gy+yy-P.SPK_SLOT_W/2, z0))
        s = s.fuse(cyl(P.SPK_SLOT_W, hz, (gx-L/2, gy+yy, z0)))
        s = s.fuse(cyl(P.SPK_SLOT_W, hz, (gx+L/2, gy+yy, z0)))
        slots.append(s)
    return slots

# ----------------------------------------------------------------- mic grille (front / lid)
def mic_grille(zc, hZ):
    """Small earpiece-style hole cluster over the INMP441 port: centre + hex ring."""
    holes = [cyl(P.MIC_HOLE_D, hZ, (P.MIC_CX, P.MIC_CY, zc))]
    r = P.MIC_GRILLE_D/2.0 - P.MIC_HOLE_D
    import math
    for k in range(6):
        a = math.radians(60*k)
        holes.append(cyl(P.MIC_HOLE_D, hZ, (P.MIC_CX+r*math.cos(a), P.MIC_CY+r*math.sin(a), zc)))
    return holes

# ----------------------------------------------------------------- power switch (side, left edge)
def power_hole():
    """Plunger window through the LEFT wall for the 7x7 self-locking switch."""
    zc = P.FLOOR_T + P.PWR_BODY/2.0
    return Part.makeBox(P.WALL+2, P.PWR_HOLE, P.PWR_HOLE,
                        Vector(-P.WALL-1, P.PWR_POS-P.PWR_HOLE/2, zc-P.PWR_HOLE/2))

def power_cradle():
    """Two floor ribs that locate the switch body against the wall (glue to retain)."""
    ribs = []
    for sy in (P.PWR_POS-P.PWR_BODY/2-P.PWR_RIB_T, P.PWR_POS+P.PWR_BODY/2):
        ribs.append(Part.makeBox(P.PWR_RIB_LEN, P.PWR_RIB_T, P.PWR_RIB_H, Vector(0, sy, P.FLOOR_T)))
    return ribs

# ----------------------------------------------------------------- snap-fit
LIPX0, LIPY0 = LIP_GAP, LIP_GAP
LIPW,  LIPL  = P.IW - 2*LIP_GAP, P.IH - 2*LIP_GAP
SNAP_ZC = LID_UNDER - LIP_H + 0.5

def _tab_frame(side, c):
    if side == "B": return (c, LIPY0, (0,-1))
    if side == "T": return (c, LIPY0+LIPL, (0,1))
    if side == "L": return (LIPX0, c, (-1,0))
    return (LIPX0+LIPW, c, (1,0))

def snap_nib(side, c):
    cx, cy, (nx, ny) = _tab_frame(side, c)
    if nx != 0:
        x0 = cx if nx > 0 else cx - SNAP_PROJ
        return Part.makeBox(SNAP_PROJ, SNAP_W, SNAP_H, Vector(x0, cy-SNAP_W/2, SNAP_ZC))
    y0 = cy if ny > 0 else cy - SNAP_PROJ
    return Part.makeBox(SNAP_W, SNAP_PROJ, SNAP_H, Vector(cx-SNAP_W/2, y0, SNAP_ZC))

def snap_pocket(side, c):
    cx, cy, (nx, ny) = _tab_frame(side, c)
    depth = SNAP_PROJ + 0.35; ph = SNAP_H + 0.6; z0 = SNAP_ZC - 0.3
    if nx != 0:
        x0 = cx if nx > 0 else cx - depth
        return Part.makeBox(depth, SNAP_W+0.6, ph, Vector(x0, cy-(SNAP_W+0.6)/2, z0))
    y0 = cy if ny > 0 else cy - depth
    return Part.makeBox(SNAP_W+0.6, depth, ph, Vector(cx-(SNAP_W+0.6)/2, y0, z0))

# ----------------------------------------------------------------- TRAY (back)
def build_tray():
    shell  = rrect(OW, OL, LID_UNDER, R_OUT, (OX, OY, 0))
    cavity = rrect(P.IW, P.IH, LID_UNDER+1, R_IN, (0, 0, P.FLOOR_T))
    tray = shell.cut(cavity)
    tray = tray.cut(Part.makeCompound([port_solid(p) for p in
                                       (P.PORT_USB, P.PORT_HP, P.PORT_SD)]))
    tray = tray.cut(Part.makeCompound(speaker_grille()))
    tray = tray.cut(power_hole())                          # power-switch plunger window (left wall)
    tray = tray.fuse(Part.makeCompound(power_cradle()))    # power-switch body cradle ribs
    # NOTE: no battery corral -- the cell is taped down wherever there's floor, so
    # the tray stays generic and lid iterations don't force a tray reprint.
    tray = tray.cut(Part.makeCompound([snap_pocket(s,c) for (s,c) in SNAP_TABS]))
    return tray.removeSplitter()

# ----------------------------------------------------------------- screen rabbet
def screen_features():
    gcx, gcy = P.SCR_CX, P.SCR_CY
    rw = P.SCR_MOD_W + 2*P.SCR_RECESS_CLR
    rl = P.SCR_MOD_H + 2*P.SCR_RECESS_CLR
    h  = LID_UNDER - Z_LEDGE
    ledge = Part.makeBox(rw, rl, h, Vector(gcx-rw/2, gcy-rl/2, Z_LEDGE))
    glass = Part.makeBox(P.SCR_GLASS_W, P.SCR_GLASS_H, (TOTAL-Z_LEDGE)+2.0,
                         Vector(gcx-P.SCR_GLASS_W/2, gcy-P.SCR_GLASS_H/2, Z_LEDGE-0.5))
    return [ledge], [glass]

# ----------------------------------------------------------------- LID (front)
def build_lid():
    plate = rrect(OW, OL, P.TOP_T, R_OUT, (OX, OY, LID_UNDER))
    lip = ring(LIPW, LIPL, LIP_H, 1.2, max(0.1, R_IN-LIP_GAP), (LIPX0, LIPY0, LID_UNDER-LIP_H))
    lid = plate.fuse(lip)
    lid = lid.fuse(Part.makeCompound([snap_nib(s,c) for (s,c) in SNAP_TABS]))
    # Locating PEGS hanging from the lid underside -> drop into the board mounting
    # holes so the D-pad and mic self-align, then tape/press them to the lid.
    pegs = []
    for (bx,by) in P.DPAD_HOLES:
        pegs.append(cyl(P.DPAD_PEG_D, P.DPAD_PEG_H, (bx, by, LID_UNDER-P.DPAD_PEG_H)))
    for (bx,by) in P.MIC_PEGS:
        pegs.append(cyl(P.MIC_PEG_D, P.MIC_PEG_H, (bx, by, LID_UNDER-P.MIC_PEG_H)))
    lid = lid.fuse(Part.makeCompound(pegs))
    lid = fillet_perimeter(lid, TOTAL, TOP_FILLET)   # round top edge while it's still a clean face
    zc, hZ = LID_UNDER-1, P.TOP_T+2
    tools = []
    tools.append(cyl(P.NAV_D, hZ, (P.NAV_CX, P.NAV_CY, zc)))          # round hole over the 5-way knob
    tools += mic_grille(zc, hZ)                                        # INMP441 mic grille
    # flush screen mount: fuse ledge, then cut the glass opening + all holes
    adds, dcuts = screen_features()
    for a in adds: lid = lid.fuse(a)
    tools += dcuts
    lid = lid.cut(Part.makeCompound(tools))
    return lid.removeSplitter()

# ----------------------------------------------------------------- build + export
doc = App.newDocument("qurannode_enclosure")
tray = build_tray(); lid = build_lid()
tray = fillet_perimeter(tray, 0.0,   BOT_FILLET)   # lid top fillet done inside build_lid()

t = doc.addObject("Part::Feature", "Tray"); t.Shape = tray
l = doc.addObject("Part::Feature", "Lid");  l.Shape = lid
doc.recompute()
doc.saveAs(os.path.join(HERE, "qurannode_enclosure.FCStd"))
Part.export([t, l], os.path.join(HERE, "qurannode_enclosure.step"))
Part.export([t], os.path.join(HERE, "tray.step"))
Part.export([l], os.path.join(HERE, "lid.step"))
try:
    import Mesh
    Mesh.export([t], os.path.join(HERE, "tray.stl"))
    Mesh.export([l], os.path.join(HERE, "lid.stl"))
    print("Exported STL: tray.stl, lid.stl")
except Exception as e:
    print("  ! STL export skipped: %s" % e)

_ok = "OK" if (len(tray.Solids)==1 and len(lid.Solids)==1) else "!! MULTI-SOLID"
print("SOLIDITY %s" % _ok)
print("TRAY solids=%d valid=%s" % (len(tray.Solids), tray.isValid()))
print("LID  solids=%d valid=%s" % (len(lid.Solids),  lid.isValid()))
print("Overall: %.1f x %.1f x %.1f mm" % (OW, OL, TOTAL))
print("Screen glass flush @ z=%.1f, module seat @ z=%.1f" % (TOTAL, Z_LEDGE))
print("Screen window: %.1f x %.1f mm (320px x 480px sides)" % (P.SCR_GLASS_W, P.SCR_GLASS_H))
print("Battery pocket %sx%sx%s mm  ~%d mAh" % (P.BATT["w"],P.BATT["h"],P.BATT["t"], round(P.batt_mah())))
