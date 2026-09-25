#!/usr/bin/env python3
"""Generate a KiCad netlist for the QuranNode mainboard with SKiDL, from scratch-
defined parts (no local KiCad symbol libs needed). Footprints are strings KiCad
resolves on import.

    python3 gen_netlist.py   ->  qurannode.net   (KiCad: Pcbnew -> Import Netlist)

INTEGRITY NOTE
  - CONNECTIVITY (pin-function -> net) is faithful to ../netlist.md.
  - PASSIVE pin numbers (1,2 / A,K) are correct.
  - IC PIN NUMBERS are best-effort and **MUST be checked against each datasheet /
    the verified symbol you place** -- see PINOUT-VERIFY.md. A wrong pin number wires
    a net to the wrong pad. This netlist is a verified-*connectivity* scaffold, not a
    verified board.
"""
from skidl import Part, Pin, Net, TEMPLATE, SKIDL, generate_netlist
PAS = Pin.types.PASSIVE; PWR = Pin.types.PWRIN; OUT = Pin.types.OUTPUT
BI  = Pin.types.BIDIR;   INP = Pin.types.INPUT

# ---------------------------------------------------------------- nets
n = {name: Net(name) for name in [
    "+3V3","VSYS","VBAT","VBUS","GND","USB_DP","USB_DM",
    "I2S_BCK","I2S_WS","I2S_DATA","MIC_SCK","MIC_WS","MIC_SD",
    "DISP_CS","DISP_MOSI","DISP_CLK","DISP_DC","DISP_RST","DISP_BL","BL_G",
    "TOUCH_SDA","TOUCH_SCL","SD_CS","SD_MOSI","SD_CLK","SD_MISO",
    "NAV_UP","NAV_DN","NAV_L","NAV_R","NAV_MID","AMP_EN",
    "OUTL","OUTR","AIN","SPK_P","SPK_N","HP_L","HP_R","HP_DET",
    "EN","IO0","CHG_CHRG","CHG_STBY","PG","VBAT_SENSE",
    "PWR_G","PWR_HOLD","HOLD_G",
]}
GND=n["GND"]; V3=n["+3V3"]; VSYS=n["VSYS"]; VBAT=n["VBAT"]; VBUS=n["VBUS"]

# ---------------------------------------------------------------- passive templates
def tmpl(name, ref, fp, pins):
    t = Part(name=name, ref_prefix=ref, dest=TEMPLATE, tool=SKIDL, footprint=fp)
    for num, pname, func in pins: t += Pin(num=num, name=pname, func=func)
    return t
R  = tmpl("R","R","Resistor_SMD:R_0402_1005Metric",[("1","1",PAS),("2","2",PAS)])
C4 = tmpl("C","C","Capacitor_SMD:C_0402_1005Metric",[("1","1",PAS),("2","2",PAS)])
C6 = tmpl("C","C","Capacitor_SMD:C_0603_1608Metric",[("1","1",PAS),("2","2",PAS)])
C8 = tmpl("C","C","Capacitor_SMD:C_0805_2012Metric",[("1","1",PAS),("2","2",PAS)])
Lp = tmpl("L","L","qurannode:MWSA0402S",[("1","1",PAS),("2","2",PAS)])  # matches C408334 land
FB = tmpl("FB","FB","Inductor_SMD:L_0603_1608Metric",[("1","1",PAS),("2","2",PAS)])
LED= tmpl("LED","D","LED_SMD:LED_0603_1608Metric",[("1","K",PAS),("2","A",PAS)])

def r(val,tag): return R(value=val,tag=tag)
def c(val,fp,tag):
    t={"0402":C4,"0603":C6,"0805":C8}[fp]; return t(value=val,tag=tag)

# ---------------------------------------------------------------- ICs (VERIFY pin #s)
def ic(name, ref, fp, pins, tag):
    p = Part(name=name, ref_prefix=ref, dest=TEMPLATE, tool=SKIDL, footprint=fp)
    for num, pname, func in pins: p += Pin(num=num, name=pname, func=func)
    return p(tag=tag)

# ESP32-S3-WROOM-1  (standard Espressif module numbering; 41=thermal GND pad)
esp = ic("ESP32-S3-WROOM-1","U","RF_Module:ESP32-S3-WROOM-1",[
    ("1","GND",PWR),("2","3V3",PWR),("3","EN",INP),("4","IO4",BI),("5","IO5",BI),
    ("6","IO6",BI),("7","IO7",BI),("8","IO15",BI),("9","IO16",BI),("10","IO17",BI),
    ("11","IO18",BI),("12","IO8",BI),("13","IO19",BI),("14","IO20",BI),("15","IO3",BI),
    ("16","GND",PWR),("17","IO46",BI),("18","IO9",BI),("19","IO10",BI),("20","IO11",BI),
    ("21","IO12",BI),("22","IO13",BI),("23","IO14",BI),("24","IO21",BI),("25","IO47",BI),
    ("26","IO48",BI),("27","IO45",BI),("28","IO0",BI),("29","IO35",BI),("30","IO36",BI),
    ("31","IO37",BI),("32","IO38",BI),("33","IO39",BI),("34","IO40",BI),("35","IO41",BI),
    ("36","IO42",BI),("37","RXD0",BI),("38","TXD0",BI),("39","IO2",BI),("40","IO1",BI),
    ("41","GND",PWR)], "ESP32")

# PCM5102A TSSOP-20 (PW) -- datasheet SLAS859C p5 VERIFIED (was reversed: 19/20 pins wrong).
# pins 1-10 down the left, 11-20 up the right. pin3 CPGND named "GND" (ties to GND plane).
dac = ic("PCM5102A","U","Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm",[
    ("1","CPVDD",PWR),("2","CAPP",PAS),("3","GND",PWR),("4","CAPM",PAS),("5","VNEG",PAS),
    ("6","OUTL",OUT),("7","OUTR",OUT),("8","AVDD",PWR),("9","AGND",PWR),("10","DEMP",INP),
    ("11","FLT",INP),("12","SCK",INP),("13","BCK",INP),("14","DIN",INP),("15","LRCK",INP),
    ("16","FMT",INP),("17","XSMT",INP),("18","LDOO",PAS),("19","DGND",PWR),("20","DVDD",PWR)],
    "DAC")

# PAM8302A MSOP-8 (PAM8302AASCR, no thermal pad) -- datasheet DS41333 p9 VERIFIED
# (was: VDD/GND swapped, VO+/VO- swapped, pin2 tied to GND but it is NC).
amp = ic("PAM8302A","U","Package_SO:MSOP-8_3x3mm_P0.65mm",[
    ("1","SD",INP),("2","NC",PAS),("3","IN+",INP),("4","IN-",INP),
    ("5","VO+",OUT),("6","VDD",PWR),("7","GND",PWR),("8","VO-",OUT)], "AMP")

# TP4056 ESOP-8  [VERIFY -- common: 1 TEMP,2 PROG,3 GND,4 VCC,5 BAT,6 STDBY,7 CHRG,8 CE,9 EP]
chg = ic("TP4056","U","Package_SO:SOIC-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x3.3mm",[
    ("1","TEMP",INP),("2","PROG",PAS),("3","GND",PWR),("4","VCC",PWR),("5","BAT",PWR),
    ("6","STDBY",OUT),("7","CHRG",OUT),("8","CE",INP),("9","EP",PWR)], "CHG")

# TPS63020 DSJ (VSON-14, 0.5mm pitch) -- datasheet SLVS916 VERIFIED. Custom footprint:
# KiCad's generic VSON-14 is 0.65mm pitch (wrong). EP(15)=PGND, joined to GND on the plane.
reg = ic("TPS63020","U","qurannode:TPS63020_DSJ_VSON14",[
    ("1","VINA",PWR),("2","GND",PWR),("3","FB",INP),("4","VOUT",PWR),("5","VOUT",PWR),
    ("6","L2",PAS),("7","L2",PAS),("8","L1",PAS),("9","L1",PAS),("10","VIN",PWR),
    ("11","VIN",PWR),("12","EN",INP),("13","PS/SYNC",INP),("14","PG",OUT),("15","GND",PWR)], "REG")

# USBLC6-2SC6 SOT-23-6 -- ST DocID11265 VERIFIED. pins 1&6 = one data line (I/O1),
# pins 3&4 = the other (I/O2). (Was cross-wired: D+/D- split across both lines.)
esd = ic("USBLC6-2SC6","U","Package_TO_SOT_SMD:SOT-23-6",[
    ("1","IO1",BI),("2","GND",PWR),("3","IO2",BI),("4","IO2",BI),("5","VBUS",PWR),("6","IO1",BI)],
    "ESD")

# ICS-43434 (TDK) I2S MEMS mic -- INMP441 is out-of-stock; ICS-43434 (C5656610) is the
# pin-compatible successor. KiCad Sensor_Audio symbol pinout below. Footprint ships with
# the acoustic port hole (unlike the INMP441 land).
mic = ic("ICS-43434","MK","Sensor_Audio:InvenSense_ICS-43434-6_3.5x2.65mm",[
    ("1","WS",INP),("2","L/R",INP),("3","GND",PWR),("4","SCK",INP),("5","VDD",PWR),("6","SD",OUT)],
    "MIC")

# Cell protection (DW01A + FS8205A) OMITTED: the EEMB LP603449 ships with its own integrated
# protection PCB, so an on-board series stage is redundant (extra Rds(on) loss + nuisance-trip
# risk) -- and the previous topology was wrong. Cell- ties straight to GND. To re-add, use a
# known-good ref (FS8205: 1 G1,2 S1,3 D,4 G2,5 S2,6 D; OD->cell- FET gate, OC->P- FET gate).

# Connectors (hand-soldered)
usbc = ic("USB-C-16P","J","Connector_USB:USB_C_Receptacle_XKB_U262-16XN-4BVC11",[
    ("A1","GND",PWR),("A4","VBUS",PWR),("A5","CC1",PAS),("A6","DP1",BI),("A7","DM1",BI),("A9","VBUS",PWR),("A12","GND",PWR),
    ("B1","GND",PWR),("B4","VBUS",PWR),("B5","CC2",PAS),("B6","DP2",BI),("B7","DM2",BI),("B9","VBUS",PWR),("B12","GND",PWR),
    ("SH","SH",PWR)], "USBC")
sd  = ic("microSD","J","Connector_Card:microSD_HC_Hirose_DM3AT-SF-PEJM5",[
    ("1","DAT2",BI),("2","CD/DAT3",BI),("3","CMD",BI),("4","VDD",PWR),("5","CLK",INP),
    ("6","GND",PWR),("7","DAT0",BI),("8","DAT1",BI),("SH","SH",PWR)], "SD")
# LCD input rail = the module's real 14-pin 2.54mm header, HORIZONTAL, so the LCD
# solders straight on. Pinout MEASURED from the module silkscreen (1..14).
disp= ic("LCD_HDR_1x14","J","Connector_PinHeader_2.54mm:PinHeader_1x14_P2.54mm_Horizontal",[
    ("1","VCC",PWR),("2","GND",PWR),("3","LCD_CS",INP),("4","LCD_RST",INP),("5","LCD_RS",INP),
    ("6","MOSI",INP),("7","SCK",INP),("8","LED",PWR),("9","MISO",OUT),("10","CTP_SCL",BI),
    ("11","CTP_RST",INP),("12","CTP_SDA",BI),("13","CTP_INT",OUT),("14","SD_CS",INP)], "DISP")
spk = ic("SPK_JST_PH2","J","Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",[
    ("1","P",PAS),("2","N",PAS)], "SPK")
bat = ic("BATT_JST_PH2","J","Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical",[
    ("1","P",PWR),("2","N",PWR)], "BATT")
hp  = ic("TRS_3.5","J","Connector_Audio:Jack_3.5mm_PJ31060-I_Horizontal",[
    ("S","GND",PWR),("T","L",PAS),("R1","R",PAS),("R2","NC2",PAS),("TN","TN",PAS),("R1N","R1N",PAS)], "HP")
# SMD 5-way switch placed directly on the PCB (custom footprint, 9.9mm square).
# Pads 1-6 = UP/DN/L/R/MID/COM. [VERIFY land pattern vs your exact part before fab]
navsw = ic("NAV_5WAY","SW","qurannode:NAV_5WAY_SMD",[
    ("1","UP",PAS),("2","DN",PAS),("3","L",PAS),("4","R",PAS),("5","MID",PAS),("6","COM",PAS)],
    "NAV")
rstsw = ic("RST","SW","Button_Switch_SMD:SW_SPST_TL3342",[("1","1",PAS),("2","2",PAS)], "RST")
bootsw= ic("BOOT","SW","Button_Switch_SMD:SW_SPST_TL3342",[("1","1",PAS),("2","2",PAS)], "BOOT")
q1  = ic("2N7002","Q","Package_TO_SOT_SMD:SOT-23",[("1","G",INP),("2","S",PAS),("3","D",PAS)], "Q1")
# soft-latch power (replaces the mechanical on/off switch): center D-pad turns on,
# ESP32 latches itself on via PWR_HOLD (GPIO48) and can power down in software.
qpwr = ic("AO3401A_PFET","Q","Package_TO_SOT_SMD:SOT-23",[("1","G",INP),("2","S",PAS),("3","D",PAS)],"QPWR")
qhold= ic("2N7002_NFET","Q","Package_TO_SOT_SMD:SOT-23",[("1","G",INP),("2","S",PAS),("3","D",PAS)],"QHOLD")
dbtn = ic("BAT54","D","Diode_SMD:D_SOD-323",[("1","K",PAS),("2","A",PAS)],"DBTN")

# ==================================================================== POWER
usbc["VBUS"] += VBUS; usbc["GND"] += GND; usbc["SH"] += GND
Rcc1=R(value="5k1",tag="Rcc1"); Rcc2=R(value="5k1",tag="Rcc2")
usbc["CC1"]+=Rcc1[1]; Rcc1[2]+=GND; usbc["CC2"]+=Rcc2[1]; Rcc2[2]+=GND
# D+ taps line I/O1 (esd pins 1&6), D- taps line I/O2 (pins 3&4). Tie both Type-C sides
# (DP1/DP2, DM1/DM2) so USB data works in either cable orientation.
usbc["DP1"]+=n["USB_DP"]; usbc["DP2"]+=n["USB_DP"]; esd["IO1"]+=n["USB_DP"]
usbc["DM1"]+=n["USB_DM"]; usbc["DM2"]+=n["USB_DM"]; esd["IO2"]+=n["USB_DM"]
esd["VBUS"]+=VBUS; esd["GND"]+=GND
Cvbus=C4(value="100n",tag="Cvbus"); Cvbus[1]+=VBUS; Cvbus[2]+=GND

# charger
chg["VCC"]+=VBUS; chg["CE"]+=VBUS; chg["BAT"]+=VBAT; chg["GND"]+=GND; chg["EP"]+=GND
Cin=C8(value="10u",tag="Cchgin"); Cin[1]+=VBUS; Cin[2]+=GND
Cbat=C8(value="10u",tag="Cchgbat"); Cbat[1]+=VBAT; Cbat[2]+=GND
Rprog=R(value="1k2",tag="Rprog"); chg["PROG"]+=Rprog[1]; Rprog[2]+=GND
Rt1=R(value="10k",tag="Rtemp1"); Rt2=R(value="10k",tag="Rtemp2")
chg["TEMP"]+=Rt1[1],Rt2[1]; Rt1[2]+=VBUS; Rt2[2]+=GND
chg["CHRG"]+=n["CHG_CHRG"]; chg["STDBY"]+=n["CHG_STBY"]
Dc=LED(value="red",tag="Dchrg"); Rc=R(value="1k",tag="Rchrg"); Dc["A"]+=VBUS; Dc["K"]+=Rc[1]; Rc[2]+=n["CHG_CHRG"]
Ds=LED(value="grn",tag="Dstby"); Rs=R(value="1k",tag="Rstby"); Ds["A"]+=VBUS; Ds["K"]+=Rs[1]; Rs[2]+=n["CHG_STBY"]

# Battery JST: cell+ = VBAT, cell- = GND (the LP603449's own protection PCB sits at the cell).
bat["P"]+=VBAT; bat["N"]+=GND

# soft-latch power path: Q_PWR (P-FET) VBAT -> VSYS, gate PWR_G (held off by R_G).
qpwr["S"]+=VBAT; qpwr["D"]+=VSYS; qpwr["G"]+=n["PWR_G"]
Rgl=R(value="100k",tag="Rglatch"); Rgl[1]+=VBAT; Rgl[2]+=n["PWR_G"]
# press-hold centre D-pad: NAV_MID -> GND (via switch COM) pulls PWR_G low through the
# Schottky -> Q_PWR on. Diode isolates so the button still reads on GPIO21 when running.
dbtn["A"]+=n["PWR_G"]; dbtn["K"]+=n["NAV_MID"]
# ESP32 latches on: PWR_HOLD (GPIO48) -> Q_HOLD (N-FET) pulls PWR_G low; drop it to power off.
qhold["D"]+=n["PWR_G"]; qhold["S"]+=GND; qhold["G"]+=n["HOLD_G"]
Rh=R(value="10k",tag="Rhold"); Rh[1]+=n["PWR_HOLD"]; Rh[2]+=n["HOLD_G"]
Rhpd=R(value="100k",tag="Rholdpd"); Rhpd[1]+=n["HOLD_G"]; Rhpd[2]+=GND
esp["IO48"]+=n["PWR_HOLD"]

# buck-boost -> +3V3
reg["VIN"]+=VSYS; reg["VINA"]+=VSYS; reg["EN"]+=VSYS; reg["PS/SYNC"]+=GND
reg["GND"]+=GND; reg["VOUT"]+=V3
L1=Lp(value="1u5",tag="L1"); reg["L1"]+=L1[1]; reg["L2"]+=L1[2]
Rfb1=R(value="560k",tag="Rfb1"); Rfb2=R(value="100k",tag="Rfb2")
reg["FB"]+=Rfb1[2],Rfb2[1]; Rfb1[1]+=V3; Rfb2[2]+=GND
Rpg=R(value="100k",tag="Rpg"); reg["PG"]+=Rpg[1]; Rpg[2]+=V3; reg["PG"]+=n["PG"]
Cvin1=C8(value="10u",tag="Cvin1"); Cvin1[1]+=VSYS; Cvin1[2]+=GND
Cvin2=C8(value="10u",tag="Cvin2"); Cvin2[1]+=VSYS; Cvin2[2]+=GND
Cvo1=C8(value="22u",tag="Cvo1"); Cvo1[1]+=V3; Cvo1[2]+=GND
Cvo2=C8(value="22u",tag="Cvo2"); Cvo2[1]+=V3; Cvo2[2]+=GND

# ==================================================================== MCU
esp["3V3"]+=V3; esp["EN"]+=n["EN"]
for g in ("1","16","41"): esp[g]+=GND
Cbulk=C8(value="22u",tag="Cesp"); Cbulk[1]+=V3; Cbulk[2]+=GND
Cdec=C4(value="100n",tag="Cespd"); Cdec[1]+=V3; Cdec[2]+=GND
Ren=R(value="10k",tag="Ren"); Ren[1]+=V3; Ren[2]+=n["EN"]
Cen=C4(value="1u",tag="Cen"); Cen[1]+=n["EN"]; Cen[2]+=GND
rstsw["1"]+=n["EN"]; rstsw["2"]+=GND
Rio0=R(value="10k",tag="Rio0"); Rio0[1]+=V3; Rio0[2]+=n["IO0"]; esp["IO0"]+=n["IO0"]
bootsw["1"]+=n["IO0"]; bootsw["2"]+=GND
# USB
esp["IO19"]+=n["USB_DM"]; esp["IO20"]+=n["USB_DP"]
# I2S out / mic / SPI / touch / SD / nav / amp -- by GPIO
esp["IO4"]+=n["I2S_BCK"]; esp["IO5"]+=n["I2S_WS"]; esp["IO6"]+=n["I2S_DATA"]
esp["IO7"]+=n["MIC_SCK"]; esp["IO8"]+=n["MIC_WS"]; esp["IO9"]+=n["MIC_SD"]
esp["IO10"]+=n["DISP_CS"]; esp["IO11"]+=n["DISP_MOSI"]; esp["IO12"]+=n["DISP_CLK"]
esp["IO13"]+=n["DISP_DC"]; esp["IO14"]+=n["DISP_RST"]; esp["IO15"]+=n["DISP_BL"]
esp["IO16"]+=n["TOUCH_SDA"]; esp["IO17"]+=n["TOUCH_SCL"]
esp["IO1"]+=n["SD_CS"]; esp["IO2"]+=n["SD_MOSI"]; esp["IO42"]+=n["SD_CLK"]; esp["IO41"]+=n["SD_MISO"]
esp["IO40"]+=n["NAV_UP"]; esp["IO39"]+=n["NAV_DN"]; esp["IO38"]+=n["NAV_L"]
esp["IO47"]+=n["NAV_R"]; esp["IO21"]+=n["NAV_MID"]; esp["IO18"]+=n["AMP_EN"]
# Headphone-detect on GPIO46 (module pin 17, far from the SD-bus corner so it doesn't
# congest the microSD escape). GPIO46 is a strapping pin but is IGNORED in normal boot
# (GPIO0 high); we flash over USB, not UART, and it's resistor-pulled (100k), not driven.
esp["IO46"]+=n["HP_DET"]
# battery voltage sense: VSYS -> 100k/100k divider -> GPIO3 (ADC1_CH2) + 100nF filter.
# Reads VBAT/2 when the device is on; scale x2 in firmware. GPIO3 is a strapping pin.
esp["IO3"]+=n["VBAT_SENSE"]
Rbs1=R(value="100k",tag="Rbsns1"); Rbs1[1]+=VSYS; Rbs1[2]+=n["VBAT_SENSE"]
Rbs2=R(value="100k",tag="Rbsns2"); Rbs2[1]+=n["VBAT_SENSE"]; Rbs2[2]+=GND
Cbs=C4(value="100n",tag="Cbsns"); Cbs[1]+=n["VBAT_SENSE"]; Cbs[2]+=GND

# ==================================================================== AUDIO OUT
for p in ("DVDD","AVDD","CPVDD"): dac[p]+=V3
dac["DGND"]+=GND; dac["AGND"]+=GND; dac["GND"]+=GND
for tg in ("Cdac1","Cdac2","Cdac3"):
    cc=C4(value="100n",tag=tg); cc[1]+=V3; cc[2]+=GND
Cdb=C8(value="10u",tag="Cdacbulk"); Cdb[1]+=V3; Cdb[2]+=GND
Cldo=C4(value="1u",tag="Cldoo"); dac["LDOO"]+=Cldo[1]; Cldo[2]+=GND
Cvn=C6(value="2u2",tag="Cvneg"); dac["VNEG"]+=Cvn[1]; Cvn[2]+=GND
Ccp=C6(value="2u2",tag="Ccap"); dac["CAPP"]+=Ccp[1]; dac["CAPM"]+=Ccp[2]
dac["SCK"]+=GND; dac["FMT"]+=GND; dac["FLT"]+=GND; dac["DEMP"]+=GND
Rxs=R(value="10k",tag="Rxsmt"); dac["XSMT"]+=Rxs[1]; Rxs[2]+=V3
dac["BCK"]+=n["I2S_BCK"]; dac["LRCK"]+=n["I2S_WS"]; dac["DIN"]+=n["I2S_DATA"]
dac["OUTL"]+=n["OUTL"]; dac["OUTR"]+=n["OUTR"]
# line-out series R + DC block to jack
RolL=R(value="100R",tag="RolL"); RolR=R(value="100R",tag="RolR")
CdbL=C6(value="2u2",tag="CdbL"); CdbR=C6(value="2u2",tag="CdbR")
n["OUTL"]+=RolL[1]; RolL[2]+=CdbL[1]; CdbL[2]+=n["HP_L"]
n["OUTR"]+=RolR[1]; RolR[2]+=CdbR[1]; CdbR[2]+=n["HP_R"]
hp["L"]+=n["HP_L"]; hp["R"]+=n["HP_R"]; hp["GND"]+=GND
# Headphone-insert detect: the jack's tip-switch (TN) opens when a plug is inserted.
# TN -> GPIO44 with a 100k pull-up; the tip (HP_L) gets a 20k pull-down that both DC-
# references the AC-coupled output and gives the "no plug" LOW: switch closed (no plug)
# pulls GPIO44 to ~0.55V (LOW); plug inserted -> switch open -> pull-up -> HIGH.
hp["TN"]+=n["HP_DET"]
Rhpu=R(value="100k",tag="Rhpdet"); Rhpu[1]+=n["HP_DET"]; Rhpu[2]+=V3
Rhpd=R(value="20k",tag="Rhptip"); Rhpd[1]+=n["HP_L"]; Rhpd[2]+=GND
# amp: sum L+R into IN+
RsumL=R(value="20k",tag="RsumL"); RsumR=R(value="20k",tag="RsumR")
Cinp=C4(value="470n",tag="Campin"); Cinn=C4(value="470n",tag="Campinn")
n["OUTL"]+=RsumL[1]; n["OUTR"]+=RsumR[1]; RsumL[2]+=n["AIN"]; RsumR[2]+=n["AIN"]
n["AIN"]+=Cinp[1]; Cinp[2]+=amp["IN+"]
amp["IN-"]+=Cinn[1]; Cinn[2]+=GND
amp["VDD"]+=VSYS; amp["GND"]+=GND   # PAM8302A MSOP-8 has a single GND (pin7); pin2 is NC
Camp=C8(value="10u",tag="Campvdd"); Camp[1]+=VSYS; Camp[2]+=GND
Campd=C4(value="100n",tag="Campd"); Campd[1]+=VSYS; Campd[2]+=GND
amp["SD"]+=n["AMP_EN"]; Rsd=R(value="100k",tag="Rampsd"); Rsd[1]+=n["AMP_EN"]; Rsd[2]+=GND
# ferrite + speaker
Fp=FB(value="600R",tag="Fbp"); Fn=FB(value="600R",tag="Fbn")
amp["VO+"]+=Fp[1]; Fp[2]+=n["SPK_P"]; amp["VO-"]+=Fn[1]; Fn[2]+=n["SPK_N"]
spk["P"]+=n["SPK_P"]; spk["N"]+=n["SPK_N"]

# ==================================================================== MIC
mic["VDD"]+=V3; mic["GND"]+=GND; mic["L/R"]+=GND
mic["SCK"]+=n["MIC_SCK"]; mic["WS"]+=n["MIC_WS"]; mic["SD"]+=n["MIC_SD"]
Cmic=C4(value="100n",tag="Cmic"); Cmic[1]+=V3; Cmic[2]+=GND

# ==================================================================== DISPLAY
disp["VCC"]+=V3; disp["GND"]+=GND
disp["LCD_CS"]+=n["DISP_CS"]; disp["MOSI"]+=n["DISP_MOSI"]; disp["SCK"]+=n["DISP_CLK"]
disp["LCD_RS"]+=n["DISP_DC"]; disp["LCD_RST"]+=n["DISP_RST"]
disp["CTP_SDA"]+=n["TOUCH_SDA"]; disp["CTP_SCL"]+=n["TOUCH_SCL"]; disp["CTP_RST"]+=n["DISP_RST"]
# pin 9 MISO, 13 CTP_INT, 14 SD_CS (module's onboard microSD + touch IRQ) left open:
# we use the mainboard microSD (J2) and poll touch (no free GPIO). See note in DESIGN.md.
Rts=R(value="4k7",tag="Rtsda"); Rts[1]+=n["TOUCH_SDA"]; Rts[2]+=V3
Rtc=R(value="4k7",tag="Rtscl"); Rtc[1]+=n["TOUCH_SCL"]; Rtc[2]+=V3
# backlight FET
q1["G"]+=n["BL_G"]; q1["S"]+=GND; q1["D"]+=disp["LED"]
Rbl=R(value="100R",tag="Rbl"); Rbl[1]+=n["DISP_BL"]; Rbl[2]+=n["BL_G"]
Rblp=R(value="10k",tag="Rblpd"); Rblp[1]+=n["BL_G"]; Rblp[2]+=GND
Cdisp=C8(value="10u",tag="Cdisp"); Cdisp[1]+=V3; Cdisp[2]+=GND

# ==================================================================== microSD
sd["VDD"]+=V3; sd["GND"]+=GND; sd["SH"]+=GND
sd["CMD"]+=n["SD_MOSI"]; sd["CLK"]+=n["SD_CLK"]; sd["DAT0"]+=n["SD_MISO"]; sd["CD/DAT3"]+=n["SD_CS"]
for name,tg in [("SD_CS","Rsdcs"),("SD_MOSI","Rsdmosi"),("SD_MISO","Rsdmiso")]:
    rr=R(value="10k",tag=tg); rr[1]+=n[name]; rr[2]+=V3
Csd=C8(value="10u",tag="Csd"); Csd[1]+=V3; Csd[2]+=GND
Csdd=C4(value="100n",tag="Csdd"); Csdd[1]+=V3; Csdd[2]+=GND

# ==================================================================== NAV
navsw["UP"]+=n["NAV_UP"]; navsw["DN"]+=n["NAV_DN"]; navsw["L"]+=n["NAV_L"]
navsw["R"]+=n["NAV_R"]; navsw["MID"]+=n["NAV_MID"]; navsw["COM"]+=GND

generate_netlist(file_="qurannode.net")
print("wrote qurannode.net")
