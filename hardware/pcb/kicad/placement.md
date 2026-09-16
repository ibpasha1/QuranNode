# Board outline + placement (board frame, mm; origin top-left, Y down)

- Board: **62.8 x 150.0 mm**, corner radius 2.0 mm (inner cavity 64.0 x 151.2 minus 0.6 mm/side).
- Overall device thickness 17.8 mm; cavity depth 15.0 mm.

| Ref | Board X | Board Y | Note |
|-----|--------:|--------:|------|
| J1 USB-C (bottom edge) | 45.4 | 150.0 | |
| J6 3.5mm jack (bottom) | 10.4 | 150.0 | |
| J2 microSD (top edge) | 22.4 | 0.0 | |
| SW3 power (left edge) | 0.0 | 10.6 | |
| U1 ESP32-S3 | 15.4 | 49.6 | |
| MK1 mic (=lid grille!) | 9.4 | 104.6 | |
| SW4 nav | 31.4 | 125.6 | |
| J4 speaker | 47.4 | 15.6 | |
| U4 TP4056 charger | 48.4 | 139.6 | |
| U2 DAC / J6 | 17.4 | 139.6 | |
| J5 battery | 29.4 | 111.6 | |

**Battery keep-out** (tape-mounted cell over the board): X 3.9..54.9, Y 94.35..128.85 mm — avoid tall parts here.

Ports are on the enclosure edges: **USB-C + headphone on the BOTTOM, microSD on the TOP, power switch on the LEFT.** Place those connectors hard against the matching board edge at the X/Y above.
