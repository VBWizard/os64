// The painter's cases, each recording worked out by hand from the layout
// (whose own dumps libflow's harness proves) and the drawing rules in
// paint.c: the canvas, the bevel tones, the diagonal corners, decorations,
// the atoms, and the viewport cut.
//
// Tones: the test ink 0x101010 bevels to 0x080808 (half to black) and
// 0x878787 (half to white: 0x10 + 0xef / 2); an hr's #808080 to 0x404040
// and 0xbfbfbf.

static void paint_cases(void)
{
    // The body's background is the canvas's (CSS 2.1 §14.2): it fills the
    // whole viewport, and the body does not paint it again over its box.
    // "Hi" sits on a 20px line at y 16 (the p's margin collapses with the
    // body's): half-leading 2, ascent 12, baseline 30.
    paint_case("canvas: the body's background fills the view",
               "<!doctype html><body bgcolor=#ffff00><p>Hi", 100, (os64_gui_rect_t){0, 0, 100, 50},
               "fill 0 0 100 50 #ffff00\n"
               "text \"Hi\" 8 30 #101010\n");
    paint_case("canvas: with no background anywhere, the paper",
               "<!doctype html><p>Hi", 100, (os64_gui_rect_t){0, 0, 100, 50},
               "fill 0 0 100 50 #fefefe\n"
               "text \"Hi\" 8 30 #101010\n");

    // table border=2 is a 2px outset in the ink; its cell a 1px inset.
    // Outset: top and left the lighter tone, bottom and right the darker.
    // Row 1 of the top gives up one column at each end (ceil(1*2/2)), and
    // the sides start where the top's rows stop reaching: column 0 at y 9,
    // column 1 at y 10.
    paint_case("borders: outset table, inset cell, mitred corners",
               "<!doctype html><table border=2><tr><td>x</table>", 200,
               (os64_gui_rect_t){0, 0, 200, 100},
               "fill 0 0 200 100 #fefefe\n"
               "fill 8 8 20 1 #878787\n"
               "fill 9 9 18 1 #878787\n"
               "fill 8 39 20 1 #080808\n"
               "fill 9 38 18 1 #080808\n"
               "fill 8 9 1 30 #878787\n"
               "fill 9 10 1 28 #878787\n"
               "fill 27 9 1 30 #080808\n"
               "fill 26 10 1 28 #080808\n"
               "fill 12 12 12 1 #080808\n"
               "fill 12 35 12 1 #878787\n"
               "fill 12 13 1 22 #080808\n"
               "fill 23 13 1 22 #878787\n"
               "text \"x\" 14 28 #101010\n");

    // The same page through a viewport 10 wide at (10, 9): every fill cut
    // to it, what lies wholly outside it never drawn.
    paint_case("clip: fills are cut to the viewport",
               "<!doctype html><table border=2><tr><td>x</table>", 200,
               (os64_gui_rect_t){10, 9, 10, 20},
               "fill 10 9 10 20 #fefefe\n"
               "fill 10 9 10 1 #878787\n"
               "fill 12 12 8 1 #080808\n"
               "fill 12 13 1 16 #080808\n"
               "text \"x\" 14 28 #101010\n");

    // Hacker News's shape: a row's background under its cells, a cell's
    // over it. Baselines at 11 + 14.
    paint_case("tables: a row's background, then a cell's",
               "<!doctype html><table><tr bgcolor=#ff6600><td>x<td bgcolor=#00ff00>y</table>", 200,
               (os64_gui_rect_t){0, 0, 200, 50},
               "fill 0 0 200 50 #fefefe\n"
               "fill 10 10 22 22 #ff6600\n"
               "fill 22 10 10 22 #00ff00\n"
               "text \"x\" 11 25 #101010\n"
               "text \"y\" 23 25 #101010\n");

    // The hr is a 1px inset in #808080, 2px tall, so its sides have no
    // rows left to draw. The link's underline is one pixel (16px / 16)
    // one pixel below the baseline, in the link's colour; the line-through
    // 3/10 of the size above it (30 - 4). The picture's 1px solid border
    // frames a 40x30 content box; the control is handed its box whole.
    paint_case("decorations, a rule, a picture and a control",
               "<!doctype html><p><a href=x>go</a> <s>no</s><hr><img src=known.png border=1> <input>",
               200, (os64_gui_rect_t){0, 0, 200, 140},
               "fill 0 0 200 140 #fefefe\n"
               "fill 8 52 184 1 #404040\n"
               "fill 8 53 184 1 #bfbfbf\n"
               "text \"go\" 8 30 #1010ee\n"
               "fill 8 31 16 1 #1010ee\n"
               "text \" \" 24 30 #101010\n"
               "text \"no\" 28 30 #101010\n"
               "fill 28 26 16 1 #101010\n"
               "fill 8 62 42 1 #101010\n"
               "fill 8 93 42 1 #101010\n"
               "fill 8 63 1 30 #101010\n"
               "fill 49 63 1 30 #101010\n"
               "image 9 63 40 30\n"
               "control 0 8 100 160 20\n");

    // Markers as shapes, 16 / 3 = 5 px across, left of the marker's
    // quarter-em of trailing space: x = rect.x + (12 - 4 - 5) / 2, top =
    // baseline - 16 * 3 / 10 - 5 / 2. A pixel is in the disc when
    // (2c+1-5)^2 + (2r+1-5)^2 <= 25: rows of 3, 5, 5, 5, 3. The circle
    // takes away what is inside diameter 3 (<= 9): its middle rows keep
    // only their ends. Content first, then the markers (a box's marker
    // follows its content in the tree), innermost list first.
    paint_case("markers: disc, circle and square drawn as shapes",
               "<!doctype html><ul><li>a<ul><li>b<ul><li>c</ul></ul></ul>", 200,
               (os64_gui_rect_t){0, 0, 200, 100},
               "fill 0 0 200 100 #fefefe\n"
               "text \"a\" 48 30 #101010\n"
               "text \"b\" 88 50 #101010\n"
               "text \"c\" 128 70 #101010\n"
               "fill 117 64 5 5 #101010\n"
               "fill 78 44 3 1 #101010\n"
               "fill 77 45 1 1 #101010\n"
               "fill 81 45 1 1 #101010\n"
               "fill 77 46 1 1 #101010\n"
               "fill 81 46 1 1 #101010\n"
               "fill 77 47 1 1 #101010\n"
               "fill 81 47 1 1 #101010\n"
               "fill 78 48 3 1 #101010\n"
               "fill 38 24 3 1 #101010\n"
               "fill 37 25 5 1 #101010\n"
               "fill 37 26 5 1 #101010\n"
               "fill 37 27 5 1 #101010\n"
               "fill 38 28 3 1 #101010\n");

    expect("bevel: lighter and darker halve the distance",
           yonder_lighter(0x000000) == 0x7f7f7f && yonder_darker(0xffffff) == 0x7f7f7f &&
               yonder_lighter(0x808080) == 0xbfbfbf && yonder_darker(0x808080) == 0x404040,
           NULL);
}
