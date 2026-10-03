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

    // A picture behind the body is the canvas's too, colour or none: tiled
    // across the whole viewport from the page's corner, so a view scrolled
    // down 20 still tiles from (0, 0). The body does not draw it again.
    paint_case("canvas: the body's picture, with no colour, over the paper",
               "<!doctype html><body background=sky.gif><p>Hi", 100,
               (os64_gui_rect_t){0, 0, 100, 50},
               "fill 0 0 100 50 #fefefe\n"
               "backdrop 0 0 100 50 from 0 0\n"
               "text \"Hi\" 8 30 #101010\n");
    paint_case("canvas: the body's picture over its colour, scrolled",
               "<!doctype html><body bgcolor=#ffff00 background=sky.gif><p>Hi", 100,
               (os64_gui_rect_t){0, 20, 100, 50},
               "fill 0 20 100 50 #ffff00\n"
               "backdrop 0 20 100 50 from 0 0\n"
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
    // A cell's picture over its colour, tiled from its border box's corner.
    paint_case("tables: a cell's picture over its colour",
               "<!doctype html><table><tr bgcolor=#ff6600><td>x<td bgcolor=#00ff00 background=c.gif>y"
               "</table>",
               200, (os64_gui_rect_t){0, 0, 200, 50},
               "fill 0 0 200 50 #fefefe\n"
               "fill 10 10 22 22 #ff6600\n"
               "fill 22 10 10 22 #00ff00\n"
               "backdrop 22 10 10 22 from 22 10\n"
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

    // overflow: hidden (GARB.md, G5): the p is 100 wide in a 50-by-10 div
    // that hides, so its red is cut to the div's padding box, and its
    // text — drawn whole by its verb — is handed that box as its clip.
    paint_page_case("overflow: hidden cuts what is inside it",
                    "<!doctype html><style>body { margin: 0 } div { width: 50px; height: 10px;"
                    " overflow: hidden; background: #00ff00 } p { margin: 0; width: 100px;"
                    " background: #ff0000 }</style><div><p>ab cd ef</p></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "fill 0 0 50 10 #00ff00\n"
                    "fill 0 0 50 10 #ff0000\n"
                    "text \"ab cd ef\" 0 14 #101010 clip 0 0 50 10\n");
    // A fixed header is painted where it is on the glass: scrolled 500
    // down, its background at the view's top, 500, and its text on its
    // baseline 14 below that — after the page, which is under it.
    paint_page_case("a fixed header is painted where the scroll puts it",
                    "<!doctype html><style>body { margin: 0 } p { margin: 0; height: 1000px }"
                    " div { position: fixed; top: 0; left: 0; right: 0; height: 30px;"
                    " background: #00ff00 }</style><p>long</p><div>top</div>",
                    200, (os64_gui_rect_t){0, 500, 200, 100}, true,
                    "fill 0 500 200 100 #fefefe\n"
                    "fill 0 500 200 30 #00ff00\n"
                    "text \"top\" 0 514 #101010\n");
    // A sticky header written at the top rides the view down: scrolled
    // 500, it is drawn at 500 like the fixed one, its text with it, over
    // the paragraph it was written before.
    paint_page_case("a sticky header is painted where the scroll pushes it",
                    "<!doctype html><style>body { margin: 0 } p { margin: 0; height: 1000px }"
                    " div { position: sticky; top: 0; height: 30px; background: #00ff00 }"
                    "</style><div>top</div><p>long</p>",
                    200, (os64_gui_rect_t){0, 500, 200, 100}, true,
                    "fill 0 500 200 100 #fefefe\n"
                    "fill 0 500 200 30 #00ff00\n"
                    "text \"top\" 0 514 #101010\n");

    // The blend (PILE3.md). A translucent colour reaches the verbs with
    // its alpha, for them to lay over what is there: 0.5 is 128 of 255,
    // 0.25 is 64.
    paint_page_case("a translucent background and text keep their alpha",
                    "<!doctype html><style>body { margin: 0 } div { height: 20px;"
                    " background: rgba(255, 0, 0, 0.5); color: rgba(0, 0, 255, 0.25) }"
                    "</style><div>a</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "fill 0 0 200 20 #ff0000/128\n"
                    "text \"a\" 0 14 #0000ff/64\n");
    // A block below full opacity is a stacking context, painted whole
    // where a positioned box of z-index 0 would be — after the flow it sits
    // in, so the paragraph written after it is painted first — and as ONE
    // group: its bounds when it opens, its alpha (0.5: 128) when it closes.
    paint_page_case("a block below full opacity is one group, after the flow",
                    "<!doctype html><style>body { margin: 0 } p { margin: 0; height: 20px }"
                    " .h { opacity: 0.5; height: 20px; background: #00ff00 }</style>"
                    "<div class=h>a</div><p>b</p>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "text \"b\" 0 34 #101010\n"
                    "group 0 0 200 20\n"
                    "fill 0 0 200 20 #00ff00\n"
                    "text \"a\" 0 14 #101010\n"
                    "end group 128\n");
    // An inline-block, inline-flex or inline-grid is an atom on its line and
    // a content box inside it, one element: its translucent background is
    // laid ONCE, by the atom, or 0.5 would land as 0.75 (Quinn, #202).
    static const char *const kInlineContainers[] = {"inline-block", "inline-flex",
                                                    "inline-grid"};
    for (size_t k = 0; k < sizeof(kInlineContainers) / sizeof(kInlineContainers[0]); k++) {
        char html[256];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body { margin: 0 } span { display: %s; width: 40px;"
                 " height: 40px; background: rgba(255, 0, 0, 0.5) }</style><span></span>",
                 kInlineContainers[k]);
        paint_page_case("an inline container's translucent frame is painted once", html, 200,
                        (os64_gui_rect_t){0, 0, 200, 100}, true,
                        "fill 0 0 200 100 #fefefe\n"
                        "fill 0 0 40 40 #ff0000/128\n");
    }

    // The border styles (PILE3.md § The box's edge), on a top side alone, 20
    // wide. Dashed 2px: dashes 6 long, 6 apart, from the box's edge, on
    // each of its two rows. Dotted 2px: squares 2 across, 2 apart. Double
    // 3px: the outer and inner thirds, the middle row left. Ridge 2px is a
    // groove turned over: its outer row the lighter tone (#808080 to
    // #bfbfbf), its inner the darker (#404040).
    struct { const char *style, *want; } kEdges[] = {
        {"2px dashed #ff0000",
         "fill 0 0 6 1 #ff0000\nfill 12 0 6 1 #ff0000\n"
         "fill 0 1 6 1 #ff0000\nfill 12 1 6 1 #ff0000\n"},
        {"2px dotted #ff0000",
         "fill 0 0 2 1 #ff0000\nfill 4 0 2 1 #ff0000\nfill 8 0 2 1 #ff0000\n"
         "fill 12 0 2 1 #ff0000\nfill 16 0 2 1 #ff0000\n"
         "fill 0 1 2 1 #ff0000\nfill 4 1 2 1 #ff0000\nfill 8 1 2 1 #ff0000\n"
         "fill 12 1 2 1 #ff0000\nfill 16 1 2 1 #ff0000\n"},
        {"3px double #ff0000", "fill 0 0 20 1 #ff0000\nfill 0 2 20 1 #ff0000\n"},
        {"2px ridge #808080", "fill 0 0 20 1 #bfbfbf\nfill 0 1 20 1 #404040\n"},
    };
    for (size_t k = 0; k < sizeof(kEdges) / sizeof(kEdges[0]); k++) {
        char html[256], want[512];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body { margin: 0 } div { width: 20px; height: 0;"
                 " border-top: %s }</style><div></div>",
                 kEdges[k].style);
        snprintf(want, sizeof(want), "fill 0 0 200 100 #fefefe\n%s", kEdges[k].want);
        paint_page_case("a border style is drawn as it says", html, 200,
                        (os64_gui_rect_t){0, 0, 200, 100}, true, want);
    }

    // A rounded corner, worked in 1/256 px: a 4px red square with 2px
    // corners. Row 0's middle is 384 above where the curve ends, so it
    // is in by 512 - isqrt(512^2 - 384^2) = 174: its whole pixels 1 and 2,
    // and pixels 0 and 3 at the 82 of 256 the span covers — alpha 81. Row
    // 1 is 128 above, in by 17: pixels 0 and 3 at 239 of 256, alpha 238.
    // The bottom rows mirror them.
    paint_page_case("a rounded background follows its corners, smoothed across",
                    "<!doctype html><style>body { margin: 0 } div { width: 4px; height: 4px;"
                    " border-radius: 2px; background: #ff0000 }</style><div></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "fill 1 0 2 1 #ff0000\nfill 0 0 1 1 #ff0000/81\nfill 3 0 1 1 #ff0000/81\n"
                    "fill 1 1 2 1 #ff0000\nfill 0 1 1 1 #ff0000/238\nfill 3 1 1 1 #ff0000/238\n"
                    "fill 1 2 2 1 #ff0000\nfill 0 2 1 1 #ff0000/238\nfill 3 2 1 1 #ff0000/238\n"
                    "fill 1 3 2 1 #ff0000\nfill 0 3 1 1 #ff0000/81\nfill 3 3 1 1 #ff0000/81\n");

    // A square shadow with no blur is rectangles: its shape (20x10 moved
    // 5, 5 — so 5..25 by 5..15) less the border box (0..20 by 0..10)
    // leaves a band below and a band at the right.
    paint_page_case("an unblurred square shadow is its shape less the box",
                    "<!doctype html><style>body { margin: 0 } div { width: 20px; height: 10px;"
                    " box-shadow: 5px 5px #ff0000 }</style><div></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "fill 5 10 20 5 #ff0000\n"
                    "fill 20 5 5 5 #ff0000\n");
    // A text's shadow is the run again in the shadow's colour, moved by its
    // offset, painted under it.
    paint_page_case("a text shadow is the run again, under it",
                    "<!doctype html><style>p { text-shadow: 1px 2px #ff0000 }</style><p>Hi", 100,
                    (os64_gui_rect_t){0, 0, 100, 50}, true,
                    "fill 0 0 100 50 #fefefe\n"
                    "text \"Hi\" 9 32 #ff0000\n"
                    "text \"Hi\" 8 30 #101010\n");

    // Groups nest, each laid over what was under it when it opened, and a
    // group none of which shows opens empty: the far one at 520, cut to a
    // view that ends at 100, has no height.
    paint_page_case("groups nest; one out of view opens empty",
                    "<!doctype html><style>body { margin: 0 } div { opacity: 0.5 }"
                    " .in { height: 20px; background: #0000ff }"
                    " .far { margin-top: 500px; height: 20px }</style>"
                    "<div><div class=in></div></div><div class=far></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "group 0 0 200 20\n"
                    "group 0 0 200 20\n"
                    "fill 0 0 200 20 #0000ff\n"
                    "end group 128\n"
                    "end group 128\n"
                    "group 0 520 200 0\n"
                    "end group 128\n");
}
