#!/usr/bin/env python3
"""Generate JLCPCB assembly files (CPL + BOM) straight from qurannode.kicad_pcb.

Coordinate transform verified against the shipped gerbers:
    gerber/CPL X =  board_X
    gerber/CPL Y = -board_Y     (KiCad plots page origin, Y-up)
Rotation is emitted as the KiCad footprint orientation (CCW+). JLCPCB's part
library 0deg orientation can differ per package, so polarised / multi-pin parts
MUST be eyeballed in JLCPCB's alignment preview (pin-1 dot) and corrected there.
"""
import re, csv, sys

PCB = 'qurannode.kicad_pcb'

# ---- value + KiCad-footprint  ->  (LCSC, jlc-footprint-name, verify?) --------
# LCSC numbers taken from hardware/pcb/BOM.csv and matched by value/footprint.
# 'V' = needs verification (footprint or MPN differs from the design BOM, or the
# BOM had no line for this value).
# All codes below VERIFIED against live LCSC/JLCPCB (2026-09-24). See ASSEMBLY-NOTES.md
# for stock + package-mismatch flags. Format: value -> (LCSC, jlc-footprint, verify-flag)
MAP = {
    # passives (value keyed; 0402/0603/0805 from footprint) -- generic codes confirmed
    # by JLCPCB's own auto-match; 5k1/4k7/560k CORRECTED from bad guesses.
    ('5k1','0402'):  ('C25905','R0402',''),    # was C99257 (a BUFFER IC) -- fixed
    ('1k2','0402'):  ('C25749','R0402',''),
    ('10k','0402'):  ('C25744','R0402',''),
    ('1k','0402'):   ('C11702','R0402',''),
    ('100R','0402'): ('C25076','R0402',''),
    ('100k','0402'): ('C25741','R0402',''),
    ('560k','0402'): ('C227139','R0402',''),   # was C118428 (no match) -- YAGEO, low stock 8k
    ('20k','0402'):  ('C25765','R0402',''),
    ('4k7','0402'):  ('C25900','R0402',''),     # was C51721 (matched 3.9k) -- fixed
    ('100n','0402'): ('C1525','C0402',''),
    ('1u','0402'):   ('C52923','C0402',''),
    ('470n','0402'): ('C471404','C0402',''),    # Murata GRM155 X7R 16V, in stock
    ('2u2','0603'):  ('C23630','C0603',''),
    ('10u','0805'):  ('C15850','C0805',''),
    ('22u','0805'):  ('C45783','C0805',''),
    ('600R','0603'): ('C97000','L0603',''),     # 600R@100MHz 1.3A 0603; was C1015 (100R 0805)
    # semis / ICs (value keyed)
    ('BAT54',None):  ('C86299','SOD-323',''),   # BAT54J single, ST, SOD-323, Basic
    ('red',None):    ('C2286','LED0603',''),
    ('grn',None):    ('C12624','LED0603',''),   # was C72043 (1 in stock) -> KT-0603G Basic
    ('1u5',None):    ('C408334','MWSA0402S',''),  # 1.5uH Isat5.5A; custom land matches C408334
    # MIC: INMP441 (C5438445) is out of stock. Replacing with ICS-43434 (C5656610,
    # ~4.3k stock) like the DSP-Mini board -- pin-compat I2S but SMALLER footprint
    # (3.5x2.65mm), so MK1 land must change to InvenSense_ICS-43434-6 on respin.
    ('ICS-43434',None):('C5656610','ICS-43434-6_3.5x2.65',''),  # footprint now matches part
    ('2N7002',None): ('C8545','SOT-23',''),
    ('2N7002_NFET',None):('C8545','SOT-23',''),
    ('AO3401A_PFET',None):('C15127','SOT-23',''),
    ('ESP32-S3-WROOM-1',None):('C2913202','ESP32-S3-WROOM-1',''),
    ('PCM5102A',None):('C107671','TSSOP-20',''),   # was C107672 (an ADuM1201 isolator) -- fixed
    ('PAM8302A',None):('C113367','MSOP-8',''),     # land fixed to MSOP-8 to match C113367
    ('TP4056',None): ('C382139','SOIC-8-EP',''),   # ESOP-8 == SOIC-8-1EP land, Basic
    ('TPS63020',None):('C15483','VSON-14-DSJ',''), # custom 0.5mm DSJ footprint added to match
    ('USBLC6-2SC6',None):('C7519','SOT-23-6',''),
    ('DW01A',None):  ('C351410','SOT-23-6',''),
    ('FS8205A',None):('C908265','SOT-23-6',''),    # was C32254 (0 stock) -> C908265 in stock
    ('RST',None):      ('C318884','SW_TL3342','V'), # TS-1187A 5.1x5.1; verify vs TL3342 land
    ('BOOT',None):     ('C318884','SW_TL3342','V'),
    # SMD connectors -- machine-placed; codes are exact footprint matches (verified 2026-09-24)
    ('USB-C-16P',None):('C319148','USB-C-16P',''),      # XKB U262-161N-4BVC11, JLC Basic ~38k
    ('microSD',None):  ('C114218','microSD-DM3AT','V'), # Hirose DM3AT (confirm JLC-assembly SKU)
    ('TRS_3.5',None):  ('C2939583','PJ-31060',''),      # HOOYA PJ-31060, Standard-only
    # --- hand-soldered parts below are EXCLUDED from the JLC BOM/CPL (see EXCLUDE) ---
}

# Hand-populated, EXCLUDED from the JLC assembly BOM/CPL:
#  - J3 LCD header, J4/J5 JST: THROUGH-HOLE, cannot be reflowed.
#  - SW1 nav: custom land, no in-stock SMT part.
#  - SW2/SW3 RST/BOOT: lid-covered recovery buttons on an E-Switch TL3342 6mm land
#    (the in-stock TS-1187A is 5.1mm and does not fit); hand-populate a TL3342-compatible.
# The SMD connectors (J1 USB-C, J2 uSD, J6 jack) + the module (U1) ARE machine-placed
# with exact-footprint-match in-stock parts (verified 2026-09-24) -- see MAP.
EXCLUDE = {'J3','J4','J5','SW1','SW2','SW3'}
THT  = set()  # filled from parse (attr through_hole)

def blocks(s, tag):
    key='('+tag; i=0; n=len(s); out=[]
    while True:
        j=s.find(key,i)
        if j<0: break
        if s[j+len(key)] not in ' \t\n(': i=j+len(key); continue
        depth=0;k=j
        while k<n:
            c=s[k]
            if c=='(':depth+=1
            elif c==')':
                depth-=1
                if depth==0:break
            k+=1
        out.append(s[j:k+1]); i=k+1
    return out

def fp_at(blk):
    depth=0;i=0;n=len(blk)
    while i<n:
        if blk[i]=='(':
            depth+=1
            if depth==2 and blk[i:i+3]=='(at':
                m=re.match(r'\(at\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?',blk[i:])
                if m: return float(m.group(1)),float(m.group(2)),float(m.group(3) or 0)
        elif blk[i]==')': depth-=1
        i+=1
    return None

def prop(b,name):
    m=re.search(r'\(property\s+"%s"\s+"([^"]*)"'%re.escape(name),b); return m.group(1) if m else ''
def libid(b):
    m=re.match(r'\(footprint\s+"([^"]+)"',b); return m.group(1) if m else ''

def size_of(lib):
    for s in ('0402','0603','0805','1206'):
        if s in lib: return s
    return None

src=open(PCB).read()
parts=[]; excluded=[]
for b in blocks(src,'footprint'):
    ref=prop(b,'Reference'); val=prop(b,'Value'); lid=libid(b); at=fp_at(b)
    if not ref or at is None: continue
    if '(attr through_hole' in b: THT.add(ref)
    lib=lid.split(':')[-1]
    if ref in EXCLUDE:
        excluded.append((ref,val,lib,at)); continue
    parts.append((ref,val,lib,at))

def lookup(val,lib):
    sz=size_of(lib)
    if (val,sz) in MAP: return MAP[(val,sz)]
    if (val,None) in MAP: return MAP[(val,None)]
    return ('','?','V')

def refnum(r):
    m=re.match(r'([A-Za-z]+)(\d+)',r); return (m.group(1),int(m.group(2))) if m else (r,0)

parts.sort(key=lambda p:refnum(p[0]))

# ---- CPL ---------------------------------------------------------------------
with open('fab/qurannode-cpl.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['Designator','Mid X','Mid Y','Layer','Rotation'])
    for ref,val,lib,at in parts:
        w.writerow([ref, f'{at[0]:.4f}', f'{-at[1]:.4f}', 'Top', f'{at[2]:.0f}'])
print(f'CPL : fab/qurannode-cpl.csv  ({len(parts)} SMT placements, all Top; {len(excluded)} hand parts excluded)')

# ---- BOM (one line per LCSC part; JLCPCB import format) ----------------------
# Group by LCSC # so parts with the same C# but different value strings (e.g. the
# 2N7002 tagged "2N7002" vs "2N7002_NFET") become ONE line -- JLCPCB rejects two BOM
# lines matched to the same part. Blank-LCSC lines fall back to (value,footprint).
def clean(val):  # tidy the display comment: drop _NFET/_PFET-style tags
    return re.sub(r'_(N|P)FET$','',val)
groups={}   # key -> {'refs':[], 'comment':str, 'fp':str, 'lcsc':str}
issues=[]
for ref,val,lib,at in parts:
    lcsc,fp,verify=lookup(val,lib)
    key=lcsc if lcsc else ('',clean(val),fp)
    g=groups.setdefault(key,{'refs':[],'comment':clean(val),'fp':fp,'lcsc':lcsc})
    g['refs'].append(ref)
    if verify or not lcsc:
        issues.append((ref,val,lib,lcsc,verify))

with open('fab/qurannode-bom-jlc.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['Comment','Designator','Footprint','LCSC Part #'])
    for g in sorted(groups.values(), key=lambda g: refnum(sorted(g['refs'],key=refnum)[0])):
        refs_sorted=sorted(g['refs'],key=refnum)
        w.writerow([g['comment'], ','.join(refs_sorted), g['fp'], g['lcsc']])
print(f'BOM : fab/qurannode-bom-jlc.csv  ({len(groups)} lines)')

# ---- report ------------------------------------------------------------------
print(f'\nExcluded (hand-solder, buy separately): {sorted((r for r,_,_,_ in excluded), key=refnum)}')
print('\n== NEEDS VERIFY / MISSING LCSC ==')
seen=set()
for ref,val,lib,lcsc,verify in issues:
    tag='MISSING LCSC' if not lcsc else 'verify fp/MPN'
    k=(val,lib)
    if k in seen: continue
    seen.add(k)
    ex=[r for r,v,l,_,_ in issues if v==val and l==lib]
    print(f'  {tag:13} {val:20.20} {lib:28.28} {lcsc or "-":8} refs={",".join(sorted(set(ex),key=refnum))}')
