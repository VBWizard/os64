// Masks (MASKS.md), on the recorder: a masked group is a layer, each mask
// layer a capture over it taken back as coverage (the recorder lays the
// fills and pixels painted into a capture, as the face does), and the
// layer closed with the coverage they came to. Small boxes, so every
// coverage byte can be worked by hand; s_record_all writes the rows.

static char *mask_paint(const char *html, int32_t width, os64_gui_rect_t view)
{
    bool escaped = false;
    s_record_all = true;
    char *got = paint_of(html, strlen(html), width, view, &escaped, true);
    s_record_all = false;
    expect("mask: nothing reaches past the viewport", !escaped, html);
    return got;
}

static void mask_case(const char *name, const char *css, const char *expected)
{
    char html[1024];
    snprintf(html, sizeof(html), "<!doctype html><style>body { margin: 0 } .m { %s }</style>"
             "<div class=m></div>", css);
    char *got = mask_paint(html, 200, (os64_gui_rect_t){0, 0, 200, 100});
    expect(name, got != NULL && strcmp(got, expected) == 0, got);
    free(got);
}

static int count_of(const char *got, const char *what)
{
    int n = 0;
    for (const char *at = got; at != NULL && (at = strstr(at, what)) != NULL; at++)
        n++;
    return n;
}

static void mask_paint_cases(void)
{
    // A fade: the gradient's alpha at each pixel's centre, 7/8 to 1/8 of
    // 255, is the coverage.
    mask_case("mask: a linear fade's alpha is the coverage",
              "width: 4px; height: 1px; background: #00ff00;"
              " mask-image: linear-gradient(to right, #000, transparent)",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 4 1\n"
              "fill 0 0 4 1 #00ff00\n"
              "filter 0 0 4 1\n"
              "pixels 0 0 4 1 df000000 9f000000 60000000 20000000\n"
              "take\n"
              "end filter 0 #101010 255 cover 510\n"
              "df9f6020\n");
    // Placed as a background layer is: 2 x 1 at the right, its spare
    // pixel of height split at 50%, and not repeated.
    mask_case("mask: a layer's position, size and repeat place it",
              "width: 4px; height: 2px; background: #00ff00;"
              " -webkit-mask: linear-gradient(#000 0 0) right / 2px 1px no-repeat",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 4 2\n"
              "fill 0 0 4 2 #00ff00\n"
              "filter 0 0 4 2\n"
              "pixels 0 0 4 2 00000000 00000000 00000000 00000000 00000000 00000000 ff000000 ff000000\n"
              "take\n"
              "end filter 0 #101010 255 cover 510\n"
              "00000000\n"
              "0000ffff\n");
    // The spinners' ring: the content box excluded from the border box
    // leaves the padding, one pixel all round. The bottom layer first.
    mask_case("mask: exclude cuts the content box out of the border box",
              "width: 6px; height: 4px; padding: 1px; background: #00ff00;"
              " mask: linear-gradient(#000 0 0) content-box, linear-gradient(#000 0 0);"
              " mask-composite: exclude",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 8 6\n"
              "fill 0 0 8 6 #00ff00\n"
              "filter 0 0 8 6\n"
              "pixels 0 0 8 6 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000\n"
              "take\n"
              "filter 0 0 8 6\n"
              "pixels 1 1 6 4 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000\n"
              "take\n"
              "end filter 0 #101010 255 cover 6120\n"
              "ffffffffffffffff\n"
              "ff000000000000ff\n"
              "ff000000000000ff\n"
              "ff000000000000ff\n"
              "ff000000000000ff\n"
              "ffffffffffffffff\n");
    // Luminance: white to black, the grey at each centre.
    mask_case("mask: luminance reads the grey",
              "width: 4px; height: 1px; background: #00ff00;"
              " mask: linear-gradient(to right, #fff, #000) luminance",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 4 1\n"
              "fill 0 0 4 1 #00ff00\n"
              "filter 0 0 4 1\n"
              "pixels 0 0 4 1 ffdfdfdf ff9f9f9f ff606060 ff202020\n"
              "take luminance\n"
              "end filter 0 #101010 255 cover 510\n"
              "df9f6020\n");
    // A picture is the face's, its layer flagged; one not yet arrived
    // draws nothing into the capture, so the box is hidden until it comes
    // (MASKS.md § 10, ruling 2).
    mask_case("mask: a picture not yet arrived hides the box",
              "width: 2px; height: 1px; background: #00ff00; mask: url(icon.svg)",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 2 1\n"
              "fill 0 0 2 1 #00ff00\n"
              "filter 0 0 2 1\n"
              "backdrop 0 0 2 1 mask 0 origin 0 0 2 1\n"
              "take\n"
              "end filter 0 #101010 255 cover 0\n"
              "0000\n");
    // A mask that hides the box whole paints nothing, and opens no layer.
    mask_case("mask: one that hides the box whole paints nothing",
              "width: 4px; height: 4px; background: #00ff00;"
              " mask: linear-gradient(transparent, #0000), none",
              "fill 0 0 200 100 #fefefe\n");
    mask_case("mask: one sized to nothing paints nothing",
              "width: 4px; height: 4px; background: #00ff00;"
              " mask: linear-gradient(#000 0 0) 0 0 / 0 4px",
              "fill 0 0 200 100 #fefefe\n");
    // A reference to an SVG <mask> is not drawn: the box is drawn plain,
    // in the group the mask makes, and not counted.
    mask_case("mask: a reference to an SVG mask is drawn as none",
              "width: 4px; height: 4px; background: #00ff00; mask: url(#m)",
              "fill 0 0 200 100 #fefefe\n"
              "group 0 0 4 4\n"
              "fill 0 0 4 4 #00ff00\n"
              "end group 255\n");
    expect("mask: a reference is not counted refused", yonder_paint_layers_refused == 0, NULL);
    // Only `none` is no mask: no group at all.
    mask_case("mask: none is no mask",
              "width: 4px; height: 4px; background: #00ff00; mask: none",
              "fill 0 0 200 100 #fefefe\n"
              "fill 0 0 4 4 #00ff00\n");
    // A shadow outside the border box is outside the mask (MASKS.md § 3):
    // the layer is the border box, not the shadow's reach, so the shadow
    // is not painted at all; the opacity lays it.
    mask_case("mask: the layer is cut to the border box, laid at the opacity",
              "width: 2px; height: 1px; box-shadow: 0 0 0 3px red; opacity: 0.5;"
              " mask: linear-gradient(#000 0 0)",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 2 1\n"
              "filter 0 0 2 1\n"
              "pixels 0 0 2 1 ff000000 ff000000\n"
              "take\n"
              "end filter 0 #101010 128 cover 510\n"
              "ffff\n");
    // A layer cut to no box lets the group reach past its border box: the
    // shadow 1px all round is in the layer, and the mask's one layer
    // repeated covers it.
    mask_case("mask: no-clip lets the layer reach past the border box",
              "margin: 1px; width: 2px; height: 1px; box-shadow: 0 0 0 1px red;"
              " mask: linear-gradient(#000 0 0) no-clip",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 4 3\n"
              "fill 0 0 4 1 #ff0000\n"
              "fill 0 2 4 1 #ff0000\n"
              "fill 0 1 1 1 #ff0000\n"
              "fill 3 1 1 1 #ff0000\n"
              "filter 0 0 4 3\n"
              "pixels 0 0 4 3 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000 ff000000"
              " ff000000 ff000000 ff000000 ff000000 ff000000\n"
              "take\n"
              "end filter 0 #101010 255 cover 3060\n"
              "ffffffff\n"
              "ffffffff\n"
              "ffffffff\n");
    // The bottom layer's operator is ignored: there is nothing below it
    // to intersect with (Masking 1 § 7.8).
    mask_case("mask: the bottom layer's operator is ignored",
              "width: 2px; height: 1px; mask: linear-gradient(#000 0 0) intersect",
              "fill 0 0 200 100 #fefefe\n"
              "filter 0 0 2 1\n"
              "filter 0 0 2 1\n"
              "pixels 0 0 2 1 ff000000 ff000000\n"
              "take\n"
              "end filter 0 #101010 255 cover 510\n"
              "ffff\n");
    // A rounded box rounds its mask: a 4 x 4 circle covers its corners
    // less than its middle.
    char *got = mask_paint("<!doctype html><style>body { margin: 0 } .m { width: 4px; height: 4px;"
                           " border-radius: 50%; mask: linear-gradient(#000 0 0) }</style>"
                           "<div class=m></div>",
                           200, (os64_gui_rect_t){0, 0, 200, 100});
    const char *rows = got != NULL ? strstr(got, " cover ") : NULL;
    rows = rows != NULL ? strchr(rows, '\n') : NULL;
    expect("mask: a rounded box's mask is rounded",
           rows != NULL && strncmp(rows + 1, "ff", 2) != 0 && strncmp(rows + 3, "ffff", 4) == 0,
           got);
    free(got);
    // A plain inline's mask is not drawn (flow_masked), as its opacity is.
    got = mask_paint("<!doctype html><style>body { margin: 0 } span { background: #00ff00;"
                     " mask: linear-gradient(#000 0 0) }</style><span>a</span>",
                     200, (os64_gui_rect_t){0, 0, 200, 100});
    expect("mask: a plain inline's is not drawn", got != NULL && strstr(got, "filter ") == NULL, got);
    free(got);
    // Nor an inline-block's, which is no stacked box (DEBTS.md).
    got = mask_paint("<!doctype html><style>body { margin: 0 } span { display: inline-block;"
                     " width: 4px; height: 4px; background: #00ff00; mask: linear-gradient(#000 0 0) }"
                     "</style><span></span>",
                     200, (os64_gui_rect_t){0, 0, 200, 100});
    expect("mask: an inline-block's is not drawn", got != NULL && strstr(got, "filter ") == NULL, got);
    free(got);
    // Past the paint's budget (MASKS.md § 10, ruling 1): a box the size of
    // the view costs itself and a capture, twice the view, which is all of
    // it, so the second and the fourth are drawn plain, each counted — but
    // the third, which hides its box whole, still paints nothing: its text
    // is cut to no view at all.
    static const char kThree[] =
        "<!doctype html><style>body { margin: 0 } div { position: absolute; top: 0;"
        " width: 200px; height: 100px; background: #00ff00; mask: linear-gradient(#000 0 0) }"
        " .h { mask: linear-gradient(#0000, #0000) }</style>"
        "<div>a</div><div>b</div><div class=h>c</div><div>d</div>";
    yonder_paint_layers_refused = 0;
    bool escaped = false;
    got = paint_of(kThree, strlen(kThree), 200, (os64_gui_rect_t){0, 0, 200, 100}, &escaped, true);
    expect("mask: past the budget, drawn plain and counted; a hiding mask draws nothing",
           count_of(got, "\nend filter ") == 1 && count_of(got, "take\n") == 1 &&
               count_of(got, "\ngroup ") == 2 &&
               strstr(got, "text \"c\" 0 14 #101010 clip 0 0 0 0\n") != NULL &&
               yonder_paint_layers_refused == 2,
           got);
    free(got);
    yonder_paint_layers_refused = 0;
    // A face that cannot make a capture lays the group unmasked, counted.
    s_filter_refused = true;
    got = mask_paint("<!doctype html><style>body { margin: 0 } .m { width: 2px; height: 1px;"
                     " mask: linear-gradient(#000 0 0) }</style><div class=m></div>",
                     200, (os64_gui_rect_t){0, 0, 200, 100});
    s_filter_refused = false;
    expect("mask: no layer to be had, drawn plain and counted",
           got != NULL && strstr(got, "filter ") == NULL && yonder_paint_layers_refused == 1, got);
    free(got);
    yonder_paint_layers_refused = 0;
}
