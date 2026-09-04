#!/usr/bin/env python3
"""Generate pinout.svg for the QuranNode custom ESP32-S3 prototype."""
import os

# group -> color
COL = {
    "disp": "#4aa3ff", "tch": "#2fc7c7", "dac": "#ff8c42", "mic": "#c07cff",
    "sd": "#ffcf40", "sw": "#4ade80", "pwr": "#ff5a5a", "gnd": "#888e99",
    "psram": "#933", "sys": "#5a6270",
}
LEGEND = [
    ("disp", "ST7796S display (SPI)"), ("tch", "Capacitive touch (I2C)"),
    ("dac", "PCM5102 DAC (I2S out)"), ("mic", "INMP441 mic (I2S in)"),
    ("sd", "microSD (SPI)"), ("sw", "5-way switch"),
    ("pwr", "Power"), ("gnd", "Ground"),
    ("psram", "PSRAM - do not use"), ("sys", "USB/UART/strap/RGB - reserved"),
]

# (pin label, function label, group). Physical order, left->right.
TOP = [
    ("GND","GND","gnd"),("43","TX (console)","sys"),("44","RX (console)","sys"),
    ("1","SD CS","sd"),("2","SD MOSI","sd"),("42","SD SCK","sd"),("41","SD MISO","sd"),
    ("40","SW UP","sw"),("39","SW DOWN","sw"),("38","SW LEFT","sw"),
    ("37","PSRAM","psram"),("36","PSRAM","psram"),("35","PSRAM","psram"),
    ("0","BOOT","sys"),("45","strap","sys"),("48","RGB LED","sys"),
    ("47","SW RIGHT","sw"),("21","SW MID","sw"),
    ("20","USB D+","sys"),("19","USB D-","sys"),("GND","GND","gnd"),
]
BOT = [
    ("3V3","3V3","pwr"),("3V3","3V3","pwr"),("RST","RST","sys"),
    ("4","DAC BCK","dac"),("5","DAC LRCK","dac"),("6","DAC DIN","dac"),
    ("7","MIC SCK","mic"),("15","DISP BL","disp"),
    ("16","TOUCH SDA","tch"),("17","TOUCH SCL","tch"),("18","TOUCH INT","tch"),
    ("8","MIC WS","mic"),("3","strap","sys"),("46","strap","sys"),("9","MIC SD","mic"),
    ("10","DISP CS","disp"),("11","DISP MOSI","disp"),("12","DISP SCK","disp"),
    ("13","DISP DC","disp"),("14","DISP RST","disp"),("5V","5V","pwr"),("GND","GND","gnd"),
]

CW, X0 = 58, 40
n = max(len(TOP), len(BOT))
W = X0 * 2 + n * CW
H = 760
top_y, bot_y = 300, 372   # pin-cell rows (board edges)

def cell(x, y, pin, group, label, above):
    c = COL[group]
    s = []
    s.append(f'<rect x="{x+4}" y="{y}" width="{CW-8}" height="40" rx="5" '
             f'fill="#12151c" stroke="{c}" stroke-width="2"/>')
    s.append(f'<text x="{x+CW/2}" y="{y+26}" font-size="16" font-weight="bold" '
             f'fill="{c}" text-anchor="middle" font-family="monospace">{pin}</text>')
    # rotated function label
    lx, ly = x + CW/2, (y - 8 if above else y + 48)
    anchor = "start" if not above else "end"
    s.append(f'<text x="{lx}" y="{ly}" font-size="13" fill="#c9d2de" '
             f'font-family="sans-serif" text-anchor="{anchor}" '
             f'transform="rotate(-60 {lx} {ly})">{label}</text>')
    return "".join(s)

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
       f'viewBox="0 0 {W} {H}" font-family="sans-serif">']
svg.append(f'<rect width="{W}" height="{H}" fill="#070a0f"/>')
svg.append(f'<text x="{W/2}" y="40" font-size="26" font-weight="bold" fill="#96b4f5" '
           f'text-anchor="middle">QuranNode prototype — ESP32-S3-N16R8 pinout</text>')

# board body between the pin rows
svg.append(f'<rect x="{X0-6}" y="{top_y+40}" width="{n*CW+12}" height="{bot_y-top_y-40}" '
           f'rx="6" fill="#0e1219" stroke="#2b3342" stroke-width="2"/>')
svg.append(f'<text x="{W/2}" y="{(top_y+bot_y)/2+30}" font-size="20" '
           f'fill="#46506a" text-anchor="middle" font-family="monospace" '
           f'font-weight="bold">ESP32-S3  ·  N16R8  ·  16MB / 8MB PSRAM</text>')

for i, (pin, lab, grp) in enumerate(TOP):
    svg.append(cell(X0 + i * CW, top_y, pin, grp, lab, above=True))
for i, (pin, lab, grp) in enumerate(BOT):
    svg.append(cell(X0 + i * CW, bot_y, pin, grp, lab, above=False))

# legend
lx, ly = X0, 560
svg.append(f'<text x="{lx}" y="{ly-14}" font-size="16" font-weight="bold" '
           f'fill="#c9d2de">Legend</text>')
for i, (grp, name) in enumerate(LEGEND):
    col = i % 2
    row = i // 2
    ex = lx + col * 340
    ey = ly + row * 30
    svg.append(f'<rect x="{ex}" y="{ey-12}" width="16" height="16" rx="3" fill="{COL[grp]}"/>')
    svg.append(f'<text x="{ex+24}" y="{ey+1}" font-size="14" fill="#c9d2de">{name}</text>')

# jumper reminders
jx = lx + 720
svg.append(f'<text x="{jx}" y="{ly-14}" font-size="16" font-weight="bold" fill="#ffcf40">Board jumpers</text>')
for i, t in enumerate([
    "PCM5102:  SCK -> GND  (internal PLL)",
    "PCM5102:  XSMT -> 3V3  (un-mute)",
    "PCM5102:  VIN -> 5V   INMP441: VDD -> 3V3",
    "INMP441:  L/R -> GND   5-way: COM -> GND",
    "Touch RST -> display RST (14) or 3V3",
]):
    svg.append(f'<text x="{jx}" y="{ly+18+i*26}" font-size="13" fill="#c9d2de" '
               f'font-family="monospace">{t}</text>')

svg.append("</svg>")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pinout.svg")
open(out, "w").write("\n".join(svg))
print("wrote", out)
