// tools/test_libflow_floats.inc — floats (FLOATS.md), included by
// test_libflow_host.c. Every expected geometry is worked by hand from CSS
// 2.1 § 9.5.1 and the Decisions, never captured from the engine
// (LAYOUT.md § Proof). Sized boxes, body margin 0, a 400px page, so the
// arithmetic is the spec's and not the font's.

#define FLOAT_STYLE "<!doctype html><style>body{margin:0} p{margin:0}"

// Where floats go. A container holding nothing but floats holds no block
// in the flow, so it is an inline formatting context whose one line is
// empty (FLOATS.md § Pass 3) and the floats are met on it; the first case
// meets them among blocks. Either way the exclusion space places them.
static void float_place_cases(void)
{
    // A left float and a right float at the top, side by side: the left
    // one at the left content edge, the right one's right margin edge at
    // the right content edge, 400 - 80 = 320. The paragraph after them
    // ignores both (§ 9.5: a block box in the flow does not see floats)
    // and starts at the top. Neither float adds to the body's height —
    // the body is not a formatting context root — but the page reaches
    // the left one's bottom.
    cascade_layout_case("floats: a left and a right float beside a block",
        FLOAT_STYLE ".l{float:left;width:100px;height:50px}"
        ".r{float:right;width:80px;height:30px} .p{height:20px}</style>"
        "<div class=l></div><div class=r></div><p class=p></p>", 400,
        "page 400 50\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block div 0 0 100 50 float-left\n"
        "    block div 320 0 80 30 float-right\n"
        "    block p 0 0 400 20\n");

    // Three 150px left floats in 400px: two fit side by side (300), the
    // third has 100 left beside them and goes down — not to 20, where the
    // first ends, since the second still stands beside it there, but to
    // 30, where nothing does — and as far left as it can (§ 9.5.1 rules
    // 2, 7 and 8).
    cascade_layout_case("floats: three left floats wrap to a second row",
        FLOAT_STYLE ".a{float:left;width:150px;height:20px}"
        ".b{float:left;width:150px;height:30px} .c{float:left;width:150px;height:10px}</style>"
        "<div class=a></div><div class=b></div><div class=c></div>", 400,
        "page 400 40\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 150 20 float-left\n"
        "    block div 150 0 150 30 float-left\n"
        "    block div 0 30 150 10 float-left\n");

    // § 9.5.1 rule 3: a left float may not pass a RIGHT float beside it.
    // The third float (120) fits beside the first (150 from the left) but
    // not between it and the right float (150 from the right: 100 left at
    // y 0), so it goes down to 20, where the right float ends, and sits at
    // 150. Fitted against its own side alone it would sit at 150, 0, on
    // top of the right float.
    cascade_layout_case("floats: a left float fits between both sides",
        FLOAT_STYLE ".l{float:left;width:150px;height:50px}"
        ".r{float:right;width:150px;height:20px} .m{float:left;width:120px;height:10px}</style>"
        "<div class=l></div><div class=r></div><div class=m></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 150 50 float-left\n"
        "    block div 250 0 150 20 float-right\n"
        "    block div 150 20 120 10 float-left\n");

    // § 9.5.1 rules 5 and 6: no float's top is above an earlier float's.
    // The right float (200) does not fit beside the 300px left one, so it
    // goes to 100; the left float after it, which would fit at 0, may go
    // no higher than 100, and there nothing stands on its side.
    cascade_layout_case("floats: none goes above an earlier float's top",
        FLOAT_STYLE ".a{float:left;width:300px;height:100px}"
        ".b{float:right;width:200px;height:10px} .c{float:left;width:50px;height:10px}</style>"
        "<div class=a></div><div class=b></div><div class=c></div>", 400,
        "page 400 110\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 300 100 float-left\n"
        "    block div 200 100 200 10 float-right\n"
        "    block div 0 100 50 10 float-left\n");

    // A float wider than its container goes where nothing stands beside
    // it, at its side's edge, and overflows.
    cascade_layout_case("floats: a float wider than its container",
        FLOAT_STYLE ".a{float:left;width:100px;height:10px}"
        ".w{float:left;width:500px;height:10px}</style>"
        "<div class=a></div><div class=w></div>", 400,
        "page 500 20\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 100 10 float-left\n"
        "    block div 0 10 500 10 float-left\n");

    // A float that no float stands beside goes where the flow is, however
    // wide it is: the outer left float (80) stops short of the div's
    // content edge (100), so the 400px float inside it, wider than the
    // 300 there, stands beside nothing and is not moved down past it.
    cascade_layout_case("floats: a float with nothing beside it stays where it is",
        FLOAT_STYLE ".o{float:left;width:80px;height:100px} .in{margin-left:100px}"
        ".w{float:left;width:400px;height:10px}</style>"
        "<div class=o></div><div class=in><div class=w></div></div>", 400,
        "page 500 100\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    block div 0 0 80 100 float-left\n"
        "    block div 100 0 300 0\n"
        "      line 100 0 300 0 base 14\n"
        "      block div 100 0 400 10 float-left\n");

    // A float as wide as nothing changes no edge: the one after it sits
    // where it would have alone.
    cascade_layout_case("floats: a float as wide as nothing",
        FLOAT_STYLE ".z{float:left;width:0;height:50px}"
        ".a{float:left;width:100px;height:10px}</style>"
        "<div class=z></div><div class=a></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 0 50 float-left\n"
        "    block div 0 0 100 10 float-left\n");

    // Margins and the frame: the margin box is what is placed and what the
    // next float stands beside. 10 + 2 + 5 + 50 + 5 + 2 + 20 = 94 across:
    // the border box at 10, 4 (the top margin), 64 x 64; the next float's
    // margin box starts at 94. The page reaches the border box, 68, as it
    // does any box's.
    cascade_layout_case("floats: the margin box is what is placed",
        FLOAT_STYLE ".m{float:left;width:50px;height:50px;padding:5px;border:2px solid;"
        "margin:4px 20px 6px 10px} .a{float:left;width:10px;height:10px}</style>"
        "<div class=m></div><div class=a></div>", 400,
        "page 400 68\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 10 4 64 64 float-left\n"
        "    block div 94 0 10 10 float-left\n");

    // A float is a formatting context of its own (§ 9.4.1): its child's top
    // margin stays inside it, so it is 10 + 10 tall with the paragraph 10
    // down, rather than collapsing out of its top.
    cascade_layout_case("floats: a float holds its content's margins",
        FLOAT_STYLE ".f{float:left;width:50px} .f p{margin-top:10px;height:10px}</style>"
        "<div class=f><p></p></div>", 400,
        "page 400 20\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 50 20 float-left\n"
        "      block p 0 10 50 10\n");

    // An auto width shrinks to fit (§ 10.3.5): the 60px block inside, and
    // its frame — never the whole line. A float's auto margins are 0.
    cascade_layout_case("floats: an auto width shrinks to fit",
        FLOAT_STYLE ".f{float:right;padding:0 5px;margin:0 auto}"
        ".in{width:60px;height:10px}</style>"
        "<div class=f><div class=in></div></div>", 400,
        "page 400 10\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 330 0 70 10 float-right\n"
        "      block div 335 0 60 10\n");
}

static void float_wait_cases(void)
{
    // FLOATS.md, Decision 1: a float waits for the margins above it. The
    // div's 30px top margin collapses through the div (no border, no
    // padding) with whatever comes first inside it, so the div's content
    // top is not known when the float is met; the paragraph's border edge
    // resolves it, at 30, and the float goes there.
    cascade_layout_case("floats: a float waits for the margins above it",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}"
        ".p{height:20px}</style>"
        "<div class=d><div class=l></div><p class=p></p></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 30 400 20\n"
        "    block div 0 30 400 20\n"
        "      block div 0 30 50 10 float-left\n"
        "      block p 0 30 400 20\n");

    // Waiting is not placing at the margins pending when the float is met:
    // the paragraph's own 50px top margin collapses with the div's 30, so
    // the div's content top, and the float, are at 50.
    cascade_layout_case("floats: a float waits for margins met after it",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}"
        "p.q{margin-top:50px;height:20px}</style>"
        "<div class=d><div class=l></div><p class=q></p></div>", 400,
        "page 400 70\n"
        "block html 0 0 400 70\n"
        "  block body 0 50 400 20\n"
        "    block div 0 50 400 20\n"
        "      block div 0 50 50 10 float-left\n"
        "      block p 0 50 400 20\n");

    // A border edge resolves the margins: the paragraph's top border is
    // at 30, and the float goes there, before the paragraph's content.
    cascade_layout_case("floats: a border edge resolves the margins a float waits for",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}"
        ".b{border-top:2px solid;height:20px}</style>"
        "<div class=d><div class=l></div><p class=b></p></div>", 400,
        "page 400 52\n"
        "block html 0 0 400 52\n"
        "  block body 0 30 400 22\n"
        "    block div 0 30 400 22\n"
        "      block div 0 30 50 10 float-left\n"
        "      block p 0 30 400 22\n");

    // So does a line with height: the float written before the text, in
    // the div's own line, goes to the line's top at 30 (the probe's case
    // 1a).
    cascade_layout_case("floats: a line resolves the margins a float waits for",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}</style>"
        "<div class=d><div class=l></div>x</div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 30 400 20\n"
        "    block div 0 30 400 20\n"
        "      line 0 30 400 20 base 14\n"
        "        text \"x\" 0 32 8 16 serif 16\n"
        "      block div 0 30 50 10 float-left\n");

    // A float between two blocks goes below the first one's own bottom
    // margin, 10 + 10 = 20 — not the 40 it collapses to with the next
    // block's top, which has not been met yet (as an absolute box's static
    // position, POSITION.md ruling 5; the probe's case 1d). The next block
    // is at 10 + 40 = 50.
    cascade_layout_case("floats: a float between two blocks",
        FLOAT_STYLE ".a{height:10px;margin-bottom:10px} .l{float:left;width:50px;height:20px}"
        ".b{margin-top:40px;height:10px}</style>"
        "<div class=a></div><div class=l></div><div class=b></div>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    block div 0 0 400 10\n"
        "    block div 0 20 50 20 float-left\n"
        "    block div 0 50 400 10\n");
}

static void float_wait_end_cases(void)
{
    // Laying a float out is not reaching its border edge in the flow: the
    // bordered float is laid out where it collides with nothing, and the
    // float waiting before it must not take that for the margins
    // resolving. Both wait for the paragraph, at 30, and stand side by
    // side there; the second is 20 + 2 + 2 = 24 by 10 + 2 + 2 = 14.
    cascade_layout_case("floats: laying a float out resolves no margins",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}"
        ".b{float:left;width:20px;height:10px;border:2px solid} .p{height:20px}</style>"
        "<div class=d><div class=l></div><div class=b></div><p class=p></p></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 30 400 20\n"
        "    block div 0 30 400 20\n"
        "      block div 0 30 50 10 float-left\n"
        "      block div 50 30 24 14 float-left\n"
        "      block p 0 30 400 20\n");

    // A container holding ONLY a float, with a top margin (Fable's case):
    // an inline formatting context whose one line is empty, so nothing
    // resolves the 10px, which collapses through it and the body to the
    // root's end. The float waits until then and goes where the margins
    // land, at 10.
    cascade_layout_case("floats: a float alone in a container waits to the root's end",
        FLOAT_STYLE ".d{margin-top:10px} .l{float:left;width:50px;height:20px}</style>"
        "<div class=d><div class=l></div></div>", 400,
        "page 400 30\n"
        "block html 0 0 400 10\n"
        "  block body 0 0 400 0\n"
        "    block div 0 10 400 0\n"
        "      line 0 10 400 0 base 14\n"
        "      block div 0 10 50 20 float-left\n");

    // A float's lines are not its list item's first line. In quirks mode a
    // line has no strut, so the item's first line, 400 - 8 - 8 - 40 = 344
    // across, is as tall as the 32px marker it holds open — a 40px font
    // box, its baseline 28 down — while the float's line is its own 10px
    // text's, 12.5 rounding to 13 with the baseline 8.75 down. Taken by
    // the float's line, the marker would hold that open 40 tall too.
    {
        char *got = cascade_layout_of(
            "<style>li{font-size:32px} .f{float:left;font-size:10px} .t{font-size:10px}"
            "</style><ul><li><div class=f>a</div><span class=t>x</span></ul>", 400);
        expect("floats: a float's lines leave its item's marker alone",
               got != NULL && strstr(got, " 344 40 base 28\n") != NULL &&
                   strstr(got, "line 48 8 5 13 base 9\n") != NULL, got);
        free(got);
    }
}

static void float_relative_cases(void)
{
    // A relative box holding a float: the box and the float inside it are
    // drawn at its offset, 10 across and 5 down, while the space records
    // the float unmoved, at 0 0, 50 x 20. The float after the box is met
    // below it, at 10, and stands beside that record at 50 — not at 60,
    // beside where the first is drawn (§ 9.4.3).
    cascade_layout_case("floats: a relative box holding a float",
        FLOAT_STYLE ".r{position:relative;left:10px;top:5px}"
        ".l{float:left;width:50px;height:20px} .p{height:10px}</style>"
        "<div class=r><div class=l></div><p class=p></p></div><div class=l></div>", 400,
        "page 410 30\n"
        "block html 0 0 400 10\n"
        "  block body 0 0 400 10\n"
        "    block div 10 5 400 10 positioned\n"
        "      block div 10 5 50 20 float-left\n"
        "      block p 10 5 400 10\n"
        "    block div 50 10 50 20 float-left\n");

    // A RELATIVE float: placed as a float, then drawn at its offset; what
    // flows round it — the float after it — uses where it was placed.
    cascade_layout_case("floats: a relative float",
        FLOAT_STYLE ".l{float:left;width:50px;height:20px}"
        ".m{position:relative;left:7px;top:3px}</style>"
        "<div class='l m'></div><div class=l></div>", 400,
        "page 400 23\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 7 3 50 20 float-left positioned\n"
        "    block div 50 0 50 20 float-left\n");
}

static void float_style_cases(void)
{
    // A flex item never floats: its container places it (CSS 2.1 § 9.7's
    // float row is not an item's). It is blockified as an item, and its
    // float is gone.
    {
        char *got = cascade_dump_of("<!doctype html><div style=display:flex>"
                                    "<span style=float:left>x</span></div>", 800, 600);
        expect("floats: a flex item floats nowhere",
               got != NULL && strstr(got, "span block specified-inline\n") != NULL &&
                   strstr(got, "float=") == NULL, got);
        free(got);
    }

    // Positioning off (flow_env_t.static_only; FLOATS.md, Ruling 1) lays
    // a float out where it stands, as the block it computes to: full
    // width, and the paragraph after it below it.
    {
        Sheeted p;
        sheeted_open(&p, FLOAT_STYLE ".l{float:left;width:50%;height:10px} .p{height:10px}"
                         "</style><div class=l></div><p class=p></p>", 400, 600);
        p.env.static_only = true;
        flow_tree_t *t = flow_layout(p.doc, p.page, 400, &p.env);
        char got[1024] = "";
        if (t != NULL)
            flow_dump(t, got, sizeof(got));
        expect("floats: positioning off lays a float out where it stands",
               strstr(got, "    block div 0 0 200 10\n    block p 0 10 400 10\n") != NULL &&
                   strstr(got, "float-") == NULL, got);
        flow_free(t);
        sheeted_close(&p);
    }
}

static void float_inline_relative_cases(void)
{
    // A float written inside a RELATIVE INLINE moves with it: placed at
    // the line's top, 0 0, and drawn at the span's offset, 5 across and 3
    // down, as the span's own text is.
    char *got = cascade_layout_of(
        FLOAT_STYLE ".l{float:left;width:50px;height:10px}"
        ".r{position:relative;left:5px;top:3px}</style>"
        "<span class=r><span class=l></span>x</span>", 400);
    expect("floats: a float in a relative inline moves with it",
           got != NULL && strstr(got, "block span 5 3 50 10 float-left\n") != NULL, got);
    free(got);
}

static void float_sweep_cases(void)
{
    // Floats among blocks and inline, waiting for margins, relative, and
    // inside a float: a stop anywhere leaves each placed float where the
    // whole layout puts it, or held to (d) (FLOATS.md § Proof). The first
    // float waits for margins that land at 30, where the paragraph's own
    // top margin takes them; a tree cut before the paragraph has only the
    // div's 20, so a float placed there would be in the wrong place.
    layout_relation_sweep("a float page",
        "<!doctype html><style>.l{float:left;width:60px} .r{float:right;width:40px}"
        ".m{margin-top:20px} .rel{position:relative;top:4px}</style>"
        "<div class=m style=padding-bottom:1px><div class=l>one two</div>"
        "<p style=margin-top:30px>text beside <img align=right width=20 height=20>"
        "it and more</div><div class='l rel'><div class=r>in</div>x</div>"
        "<p class=m>after <span class=r>right</span> the floats</p>", 300);
}

static void float_cases(void)
{
    float_style_cases();
    float_place_cases();
    float_wait_cases();
    float_wait_end_cases();
    float_relative_cases();
    float_inline_relative_cases();
    float_sweep_cases();
}
