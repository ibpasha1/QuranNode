#!/usr/bin/env python3
"""Isometric renders of the QuranNode enclosure (tray + lid). Pure Python + Pillow:
parses binary STLs, rasterises with a per-pixel z-buffer + backface culling.
Emits an assembled hero shot (front) and an exploded view. Run after freecad_export.py."""
import struct, math, os
from PIL import Image
import params as P

HERE = os.path.dirname(os.path.abspath(__file__))
TRAY = (74, 79, 88)          # tray (back/walls) -> holes read dark
CASE = (120, 126, 136)       # lid (front)
SCRN = (10, 32, 26)          # screen glass
ACC  = (90, 150, 210)        # nav cap (accent)

def load_stl(path):
    with open(path, "rb") as f:
        f.read(80); (n,) = struct.unpack("<I", f.read(4)); out = []
        for _ in range(n):
            dd = struct.unpack("<12f", f.read(48)); f.read(2)
            out.append((dd[0:3], [dd[3:6], dd[6:9], dd[9:12]]))
        return out

def gather(offsets, show_screen):
    tris = []
    for (fn, col), off in zip([("tray.stl", TRAY), ("lid.stl", CASE)],
                              [offsets[0], offsets[1]]):
        p = os.path.join(HERE, fn)
        if not os.path.exists(p): continue
        for nrm, v in load_stl(p):
            tris.append((nrm, [(x, y, z+off) for (x, y, z) in v], col))
    if show_screen and offsets[1] == 0:      # faux glass just below lid top (closed view)
        cx, cy = P.SCR_CX, P.SCR_CY
        hw, hh, z = P.SCR_GLASS_W/2-0.6, P.SCR_GLASS_H/2-0.6, P.TOTAL_D-0.2
        quad = [(cx-hw,cy-hh,z),(cx+hw,cy-hh,z),(cx+hw,cy+hh,z),(cx-hw,cy+hh,z)]
        tris.append(((0,0,1),[quad[0],quad[1],quad[2]],SCRN))
        tris.append(((0,0,1),[quad[0],quad[2],quad[3]],SCRN))
    return tris

L = (-0.30,-0.42,0.86); Lm = math.sqrt(sum(c*c for c in L)); L=[c/Lm for c in L]
def shade(nrm, base):
    m = math.sqrt(sum(c*c for c in nrm)) or 1.0
    diff = max(0.0, sum((nrm[i]/m)*L[i] for i in range(3)))
    k = 0.30 + 0.70*diff
    return tuple(min(255,int(c*k)) for c in base)

def render(offsets, outname, az_deg, ax_deg, show_screen):
    tris = gather(offsets, show_screen)
    az, ax = math.radians(az_deg), math.radians(ax_deg)
    ca,sa = math.cos(az),math.sin(az); cb,sb = math.cos(ax),math.sin(ax)
    # centre model on origin first (so rotation is about its middle)
    mx, my, mz = P.IW/2, P.IH/2, P.TOTAL_D/2
    def rot(p, isnrm=False):
        x,y,z = p
        if not isnrm: x,y,z = x-mx, y-my, z-mz
        x1 = x*ca - y*sa; y1 = x*sa + y*ca
        return (x1, y1*cb - z*sb, y1*sb + z*cb)
    prepared=[]; xs=[]; ys=[]
    for nrm,v,col in tris:
        rn = rot(nrm, True)
        if rn[1] >= 0: continue
        rv=[rot(p) for p in v]
        prepared.append((rv, shade(nrm,col)))
        for r in rv: xs.append(r[0]); ys.append(r[2])
    minx,maxx,miny,maxy = min(xs),max(xs),min(ys),max(ys)
    W,H,PAD = 900, 1180, 80
    sc = min((W-2*PAD)/(maxx-minx),(H-2*PAD)/(maxy-miny))
    ox = (W-(maxx-minx)*sc)/2 - minx*sc
    oy = (H-(maxy-miny)*sc)/2 - miny*sc
    def px(r): return (r[0]*sc+ox, H-(r[2]*sc+oy), r[1])
    bg=(13,15,18); pix=bytearray(bg*(W*H)); zbuf=[1e30]*(W*H)
    for rv,col in prepared:
        a,b,c = px(rv[0]),px(rv[1]),px(rv[2])
        minX=max(0,int(min(a[0],b[0],c[0]))); maxX=min(W-1,int(max(a[0],b[0],c[0])+1))
        minY=max(0,int(min(a[1],b[1],c[1]))); maxY=min(H-1,int(max(a[1],b[1],c[1])+1))
        if minX>maxX or minY>maxY: continue
        dd=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        if abs(dd)<1e-9: continue
        cr,cg,cbl=col
        for yy in range(minY,maxY+1):
            row=yy*W
            for xx in range(minX,maxX+1):
                w0=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/dd
                w1=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/dd
                w2=1-w0-w1
                if w0<-0.002 or w1<-0.002 or w2<-0.002: continue
                z=w0*a[2]+w1*b[2]+w2*c[2]; i=row+xx
                if z<zbuf[i]:
                    zbuf[i]=z; j=i*3; pix[j]=cr; pix[j+1]=cg; pix[j+2]=cbl
    img=Image.frombytes("RGB",(W,H),bytes(pix))
    out=os.path.join(HERE,outname); img.save(out); print("wrote",out,img.size)

# front hero (looking at the lid/screen), and exploded (lid lifted off)
render((0,0),   "render_front.png",    18, 32, True)
render((0,60),  "render_exploded.png", 22, 40, False)
