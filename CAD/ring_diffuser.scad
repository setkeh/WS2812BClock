// Diffuser plugs for the clock face ring holes
//
// Each ring position is a Ø8.4 mm hole through the face (about 6 mm deep)
// with the LED sitting in the Ø12 mm seat behind it. These plugs push into
// that hole from the FRONT and scatter the light, so the pixel reads as a
// glowing dot rather than a bare point source.
//
// Print in WHITE PLA, 0.2 mm layers, 100% infill, no supports.
// White PLA is a good diffuser at 2-3 mm; natural/clear is worse, black useless.

/* [Fit] */
hole_dia     = 8.4;    // measured from the face STL
clearance    = 0.2;    // per diameter; increase if they won't push in
plug_len     = 4.0;    // shorter than the 6 mm channel, so it sits recessed
lead_in      = 0.6;    // chamfer on the leading edge, makes them easy to start

/* [Batch] */
count        = 60;     // one per ring pixel
cols         = 10;     // layout on the bed
spacing      = 11.0;   // centre to centre

$fn = 48;

plug_dia = hole_dia - clearance;

module plug() {
    // Chamfered nose so it starts square in the hole, then a plain barrel.
    union() {
        cylinder(d1 = plug_dia - 2 * lead_in, d2 = plug_dia, h = lead_in);
        translate([0, 0, lead_in])
            cylinder(d = plug_dia, h = plug_len - lead_in);
    }
}

module batch() {
    rows = ceil(count / cols);
    for (i = [0 : count - 1])
        translate([(i % cols) * spacing, floor(i / cols) * spacing, 0])
            plug();
}

batch();

// Uncomment for a single test fit before committing to 60:
// plug();
