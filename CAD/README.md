# CAD — clock face geometry, LED jigs, hardware notes

Mechanical reference for the WS2812BClock build: the numbers the firmware and any
future PCB depend on, plus the print-it-yourself jigs that hold the LEDs.

## Where the geometry comes from

The printed clock body is [Thingiverse thing 3966304](https://www.thingiverse.com/thing:3966304)
("ESP8266 Clock and Thermometer" by **kwg08**, licensed **CC BY-SA**). Every dimension
below was measured directly from its STLs (`led_clock_quarter_face.stl`, `center.stl`),
not taken from the listing.

**Those STLs are not redistributed here** — download them from the link above. The
measurements recorded in this file are all the firmware and the jigs need.

That design was drawn for 8 mm round through-hole pixels. We use **SK6812 5050 SMD**
parts instead, which is why the jigs exist: they centre a 5×5 mm part in a Ø12 mm seat.

## Clock face — outer ring

| Feature | Value |
| --- | --- |
| LED seats | **60**, at **radius 160.0 mm**, one every **6°** |
| Centre-to-centre spacing | 16.75 mm chord (≈11.3 mm gap between LED bodies) |
| Seat | Ø12 mm round, 3 mm deep, opening from the back |
| Front aperture | Ø8.4 mm through-hole |
| Hour marker holes | 12 × Ø4 mm at radius 131.0 mm, every 30° |
| Screw holes | 6 × Ø3.6 mm at radius 93.3 mm (matches `center.stl`) |

## Centre plate — seven-segment digits

**28 LEDs: 4 digits × 7 segments, one LED per segment.** Same Ø12 mm round seats, 3 mm
deep, each behind a segment-shaped slot in the front face.

Digit centres: **x = −52, −20, 21, 53** (y = 0 is the middle segment row).

Segment offsets from a digit centre (mm):

| Segment | Offset |
| --- | --- |
| top | (0, +20.00) |
| top-left | (−10, +9.93) |
| top-right | (+10, +9.93) |
| middle | (0, −0.13) |
| bottom-left | (−10, −10.07) |
| bottom-right | (+10, −10.07) |
| bottom | (0, −20.00) |

**Total pixel count: 60 + 28 = 88.**

## LED part: SK6812 5050 RGB

Per the [Normand SK6812 datasheet Rev 06](https://www.normandled.com/upload/201603/SK6812%20LED%20%20Datasheet.pdf):

| Pin | Symbol | Note |
| --- | --- | --- |
| 1 | VDD | diagonally opposite VSS |
| 2 | DOUT | diagonally opposite DIN |
| 3 | VSS | **the chamfered corner**, viewed from the lens side |
| 4 | DIN | shares the short edge with VSS |

- Supply **3.5–5.5 V**. Reverse polarity destroys the part in seconds — it gets hot and
  never lights.
- Logic threshold **VIH = 0.7 × VDD**, so 3.5 V at a 5 V supply. The ESP32's 3.3 V output
  is below that; a plain silicon diode in the LED's +5 V feed (≈4.3 V rail, ≈3.0 V
  threshold) fixes it if a chain proves unreliable.
- 24-bit data, **GRB** order, MSB first. Driver config: `LED_MODEL_SK6812` with
  `LED_STRIP_COLOR_COMPONENT_FMT_GRB`.
- **Lens-down in the jig mirrors the pinout.** Identify VSS on a scrap part with a meter
  on diode test (VSS conducts to all three others), mark that jig corner, and orient every
  LED to match.

## Jigs

Both parts hold LEDs **lens-down** so the pads face up for soldering, and with
`mount_bosses = true` they stay in the clock as the LED mounting plate: the Ø11.6 mm
spigots plug into the Ø12 mm seats and centre each pixel over its aperture.

| File | Covers | Printed size |
| --- | --- | --- |
| `led_ring_jig.scad` → `led_ring_jig_seg.stl` | 6 ring LEDs (36°); 10 make the ring | 26 × 113 × 5.8 mm |
| `led_seg_jig.scad` → `led_seg_jig_{left,right}.stl` | 14 digit LEDs each (2 digits) | 70 × 57.5 × 5.8 mm |

Key parameters (top of each file): `positions`, `ring_radius`, `led_count`,
`fit_clearance` (0.25 mm per side, loosen/tighten to suit the printer), `recess_depth`
(1.2 mm of the 1.6 mm body, so pads stand proud), `light_hole` / `relief_dia` (4.6 mm),
`boss_dia` (11.6 mm), `mount_bosses`.

Printed in **PLA** on an Ender 3 V2 Neo, ~1 h per ring segment. PLA softens around 60 °C,
so keep the iron at 280–300 °C and each joint to 1–2 s; PETG would have more margin.

## Wiring plan

- **10 ring segments of 6 LEDs**, each with its own 5 V/GND feed from the regulator
  (18 AWG trunk), so only the **data line is continuous** around the ring.
- Run a **ground wire alongside the data** across each segment joint. The data return
  path is ground; without a local link the return loop goes back to the star point and
  the signal edges suffer.
- **30 AWG wire-wrap wire** for LED-to-LED links and data: ~0.33 Ω/m, so a 12 mm link is
  ~0.004 Ω, negligible at the ≤360 mA a full-white segment draws.
- **Decoupling per power group** (not per LED): 100 nF ceramic + 100 µF electrolytic
  across 5 V/GND at each feed point, ceramic closest to the LEDs. 10 groups on the ring,
  2–4 on the digits. 1000 µF bulk at the regulator.
- Full-white current for 88 pixels is ~5 A; cap brightness in firmware (25 % ≈ 1.3 A).

## If this gets rebuilt

A custom PCB ring at the same R160 / 6° geometry would replace both the jig and the
hand-wiring, with solderable pads, a ground plane and proper per-pixel decoupling.
The coordinates above drive it directly — KiCad's Python console can place the 88
footprints from the same angles and offsets rather than positioning them by hand.
