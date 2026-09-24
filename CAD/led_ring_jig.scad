// Soldering jig for SK6812 5050 LEDs — WS2812BClock face ring
//
// Geometry taken from led_clock_quarter_face.stl:
//   60 LED pockets at radius 160.0 mm, one every 6 deg
//   chord spacing between pocket centres = 2*160*sin(3 deg) = 16.75 mm
//
// LEDs sit LENS DOWN in the recesses, so the solder pads face up.
// Solder the chain in the jig, then lift the finished arc out and drop it
// into the clock face pockets.
//
// Printed in segments so each piece fits a small print bed.
// 6 positions per segment -> 10 segments for the full ring, each about
// 99 x 26 mm.

/* [Ring geometry] */
ring_radius      = 160.0;  // from the STL
led_count        = 60;     // pixels in the full ring
positions        = 6;      // LED positions per printed segment

/* [LED pocket] */
// SK6812 5050 body is 5.4 (pad-to-pad axis) x 5.0 x 1.6 mm.
// The 5.4 axis runs tangentially so DOUT of one faces DIN of the next.
led_tangential   = 5.4;
led_radial       = 5.0;
led_height       = 1.6;
fit_clearance    = 0.25;   // per side; loosen if prints come out tight
recess_depth     = 1.2;    // < led_height so the LED stands proud and pads clear

/* [Plate] */
mount_bosses     = true;   // Ø11.6 spigots that plug into the face's Ø12 seats,
                           // so the jig doubles as the LED mounting plate
boss_dia         = 11.6;
boss_height      = 2.8;    // seat depth in the face is 3 mm
plate_thickness  = 3.0;
plate_width      = 18.0;   // radial width of the arc
relief_dia       = 4.6;    // hole under each LED: passes light, still supports it
corner_notch     = 1.2;    // marks the VSS/chamfered corner = orientation key
end_pin_dia      = 3.1;    // alignment holes in the segment ends
$fn              = 48;

step  = 360 / led_count;
r_in  = ring_radius - plate_width / 2;
r_out = ring_radius + plate_width / 2;
span  = step * (positions - 1);

pad = step * 0.75;   // material either side of the end positions

module arc_plate() {
    // Arc centred on angle 0
    rotate([0, 0, -(span / 2 + pad)])
        rotate_extrude(angle = span + 2 * pad)
            translate([r_in, 0]) square([plate_width, plate_thickness]);
}

module led_recess() {
    w = led_tangential + 2 * fit_clearance;   // tangential
    d = led_radial + 2 * fit_clearance;       // radial
    translate([0, 0, plate_thickness - recess_depth])
        cube([d, w, recess_depth + 0.1], center = false);
}

module pocket(i) {
    a = i * step;
    rotate([0, 0, a]) {
        w = led_tangential + 2 * fit_clearance;
        d = led_radial + 2 * fit_clearance;
        // recess, centred on the ring radius
        translate([ring_radius - d / 2, -w / 2, 0]) led_recess();
        // light / push-out hole
        translate([ring_radius, 0, -boss_height - 1])
            cylinder(d = relief_dia, h = plate_thickness + boss_height + 2);
        // orientation key: notch at the corner that takes the LED's
        // chamfered (VSS) corner. Data flows towards increasing angle.
        translate([ring_radius - d / 2, -w / 2, plate_thickness - recess_depth])
            cylinder(d = corner_notch * 2, h = recess_depth + 0.1, $fn = 4);
    }
}

module bosses() {
    if (mount_bosses)
        for (i = [0 : positions - 1])
            rotate([0, 0, -span / 2 + i * step])
                translate([ring_radius, 0, -boss_height])
                    cylinder(d = boss_dia, h = boss_height + 0.01);
}

module segment() {
    difference() {
        union() { arc_plate(); bosses(); }
        for (i = [0 : positions - 1])
            rotate([0, 0, -span / 2]) pocket(i);
        // alignment pin holes in both end faces (use a 3 mm pin/filament
        // offcut to line neighbouring segments up on the bench)
        for (s = [-1, 1])
            rotate([0, 0, s * (span / 2 + step * 0.4)])
                translate([ring_radius, 0, -boss_height - 1])
                    cylinder(d = end_pin_dia, h = plate_thickness + boss_height + 2);
    }
}

segment();

// Uncomment to preview the whole ring:
// for (s = [0 : led_count / positions - 1])
//     rotate([0, 0, s * step * positions]) segment();
