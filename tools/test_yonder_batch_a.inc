// The Modern Web arc's slice 4, CSS batch A (MODERN_WEB_BRIEFS.md § Slice
// 4): what the painter draws for each property that is drawn, worked by
// hand as test_yonder_cases.inc's are. The view is 200 x 100.

static void batch_a_paint_cases(void)
{
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
