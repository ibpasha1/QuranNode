"""QuranNode handheld enclosure -- SINGLE SOURCE OF TRUTH for all dimensions.
Imported by both gen_panelplan.py (fast Pillow preview) and freecad_export.py
(the canonical B-rep builder). Edit here, regenerate both.

Coordinate system (device-local, millimetres):
    X = width   0 .. IW   (0 = left wall inner face)
    Y = height  0 .. IH   (0 = BOTTOM edge, IH = TOP edge)  -- Y is UP
    Z = through-thickness  0 = tray outer floor (BACK), +Z toward the LID (FRONT)

Battery = EEMB LP603449 (51 x 34.5 x 6.3 mm, 1100 mAh, JST-PHR-02), taped to the
floor (no corral).

Rev E -- I/O CONSOLIDATED to free a clean middle battery zone (the tray was full
of boards+wiring with nowhere for the cell):
  TOP    : ESP32, speaker, microSD (slot out top), USB-C charger (out top), power
  BOTTOM : PCM5102 DAC only -- headphone jack out the bottom, raised 1.5 mm
  MIDDLE : battery (clear run)
Taller (IH 160->180); D-pad now mounts FLAT against the lid to save chin depth.
"""

# ----------------------------------------------------------------- shell
IW, IH        = 64.0, 180.0     # inner cavity  width x height (Rev E: taller)
INNER_D       = 13.0            # inner cavity depth
FLOOR_T       = 1.4             # back skin (tray floor)
TOP_T         = 1.4            # front skin (lid)
WALL          = 2.4            # perimeter wall thickness
R_OUT         = 5.0            # outer corner radius
CLR           = 0.35          # part-to-part clearance (lid lip vs tray wall)

# derived outer envelope
OW  = IW + 2*WALL             # 68.8
OL  = IH + 2*WALL             # 184.8
TOTAL_D = INNER_D + FLOOR_T + TOP_T   # 15.8  (overall thickness)

# ----------------------------------------------------------------- SCREEN
# 3.5" 480x320 ST7796S SPI cap-touch module ("Openslive Yosek" V1.0), near the top.
SCR_CX, SCR_CY   = 32.0, 131.5       # module + window centre
SCR_MOD_W, SCR_MOD_H = 59.0, 91.0    # module PCB recess (> glass -> lip) [VERIFY vs real PCB]
SCR_ACT_W, SCR_ACT_H = 49.0, 74.0    # active glass area  (=320px x 480px)
SCR_GLASS_W, SCR_GLASS_H = 56.0, 86.5  # lid window opening
SCR_BODY_T       = 4.3               # glass-top -> module back
SCR_GLASS_STACK  = 3.0               # glass-top -> module PCB front (flush-mount seat depth)
SCR_RECESS_CLR   = 1.0               # per-side clearance for the module-nesting rabbet

# ----------------------------------------------------------------- D-PAD (nav board)
# 41 x 25 mm landscape 5-way switch breakout. Rev E: mounts FLAT against the lid
# (board's component side up, switch through the NAV hole; short locating pegs).
DPAD_W, DPAD_H     = 41.0, 25.0      # board outline (X, Y) -- landscape
DPAD_CX, DPAD_CY   = 33.5, 42.0      # board centre (nav lands on X=32)
NAV_CX, NAV_CY     = 32.0, 42.0      # 5-way switch centre
NAV_D              = 9.5             # round hole exposing the switch knob (tilts N/E/S/W) [VERIFY]
# SET/RST tactiles are COVERED (no lid holes) -- only the 5-way D-pad is exposed.
DPAD_HOLES = [(24.0,33.5),(24.0,50.5),(50.0,33.5),(50.0,50.5)]  # lid locating pegs
DPAD_PEG_D, DPAD_PEG_H = 2.8, 1.5    # SHORT pegs -> board lays flat against the lid

# ----------------------------------------------------------------- MICROPHONE (INMP441)
MIC_CX, MIC_CY     = 32.0, 71.0      # board + earpiece grille centre (screen<->D-pad gap)
MIC_BOARD          = 14.0            # square board edge
MIC_GRILLE_D       = 6.0             # grille pattern envelope
MIC_HOLE_D         = 1.1             # individual port holes
MIC_PEG_D, MIC_PEG_H = 2.4, 2.5      # 2 locating pegs (board mounting holes)
MIC_PEGS = [(25.0,71.0),(39.0,71.0)]  # [VERIFY against the round board's holes]

# ----------------------------------------------------------------- POWER SWITCH (on/off)
# 7x7x16mm self-locking push switch. SIDE-mounted: plunger out the LEFT edge, upper.
PWR_POS     = 162.0    # Y on the left edge (top group)
PWR_BODY    = 7.0      # switch body cross-section (square)
PWR_HOLE    = 6.5      # plunger hole (square) in the left wall
PWR_RIB_T   = 1.2      # cradle rib thickness
PWR_RIB_H   = 7.2      # cradle rib height (~body)
PWR_RIB_LEN = 9.0      # cradle rib length into the device (X)

# ----------------------------------------------------------------- SPEAKER (back grille)
SPK_CX, SPK_CY   = 48.0, 140.0       # 28 mm driver, back-firing, TOP-right (raised)
SPK_D            = 28.0
SPK_T            = 5.5
SPK_GRILLE_D     = 26.0
SPK_SLOT_W, SPK_PITCH = 1.8, 3.4

# ----------------------------------------------------------------- EDGE PORTS
# (edge, centre-coord-along-that-edge, width, height, [zrel]) ; zrel = connector
# centre-line above the floor (default 3.2). HP raised +1.5 -> 4.7.
PORT_USB   = dict(edge="T", pos=51.0, w=9.5, h=3.6)            # TP4056 USB-C, TOP edge (right)
PORT_HP    = dict(edge="B", pos=22.0, w=6.6, h=6.6, zrel=4.7)  # PCM5102 jack, BOTTOM edge, toward LEFT (raised 1.5) [VERIFY jack X]
PORT_SD    = dict(edge="T", pos=28.0, w=12.0, h=2.2)           # microSD slot, TOP edge (middle)

# ----------------------------------------------------------------- INTERNAL COMPONENTS
# Mock footprints for the panel plan / clearance check (X-centre, Y-centre, W, H).
BATT  = dict(cx=19.0, cy=55.0, w=34.5, h=51.0, t=6.3)   # EEMB LP603449, 1100 mAh -- MIDDLE-LEFT
TPS   = dict(cx=50.0, cy=55.0, w=26.0, h=18.0)          # TPS63020 buck-boost regulator, right of battery
ESP32 = dict(cx=16.0, cy=121.5, w=28.0, h=63.0)         # vertical, top-left behind screen
DAC   = dict(cx=32.0, cy=11.0,  w=32.0, h=17.0)         # BOTTOM, LANDSCAPE, jack out the bottom (long-edge jack) [VERIFY]
SD    = dict(cx=28.0, cy=168.0, w=17.9, h=17.9)         # TOP-middle, slot out the top edge
CHG   = dict(cx=51.0, cy=167.5, w=26.0, h=17.0)         # TOP-right, USB-C out the top edge

# component through-thickness heights (for the stack/thickness report)
T_BOARD_ESP = 4.8    # PCB + USB-C connector height
T_BOARD_SM  = 3.5    # small boards (DAC/charger/SD)
T_DPAD      = 5.5    # dpad PCB + switch body/cap

def batt_mah():
    return 1100        # EEMB LP603449, rated
