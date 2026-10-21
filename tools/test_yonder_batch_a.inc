// The Modern Web arc's slice 4, CSS batch A (MODERN_WEB_BRIEFS.md § Slice
// 4): what the painter draws for each property that is drawn, worked by
// hand as test_yonder_cases.inc's are. The view is 200 x 100.

// A picture's place in its content box (yonder_object_rect), each worked
// by hand: the box 10, 20, 100 x 50; a square picture 100 x 100, and a
// small one 40 x 30. A percentage position is of the room left, which is
// negative where the picture is the bigger.
static bool object_rect_is(flow_object_fit_t fit, flow_length_t x, flow_length_t y, uint32_t iw,
                           uint32_t ih, int32_t ax, int32_t ay, int32_t aw, int32_t ah)
{
    flow_style_t s;
    memset(&s, 0, sizeof(s));
    s.object_fit = fit;
    s.object_position[0] = x;
    s.object_position[1] = y;
    os64_gui_rect_t r = yonder_object_rect(&s, (os64_gui_rect_t){10, 20, 100, 50}, iw, ih);
    return r.x == ax && r.y == ay && r.w == aw && r.h == ah;
}

static void object_rect_cases(void)
{
    const flow_length_t half = {FLOW_LENGTH_PERCENT, 50 * 64, 0};
    const flow_length_t zero = {FLOW_LENGTH_PERCENT, 0, 0};
    // `right 10px` and `bottom 0`: 100% less 10px, and 100%.
    const flow_length_t right10 = {FLOW_LENGTH_PERCENT, 100 * 64, -10 * 64};
    const flow_length_t bottom = {FLOW_LENGTH_PERCENT, 100 * 64, 0};
    expect("object-fit: fill is the content box",
           object_rect_is(FLOW_OBJECT_FILL, half, half, 100, 100, 10, 20, 100, 50), NULL);
    // The square is taller in shape than the box: it fits by the height,
    // 50 x 50, centred in the 50 left across: x 35.
    expect("object-fit: contain fits inside, centred",
           object_rect_is(FLOW_OBJECT_CONTAIN, half, half, 100, 100, 35, 20, 50, 50), NULL);
    // And covers by the width, 100 x 100, half the -50 left down: y -5.
    expect("object-fit: cover covers, centred, past the box",
           object_rect_is(FLOW_OBJECT_COVER, half, half, 100, 100, 10, -5, 100, 100), NULL);
    expect("object-position: cover from the top left",
           object_rect_is(FLOW_OBJECT_COVER, zero, zero, 100, 100, 10, 20, 100, 100), NULL);
    // 40 x 30 at its size, centred: 30 and 10 in.
    expect("object-fit: none is its own size",
           object_rect_is(FLOW_OBJECT_NONE, half, half, 40, 30, 40, 30, 40, 30), NULL);
    expect("object-fit: scale-down is none when that fits",
           object_rect_is(FLOW_OBJECT_SCALE_DOWN, half, half, 40, 30, 40, 30, 40, 30), NULL);
    expect("object-fit: scale-down is contain when none does not",
           object_rect_is(FLOW_OBJECT_SCALE_DOWN, half, half, 100, 100, 35, 20, 50, 50), NULL);
    // 60 left across, less 10: 10 + 50; 20 down: 20 + 20.
    expect("object-position: an edge and an offset",
           object_rect_is(FLOW_OBJECT_NONE, right10, bottom, 40, 30, 60, 40, 40, 30), NULL);
    // A 1 x 10000000 picture covers the box 100 wide and a billion tall,
    // held to a quarter of an int32_t (536870911); at 100% down its top is
    // 20 + 50 - that, inside the hold.
    expect("object-fit: cover of an extreme picture is held",
           object_rect_is(FLOW_OBJECT_COVER, zero, bottom, 1, 10000000, 10, 70 - 536870911, 100,
                          536870911), NULL);
    expect("object-fit: a picture with no size fills",
           object_rect_is(FLOW_OBJECT_CONTAIN, half, half, 0, 30, 10, 20, 100, 50), NULL);
}

static void batch_a_paint_cases(void)
{
    object_rect_cases();
    // text-overflow: the div is 40 wide, its line abcdefghij 68. The
    // ellipsis is 8 (16px serif), so what is kept ends by 32: abcd, a-d
    // 8 each. The text is drawn cut there, and the ellipsis at 32 on the
    // line's baseline, in the div's colour.
    paint_page_case("text-overflow: an ellipsis ends a line that passes its end",
                    "<!doctype html><style>body { margin: 0 } div { width: 40px; overflow: hidden;"
                    " white-space: nowrap; text-overflow: ellipsis; color: #00ff00 }</style>"
                    "<div>abcdefghij</div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "text \"\" 32 14 #00ff00 clip 0 0 40 20\n"
                    "text \"abcdefghij\" 0 14 #00ff00 clip 0 0 32 20\n");
    // Across boxes: ab (16), then the b's cdefgh from 16; the limit 32
    // keeps cd of it, to 32, which is where its ellipsis goes; the i
    // element past the limit is not drawn. A line that fits has none, and
    // nor has a block whose overflow is visible: its text runs on.
    paint_page_case("text-overflow: the ellipsis after the last box kept, none where it fits",
                    "<!doctype html><style>body { margin: 0 } div { width: 40px; overflow: hidden;"
                    " white-space: nowrap; text-overflow: ellipsis } p { margin: 0; width: 40px;"
                    " white-space: nowrap; text-overflow: ellipsis }</style>"
                    "<div>ab<b>cdefgh</b><i>xyz</i></div><div>abc</div><p>abcdefghij</p>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "text \"ab\" 0 14 #101010 clip 0 0 40 20\n"
                    "text \"\" 32 14 #101010 clip 0 0 40 20\n"
                    "text \"cdefgh\" 16 14 #101010 clip 0 0 32 20\n"
                    "text \"abc\" 0 34 #101010 clip 0 20 40 20\n"
                    "text \"abcdefghij\" 0 54 #101010\n");
    // An outline is a ring round the border box (5, 5, 20 x 10), out by its
    // offset, 1, and 2 wide: -3 a side from the box to 2, 2, 26 x 16,
    // drawn as a square 2px border is, after the box's background.
    paint_page_case("outline: a ring past the border box, by its offset",
                    "<!doctype html><style>body { margin: 0 } div { margin: 5px; width: 20px;"
                    " height: 10px; background: #0000ff; outline: 2px solid #ff0000;"
                    " outline-offset: 1px }</style><div></div>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    "fill 5 5 20 10 #0000ff\n"
                    // The top two rows and the bottom two, each giving up a
                    // column at each end to the sides on the diagonal.
                    "fill 2 2 26 1 #ff0000\nfill 3 3 24 1 #ff0000\n"
                    "fill 2 17 26 1 #ff0000\nfill 3 16 24 1 #ff0000\n"
                    // The left two columns, and the right two.
                    "fill 2 3 1 14 #ff0000\nfill 3 4 1 12 #ff0000\n"
                    "fill 27 3 1 14 #ff0000\nfill 26 4 1 12 #ff0000\n");
    // A dotted one is a border's dots; a style of none draws nothing, nor
    // does the outline of an inline box's piece (booked).
    paint_page_case("outline: dotted as a border is, none not at all",
                    "<!doctype html><style>body { margin: 0 } div { margin: 4px; width: 8px; height: 2px;"
                    " outline: 2px dotted #ff0000 } p { margin: 0; height: 4px;"
                    " outline: 3px none #00ff00 }</style><div></div><p></p>",
                    200, (os64_gui_rect_t){0, 0, 200, 100}, true,
                    "fill 0 0 200 100 #fefefe\n"
                    // The ring 2, 2, 12 x 6. Its rows: squares 2 across, 2
                    // apart, from its edge; the inner row gives up a column
                    // at each end, cutting the first square to one.
                    "fill 2 2 2 1 #ff0000\nfill 6 2 2 1 #ff0000\nfill 10 2 2 1 #ff0000\n"
                    "fill 3 3 1 1 #ff0000\nfill 6 3 2 1 #ff0000\nfill 10 3 2 1 #ff0000\n"
                    "fill 2 7 2 1 #ff0000\nfill 6 7 2 1 #ff0000\nfill 10 7 2 1 #ff0000\n"
                    "fill 3 6 1 1 #ff0000\nfill 6 6 2 1 #ff0000\nfill 10 6 2 1 #ff0000\n"
                    // The sides' outer columns, 3..7 down: the squares from
                    // the top edge meet them at 3 and 6; the inner columns,
                    // 4..6, meet none.
                    "fill 2 3 1 1 #ff0000\nfill 2 6 1 1 #ff0000\n"
                    "fill 13 3 1 1 #ff0000\nfill 13 6 1 1 #ff0000\n");
}
