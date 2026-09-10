"""QuranNode handheld enclosure -- SINGLE SOURCE OF TRUTH for all dimensions.
Imported by both gen_panelplan.py (fast Pillow preview) and freecad_export.py
(the canonical B-rep builder). Edit here, regenerate both.

Coordinate system (device-local, millimetres):
    X = width   0 .. IW   (0 = left wall inner face)
    Y = height  0 .. IH   (0 = BOTTOM edge, IH = TOP edge)  -- Y is UP
    Z = through-thickness  0 = tray outer floor (BACK), +Z toward the LID (FRONT)

Battery = EEMB LP603449 (51 x 34.5 x 6.3 mm, 1100 mAh, JST-PHR-02), taped to the
floor (no corral).

Rev F -- SHORTER shell (OL 184.8 -> 156.0). The Rev E "middle battery zone" cost
~35-51 mm of length; Rev F reclaims it by re-packing into a bit more THICKNESS
(inner depth 13 -> 15 mm, overall 15.8 -> 17.8 mm, still < 18):
  CHIN (below screen): PCM5102 DAC (bottom, jack out) + battery rotated LANDSCAPE
                       (51 wide x 34.5 tall -- saves 16.5 mm of length vs portrait)
  BEHIND SCREEN      : ESP32 (left) | speaker + TPS regulator (right, stacked)
  TOP EDGE           : microSD + USB-C charger + power switch, pulled down
Screen window enlarged (56x86.5 -> 57x88) for a bit more glass, same module.
"""

# ----------------------------------------------------------------- shell
IW, IH        = 64.0, 151.2     # inner cavity  width x height (Rev F: shorter)
INNER_D       = 15.0            # inner cavity depth (Rev F: +2 for behind-screen stack)
FLOOR_T       = 1.4             # back skin (tray floor)
TOP_T         = 1.4            # front skin (lid)
WALL          = 2.4            # perimeter wall thickness
R_OUT         = 5.0            # outer corner radius
CLR           = 0.35          # part-to-part clearance (lid lip vs tray wall)

# derived outer envelope
OW  = IW + 2*WALL             # 68.8
OL  = IH + 2*WALL             # 156.0
TOTAL_D = INNER_D + FLOOR_T + TOP_T   # 17.8  (overall thickness)

# ----------------------------------------------------------------- SCREEN
# 3.5" 480x320 ST7796S SPI cap-touch module ("Openslive Yosek" V1.0), near the top.
SCR_CX, SCR_CY   = 32.0, 104.0       # module + window centre (Rev F: high, near top)
SCR_MOD_W, SCR_MOD_H = 55.6, 91.0    # module PCB / glass outline (PCB == glass width, MEASURED)
SCR_ACT_W, SCR_ACT_H = 49.0, 74.0    # active (lit) glass area (=320px x 480px, inset in the 55.6 glass)
SCR_GLASS_W, SCR_GLASS_H = 54.0, 88.0  # lid window opening -- < MOD_W so the lip catches the module
                                       # (0.8 mm lip/side onto the glass; still clears the 49 mm active)
SCR_BODY_T       = 4.3               # glass-top -> module back
SCR_GLASS_STACK  = 3.0               # glass-top -> module PCB front (flush-mount seat depth)
SCR_RECESS_CLR   = 1.0               # per-side clearance for the module-nesting rabbet

# ----------------------------------------------------------------- D-PAD (nav board)
# 40.7 x 24.8 mm landscape 5-way switch breakout (MEASURED). Mounts FLAT against the
# lid (component side up, switch through the NAV opening; tape to retain, no pegs).
# The 5-way switch is a 9.9 mm SQUARE body, 3.9 mm tall, ~centred on the board (15.8/
# 13.5 mm from the ends along the long axis, 7.1 mm each side). Its body noses UP
# THROUGH the lid via a SQUARE NAV opening so the PCB seats close to the inner face;
# the actuator protrudes to press/tilt. SET/RST buttons stay covered.
DPAD_W, DPAD_H     = 40.7, 24.8      # board outline (X, Y) -- landscape (MEASURED)
DPAD_CX, DPAD_CY   = 32.0, 27.0      # board centre (switch ~centred -> board centres on NAV)
NAV_CX, NAV_CY     = 32.0, 27.0      # 5-way switch centre
NAV_SW             = 9.9             # switch BODY, square (MEASURED)
NAV_SW_H           = 3.9             # switch total height off the PCB (MEASURED)
NAV_SW_CLR         = 0.25            # per-side clearance -> square NAV opening = 10.4 mm
NAV_D              = 9.5             # actuator/knob dia (protrudes through the opening) [VERIFY]
# SET/RST tactiles are COVERED (no lid holes) -- only the 5-way D-pad is exposed.
# (D-pad locating pegs removed -- the switch body in the NAV opening locates it.)

# ----------------------------------------------------------------- MICROPHONE (INMP441)
MIC_CX, MIC_CY     = 10.0, 48.0      # board + grille centre -- FAR LEFT, just above the D-pad
MIC_BOARD          = 14.0            # square board edge
MIC_GRILLE_D       = 6.0             # grille pattern envelope
MIC_HOLE_D         = 1.1             # individual port holes
# (no locating pegs -- board sits under the grille and is taped to the lid)

# ----------------------------------------------------------------- POWER SWITCH (on/off)
# 7x7x16mm self-locking push switch. SIDE-mounted: plunger out the LEFT edge, upper.
PWR_POS     = 140.0    # Y on the left edge (top group, Rev F: pulled down)
PWR_BODY    = 7.0      # switch body cross-section (square)
PWR_HOLE    = 6.5      # plunger hole (square) in the left wall
PWR_RIB_T   = 1.2      # cradle rib thickness
PWR_RIB_H   = 7.2      # cradle rib height (~body)
PWR_RIB_LEN = 9.0      # cradle rib length into the device (X)

# ----------------------------------------------------------------- SPEAKER (back grille)
SPK_CX, SPK_CY   = 48.0, 135.0       # 28 mm driver, back-firing, TOP-right (charger moved to bottom)
SPK_D            = 28.0
SPK_T            = 5.5
SPK_GRILLE_D     = 26.0
SPK_SLOT_W, SPK_PITCH = 1.8, 3.4

# ----------------------------------------------------------------- EDGE PORTS
# (edge, centre-coord-along-that-edge, width, height, [zrel]) ; zrel = connector
# centre-line above the floor (default 3.2). HP raised +1.5 -> 4.7.
PORT_USB   = dict(edge="B", pos=46.0, w=9.5, h=3.6)            # TP4056 USB-C, BOTTOM edge (right, beside HP)
PORT_HP    = dict(edge="B", pos=11.0, w=6.6, h=6.6, zrel=4.7)  # PCM5102 jack, BOTTOM edge, toward LEFT (raised 1.5) [VERIFY jack X]
PORT_SD    = dict(edge="T", pos=23.0, w=12.0, h=2.2)           # microSD slot, TOP edge (left of speaker)

# ----------------------------------------------------------------- INTERNAL COMPONENTS
# Mock footprints for the panel plan / clearance check (X-centre, Y-centre, W, H).
BATT  = dict(cx=30.0, cy=39.0, w=51.0, h=34.5, t=6.3)   # EEMB LP603449, 1100 mAh -- CHIN, LANDSCAPE
TPS   = dict(cx=48.0, cy=90.0, w=26.0, h=18.0)          # TPS63020 buck-boost, behind screen (right, below spk)
ESP32 = dict(cx=16.0, cy=101.0, w=28.0, h=63.0)         # vertical, left, behind screen
DAC   = dict(cx=18.0, cy=11.0,  w=32.0, h=17.0)         # BOTTOM-LEFT, LANDSCAPE, jack out the bottom (long-edge jack) [VERIFY]
SD    = dict(cx=23.0, cy=142.0, w=17.9, h=17.9)         # TOP, slot out the top edge (left of speaker)
CHG   = dict(cx=49.0, cy=11.0,  w=26.0, h=17.0)         # BOTTOM-RIGHT beside DAC, USB-C out the bottom edge

# component through-thickness heights (for the stack/thickness report)
T_BOARD_ESP = 4.8    # PCB + USB-C connector height
T_BOARD_SM  = 3.5    # small boards (DAC/charger/SD)
T_DPAD      = 2.7    # dpad intrusion: PCB + the 3.9 mm switch noses into the NAV opening
                     # (actuator ~1 mm proud); depends on how proud you set the knob [VERIFY]

def batt_mah():
    return 1100        # EEMB LP603449, rated
