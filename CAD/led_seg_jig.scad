// Soldering jig / backing plate for the 4-digit seven-segment display
// WS2812BClock — geometry taken from center.stl
//
//   28 LED pockets (4 digits x 7 segments), each 12 x 12 mm, floor at z = 13
//   digit centres at x = -52, -20, 21, 53
//   segment offsets from the digit centre:
//       top        ( 0.0,  20.00)
//       top-left   (-10.0,  9.93)   top-right   (10.0,  9.93)
//       middle     ( 0.0,  -0.13)
//       bottom-left(-10.0,-10.07)   bottom-right(10.0,-10.07)
//       bottom     ( 0.0, -20.00)
//
// LEDs sit LENS DOWN in the recesses so the pads face up for soldering.
// With mount_bosses = true the same part doubles as the backing plate:
// each boss plugs into a 12 mm pocket in center.stl and centres the 5050
// over the light hole.

/* [What to render] */
part          = "left";   // "left" = digits 1-2, "right" = digits 3-4, "all" = one piece
mount_bosses  = true;     // false = flat jig only, true = also a mounting plate

/* [LED pocket] */
led_tangential = 5.4;     // pad-to-pad axis of the SK6812 5050
led_radial     = 5.0;
fit_clearance  = 0.25;    // per side
recess_depth   = 1.2;     // < 1.6 mm body height, so pads stand clear
light_hole     = 4.6;     // under each LED: passes light, supports the body

/* [Plate] */
plate_thickness = 3.0;
border          = 6.0;    // material around the outermost pockets
boss_dia        = 11.6;   // fits the Ø12 mm round LED seats in center.stl
boss_height     = 2.8;    // pocket depth is 3 mm
corner_notch    = 1.2;    // orientation key
pin_dia         = 3.1;    // alignment pins between left and right halves
$fn             = 48;

digit_x  = [-52, -20, 21, 53];
segments = [[0, 20.00], [-10, 9.93], [10, 9.93], [0, -0.13],
            [-10, -10.07], [10, -10.07], [0, -20.00]];

w = led_tangential + 2 * fit_clearance;
d = led_radial + 2 * fit_clearance;

function digits_for(p) = p == "left"  ? [0, 1]
                       : p == "right" ? [2, 3]
                       : [0, 1, 2, 3];

module led_cut(x, y) {
    // recess for the LED body
    translate([x - w / 2, y - d / 2, plate_thickness - recess_depth])
        cube([w, d, recess_depth + 0.1]);
    // light / push-out hole
    translate([x, y, -boss_height - 1])
        cylinder(d = light_hole, h = plate_thickness + boss_height + 2);
    // orientation key in one corner of the recess
    translate([x - w / 2, y - d / 2, plate_thickness - recess_depth])
        cylinder(d = corner_notch * 2, h = recess_depth + 0.1, $fn = 4);
}

module plate_body(ds) {
    xs = [for (i = ds) digit_x[i]];
    x0 = min(xs) - 10 - w / 2 - border;
    x1 = max(xs) + 10 + w / 2 + border;
    y0 = -20 - d / 2 - border;
    y1 =  20 + d / 2 + border;
    translate([x0, y0, 0]) cube([x1 - x0, y1 - y0, plate_thickness]);
}

module bosses(ds) {
    if (mount_bosses)
        for (i = ds, s = segments)
            translate([digit_x[i] + s[0], s[1], -boss_height])
                cylinder(d = boss_dia, h = boss_height + 0.01);
}

module jig(p) {
    ds = digits_for(p);
    difference() {
        union() {
            plate_body(ds);
            bosses(ds);
        }
        for (i = ds, s = segments)
            led_cut(digit_x[i] + s[0], s[1]);
        // alignment pin holes on the split line between the halves
        if (p != "all")
            for (yy = [-30, 30])
                translate([p == "left" ? 3 : -3, yy, -boss_height - 1])
                    cylinder(d = pin_dia, h = plate_thickness + boss_height + 2);
    }
}

jig(part);
