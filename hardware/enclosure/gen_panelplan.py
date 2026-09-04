#!/usr/bin/env python3
"""Fast top-view panel plan of the QuranNode enclosure (FRONT + BACK faces),
drawn 1:1 with the device so the layout can be eyeballed without FreeCAD.
Pure Pillow. Run: python3 gen_panelplan.py"""
import os, math
from PIL import Image, ImageDraw, ImageFont
import params as P

HERE = os.path.dirname(os.path.abspath(__file__))
S   = 4.4                       # px per mm
MB  = 46                        # outer margin px
GAP = 60                        # gap between the two views
OWpx = int((P.OW + 8) * S)
Hpx  = int((P.OL + 8) * S) + 2*MB + 56
Wpx  = 2*OWpx + GAP + 2*MB

BG=(15,17,20); SLAB=(26,30,36); EDGE=(228,226,219); ACC=(96,202,250)
CUT=(12,14,17); GRN=(0,224,160); GRNF=(10,30,25); YEL=(255,204,0)
DIM=(150,156,164); COMP=(70,78,90); COMPF=(30,34,40); ORG=(255,150,70)
BATF=(40,32,20); BATE=(210,150,60); SPKC=(120,128,140)

img = Image.new("RGB",(Wpx,Hpx),BG); d = ImageDraw.Draw(img)
def font(sz):
    for p in ["/System/Library/Fonts/Supplemental/Arial Bold.ttf",
              "/System/Library/Fonts/Helvetica.ttc"]:
        if os.path.exists(p):
            try: return ImageFont.truetype(p, sz)
            except Exception: pass
    return ImageFont.load_default()
def ctext(x,y,t,col,sz):
    f=font(sz); bb=d.textbbox((0,0),t,font=f)
    d.text((x-(bb[2]-bb[0])/2, y-(bb[3]-bb[1])/2-bb[1]), t, fill=col, font=f)

class View:
    def __init__(self, x0, mirror):
        self.x0=x0; self.y0=MB+28; self.mirror=mirror   # mirror=back face (flip X)
    def px(self,x):
        xx = (P.IW-x) if self.mirror else x
        return self.x0 + (xx+ (P.WALL+4))*S
    def py(self,y): return self.y0 + (P.IH - y + (P.WALL+4))*S
    def rect(self,cx,cy,w,h,**kw):
        d.rectangle([self.px(cx)-w/2*S,self.py(cy)-h/2*S,self.px(cx)+w/2*S,self.py(cy)+h/2*S],**kw)
    def rrect(self,cx,cy,w,h,r,**kw):
        d.rounded_rectangle([self.px(cx)-w/2*S,self.py(cy)-h/2*S,self.px(cx)+w/2*S,self.py(cy)+h/2*S],radius=r,**kw)
    def circ(self,cx,cy,dia,**kw):
        d.ellipse([self.px(cx)-dia/2*S,self.py(cy)-dia/2*S,self.px(cx)+dia/2*S,self.py(cy)+dia/2*S],**kw)

def slab(v,title):
    # outer shell (rounded) + inner cavity
    d.rounded_rectangle([v.px(-P.WALL) if not v.mirror else v.px(P.IW+P.WALL),
                         v.py(P.IH+P.WALL),
                         v.px(P.IW+P.WALL) if not v.mirror else v.px(-P.WALL),
                         v.py(-P.WALL)],
                        radius=int(P.R_OUT*S), outline=EDGE, width=3, fill=SLAB)
    ctext((v.px(0)+v.px(P.IW))/2, MB-2, title, EDGE, 20)

fv = View(MB, False)                     # FRONT
bv = View(MB+OWpx+GAP, True)             # BACK
slab(fv, "FRONT  (lid)")
slab(bv, "BACK  (tray)")

# ---- FRONT: screen module outline + glass window + active area
fv.rect(P.SCR_CX,P.SCR_CY,P.SCR_MOD_W,P.SCR_MOD_H, outline=COMP, width=1)
fv.rrect(P.SCR_CX,P.SCR_CY,P.SCR_GLASS_W,P.SCR_GLASS_H,int(2*S), outline=GRN, fill=GRNF, width=3)
fv.rect(P.SCR_CX,P.SCR_CY,P.SCR_ACT_W,P.SCR_ACT_H, outline=(0,150,110), width=1)
ctext(fv.px(P.SCR_CX),fv.py(P.SCR_CY),'3.5"\n480x320\nST7796S',GRN,15)
# ---- FRONT: dpad board (41x25 landscape, faint) + nav cutout + SET/RST + pegs
fv.rect(P.DPAD_CX,P.DPAD_CY,P.DPAD_W,P.DPAD_H, outline=COMP, width=1)
for (hx,hy) in P.DPAD_HOLES: fv.circ(hx,hy,3.0, outline=COMP, width=1)
# nav: round hole exposing the 5-way switch knob directly
fv.circ(P.NAV_CX,P.NAV_CY,P.NAV_D, outline=ACC, fill=CUT, width=3)
ctext(fv.px(P.NAV_CX),fv.py(P.NAV_CY),"NAV",ACC,9)
# SET/RST tactiles are covered (no cutouts) -- D-pad only
# ---- FRONT: mic grille (earpiece cluster) between screen and dpad
fv.circ(P.MIC_CX,P.MIC_CY,P.MIC_BOARD, outline=COMP, width=1)
gx,gy = fv.px(P.MIC_CX), fv.py(P.MIC_CY); rr=P.MIC_GRILLE_D/2-P.MIC_HOLE_D
for dx,dy in [(0,0)]+[(rr*math.cos(math.radians(60*k)),rr*math.sin(math.radians(60*k))) for k in range(6)]:
    d.ellipse([gx+dx*S-P.MIC_HOLE_D/2*S, gy-dy*S-P.MIC_HOLE_D/2*S,
               gx+dx*S+P.MIC_HOLE_D/2*S, gy-dy*S+P.MIC_HOLE_D/2*S], fill=CUT, outline=ACC)
ctext(gx, gy-P.MIC_BOARD/2*S-7, "MIC (INMP441)", DIM, 9)

# ---- BACK: internal components (mock) so packing is visible through the tray
def comp(v,c,label,fill=COMPF,ec=COMP,tc=DIM):
    v.rrect(c["cx"],c["cy"],c["w"],c["h"],int(1.2*S), outline=ec, fill=fill, width=2)
    ctext(v.px(c["cx"]),v.py(c["cy"]),label,tc,12)
comp(bv,P.BATT,f'BATTERY\n{int(P.BATT["w"])}x{int(P.BATT["h"])}x{int(P.BATT["t"])}\n~{round(P.batt_mah())}mAh', fill=BATF, ec=BATE, tc=BATE)
comp(bv,P.ESP32,"ESP32-S3\n28x63")
comp(bv,P.TPS,"TPS63020\nbuck-boost")
comp(bv,P.DAC,"PCM5102\nDAC")
comp(bv,P.SD,"microSD")
comp(bv,P.CHG,"TP4056\ncharger")
# speaker grille (concentric slotted circle) on the BACK
bv.circ(P.SPK_CX,P.SPK_CY,P.SPK_D, outline=SPKC, width=1)
gx,gy = bv.px(P.SPK_CX), bv.py(P.SPK_CY); rr=P.SPK_GRILLE_D/2.0
n=int(P.SPK_GRILLE_D//P.SPK_PITCH)
for i in range(n):
    yy=(i-(n-1)/2.0)*P.SPK_PITCH
    half=(rr*rr-yy*yy);
    if half<=0: continue
    half=half**0.5
    d.rounded_rectangle([gx-half*S, gy+yy*S-P.SPK_SLOT_W/2*S, gx+half*S, gy+yy*S+P.SPK_SLOT_W/2*S],
                        radius=int(P.SPK_SLOT_W/2*S), fill=CUT, outline=SPKC, width=1)
ctext(gx, bv.py(P.SPK_CY-P.SPK_D/2-3), "SPEAKER", SPKC, 10)
# power switch body (left edge, upper) + cradle ribs
comp(bv, dict(cx=P.PWR_BODY/2, cy=P.PWR_POS, w=P.PWR_BODY, h=P.PWR_BODY), "PWR", tc=ORG, ec=ORG)

# ---- EDGE PORTS (draw on both views at the correct edge)
def port(v, pd, name):
    e=pd["edge"]; w=pd["w"]; h=pd["h"]
    if e=="B": cx,cy = pd["pos"], -P.WALL/2 ; ww,hh=w,P.WALL+2
    elif e=="T": cx,cy = pd["pos"], P.IH+P.WALL/2 ; ww,hh=w,P.WALL+2
    elif e=="R": cx,cy = P.IW+P.WALL/2, pd["pos"] ; ww,hh=P.WALL+2,w
    else: cx,cy = -P.WALL/2, pd["pos"]; ww,hh=P.WALL+2,w
    v.rect(cx,cy,ww,hh, outline=ORG, fill=CUT, width=2)
    lx,ly = v.px(cx), v.py(cy)
    if e=="B": ctext(lx,ly+14,name,ORG,10)
    elif e=="T": ctext(lx,ly-14,name,ORG,10)
    elif e=="R": ctext(lx+2,ly,name,ORG,10)
for v in (fv,bv):
    port(v,P.PORT_USB,"USB-C"); port(v,P.PORT_HP,"HP"); port(v,P.PORT_SD,"microSD")
    port(v,dict(edge="L",pos=P.PWR_POS,w=P.PWR_HOLE,h=P.PWR_HOLE),"PWR")

foot = (f"QuranNode  ·  {P.OW:.1f} x {P.OL:.1f} x {P.TOTAL_D:.1f} mm  ·  "
        f"3.5\" ST7796S portrait  ·  battery pocket ~{round(P.batt_mah())} mAh @ {P.BATT['t']:.0f} mm")
ctext(Wpx/2, Hpx-26, foot, EDGE, 17)
out=os.path.join(HERE,"panelplan.png"); img.save(out); print("wrote",out,img.size)
