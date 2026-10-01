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

### Pad mapping that was verified on these parts (2026-09-25)

Established empirically on the eBay SK6812 5050s used in this build, after two LEDs lit:

> **DIN is the pad next to VDD. DOUT is the pad next to VSS.**
> VDD is diagonally opposite VSS, and DIN diagonally opposite DOUT.

Both data pads are adjacent to VSS — one along each axis — so "next to VSS" is ambiguous
and is not a safe rule. Key off VDD instead.

Diagnostics that identified the faults, for reuse on the remaining pixels:

- **Data line sits at ~2.2 V instead of 3.3 V** (measured through the 390 Ω series
  resistor, ≈2.8 mA): the LED is not powered, and the data pin is phantom-powering the
  chip through its protection diode. Check VDD/VSS at the LED's own pads.
- **No light, no heat, correct supply:** data on the wrong pad (DOUT instead of DIN).
- **Hot, no light:** VDD/VSS reversed. The part is destroyed within seconds.
- **DOUT is idle unless more pixels' data is being sent than the chain has consumed.**
  Testing with `LED_TEST_COUNT` set to 1 means LED 1 absorbs the whole frame and forwards
  nothing, so probing DOUT proves nothing. Set the count to at least the number wired.
- A plain 1 Hz GPIO toggle on the data pin (meter-readable) separates "pin/board/firmware"
  faults from LED-side faults; the LED never repeats it, since it isn't valid data.

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

## Data line level: the diodes on the power feeds

The SK6812 needs a logic high of at least **0.7 x VDD**. The regulator measures
**5.04 V**, which puts the threshold at **3.53 V**, and the ESP32 drives **3.3 V**.
The data line is therefore below specification on this build, and has behaved
accordingly: intermittent wrong-coloured pixels on both chains, and on 2026-10-01 the
seven-segment chain latched a corrupted frame and stopped accepting data entirely
until power was removed for a minute. Firmware was ruled out — all 28 segment pixels
are rewritten every second — and so was a broken connection, since a power cycle
restored it without anything being touched. See issue #36.

The fix on this hand-wired build is a **1N4007 in series with each of the twelve 5 V
injection points**, dropping the LED supply to roughly 4.0–4.3 V:

| Load | LED VDD | Threshold (0.7 x VDD) | ESP32 drives |
| --- | --- | --- | --- |
| Low (normal brightness) | ~4.34 V | 3.04 V | 3.3 V |
| Heavy (bright effects) | ~4.04 V | 2.83 V | 3.3 V |

The margin improves under load, because a diode's forward drop rises with current.
Both figures stay inside the SK6812's 3.5–5.5 V supply range.

All twelve feeds get a diode rather than one, so brightness stays uniform across the
ring and digits; the overall loss is retuned with the brightness percentages in
menuconfig. A single diode on the first LED's VDD alone would also work — that LED
then outputs ~4.3 V, satisfying every LED after it — but it leaves one pixel visibly
dimmer, which on a seven-segment digit is one dim segment.

**A lower-drop diode is the wrong instinct here.** A Schottky at ~0.4 V leaves VDD at
~4.64 V and the threshold at 3.25 V, which is almost exactly what the ESP32 drives and
no better than the present situation. Two diodes in series overshoot the other way:
~3.6 V supply is only 0.1 V above the minimum. Around 0.7–1.0 V of drop is the useful
range, which is what an ordinary silicon rectifier gives.

Worth measuring once fitted: the supply at the LED furthest from an injection point
during a full-white test, confirming it stays above 3.5 V, and the diode temperature
during a bright animation, since current does not divide evenly between twelve feeds.

## If this gets rebuilt

A custom PCB ring at the same R160 / 6° geometry would replace both the jig and the
hand-wiring, with solderable pads, a ground plane and proper per-pixel decoupling.
It should also carry a **74AHCT125 or 74HCT245 level shifter** on the data lines,
which is the correct fix for the threshold problem above and makes the twelve diodes
unnecessary. It must be an AHCT or HCT part: an HC part needs 3.5 V for a logic high
on a 5 V supply and would change nothing.
The coordinates above drive it directly — KiCad's Python console can place the 88
footprints from the same angles and offsets rather than positioning them by hand.
