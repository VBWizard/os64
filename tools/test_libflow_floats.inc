// tools/test_libflow_floats.inc — floats (FLOATS.md), included by
// test_libflow_host.c. Every expected geometry is worked by hand from CSS
// 2.1 § 9.5.1 and the Decisions, never captured from the engine
// (LAYOUT.md § Proof). Sized boxes, body margin 0, a 400px page, so the
// arithmetic is the spec's and not the font's. The root is a formatting
// context's root, so it grows to hold its floats' margin boxes (§ 10.6.7)
// while the body, which is not one, does not.

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
        "block html 0 0 400 50\n"
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
        "block html 0 0 400 40\n"
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
        "block html 0 0 400 50\n"
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
        "block html 0 0 400 110\n"
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
        "block html 0 0 400 20\n"
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
        "block html 0 0 400 100\n"
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
        "block html 0 0 400 50\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 0 50 float-left\n"
        "    block div 0 0 100 10 float-left\n");

    // Margins and the frame: the margin box is what is placed and what the
    // next float stands beside. 10 + 2 + 5 + 50 + 5 + 2 + 20 = 94 across:
    // the border box at 10, 4 (the top margin), 64 x 64; the next float's
    // margin box starts at 94. The root holds the margin box, 74.
    cascade_layout_case("floats: the margin box is what is placed",
        FLOAT_STYLE ".m{float:left;width:50px;height:50px;padding:5px;border:2px solid;"
        "margin:4px 20px 6px 10px} .a{float:left;width:10px;height:10px}</style>"
        "<div class=m></div><div class=a></div>", 400,
        "page 400 74\n"
        "block html 0 0 400 74\n"
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
        "block html 0 0 400 20\n"
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
        "block html 0 0 400 10\n"
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
    // 1a), and the line, broken again beside it, starts at 50.
    cascade_layout_case("floats: a line resolves the margins a float waits for",
        FLOAT_STYLE ".d{margin-top:30px} .l{float:left;width:50px;height:10px}</style>"
        "<div class=d><div class=l></div>x</div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 30 400 20\n"
        "    block div 0 30 400 20\n"
        "      line 50 30 350 20 base 14\n"
        "        text \"x\" 50 32 8 16 serif 16\n"
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
    // an inline formatting context whose one line is empty, so nothing in
    // it resolves the 10px, which collapses through it. The empty box,
    // having met a float, places it where it stands, at 10 (the probe's
    // case 1c).
    cascade_layout_case("floats: a float alone in a container stands where it does",
        FLOAT_STYLE ".d{margin-top:10px} .l{float:left;width:50px;height:20px}</style>"
        "<div class=d><div class=l></div></div>", 400,
        "page 400 30\n"
        "block html 0 0 400 30\n"
        "  block body 0 0 400 0\n"
        "    block div 0 10 400 0\n"
        "      line 0 10 400 0 base 14\n"
        "      block div 0 10 50 20 float-left\n");

    // A float's lines are not its list item's first line. In quirks mode a
    // line has no strut, so the item's first line — beside the 5px float,
    // 400 - 8 - 8 - 40 - 5 = 339 across — is as tall as the 32px marker it
    // holds open — a 40px font
    // box, its baseline 28 down — while the float's line is its own 10px
    // text's, 12.5 rounding to 13 with the baseline 8.75 down. Taken by
    // the float's line, the marker would hold that open 40 tall too.
    {
        char *got = cascade_layout_of(
            "<style>li{font-size:32px} .f{float:left;font-size:10px} .t{font-size:10px}"
            "</style><ul><li><div class=f>a</div><span class=t>x</span></ul>", 400);
        expect("floats: a float's lines leave its item's marker alone",
               got != NULL && strstr(got, " 339 40 base 28\n") != NULL &&
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
        "block html 0 0 400 30\n"
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
        "block html 0 0 400 20\n"
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

// Ten letters, 80px in the harness face; a space is 4.
#define W10 "aaaaaaaaaa"

static void float_line_cases(void)
{
    // Lines beside a 100 x 30 float are 300 wide from 100: three words fit
    // (80 + 4 + 80 + 4 + 80 = 248; a fourth would be 332). The first two
    // lines, 0 and 20, are beside it; the third, at 40, is below it and
    // has the whole 400, where four fit.
    cascade_layout_case("floats: lines beside a float are shortened",
        FLOAT_STYLE ".l{float:left;width:100px;height:30px}</style>"
        "<div class=l></div><p>" W10 " " W10 " " W10 " " W10 " " W10 " " W10 " " W10 " "
        W10 " " W10 " " W10 "</p>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    block div 0 0 100 30 float-left\n"
        "    block p 0 0 400 60\n"
        "      line 100 0 300 20 base 14\n"
        "        text \"" W10 " " W10 " " W10 "\" 100 2 248 16 serif 16\n"
        "      line 100 20 300 20 base 14\n"
        "        text \"" W10 " " W10 " " W10 "\" 100 22 248 16 serif 16\n"
        "      line 0 40 400 20 base 14\n"
        "        text \"" W10 " " W10 " " W10 " " W10 "\" 0 42 332 16 serif 16\n");

    // A line with no room beside the floats for its first word moves down
    // to where they end: 50 left beside the 350px float, the word is 80,
    // so the line goes to 30. The paragraph's top stays at 0, where its
    // margins resolved.
    cascade_layout_case("floats: a line with no room for its first word moves down",
        FLOAT_STYLE ".l{float:left;width:350px;height:30px}</style>"
        "<div class=l></div><p>" W10 "</p>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block div 0 0 350 30 float-left\n"
        "    block p 0 0 400 50\n"
        "      line 0 30 400 20 base 14\n"
        "        text \"" W10 "\" 0 32 80 16 serif 16\n");

    // A centred line centres in the room beside the float: 100 + (300 -
    // 80) / 2 = 210.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:30px} p{text-align:center}</style>"
            "<div class=l></div><p>" W10 "</p>", 400);
        expect("floats: a centred line centres beside the float",
               got != NULL && strstr(got, "text \"" W10 "\" 210 2 80 16 serif 16\n") != NULL,
               got);
        free(got);
    }

    // A float in the middle of a line goes on it when its margin box fits
    // beside what is already there (FLOATS.md, Decision 2): after three
    // words the pen is at 252, and 252 + 100 fits in 400, so the float
    // stands at the line's top at 300, and the line, broken again in the
    // 300 left, ends before the fourth word.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".r{float:right;width:100px;height:10px}</style>"
            "<p>" W10 " " W10 " " W10 " <span class=r></span>" W10 " " W10 "</p>", 400);
        expect("floats: a float that fits goes on its line",
               got != NULL && strstr(got, "      line 0 0 300 20 base 14\n") != NULL &&
                   strstr(got, "block span 300 0 100 10 float-right\n") != NULL &&
                   strstr(got, "      line 0 20 400 20 base 14\n") != NULL, got);
        free(got);
    }

    // A line broken again starts as the first breaking did: its text-indent
    // (the first word at 20) and the inline open across it (one piece of
    // the b on each of the two lines, not a second one opened again). 20 +
    // 252 + 100 fits in 400, so the float goes on the line, and 20 + 248
    // fits in the 300 left, the line's last space collapsing.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".r{float:right;width:100px;height:10px} p{text-indent:20px}</style>"
            "<p><b>" W10 " " W10 " " W10 " <span class=r></span>" W10 " " W10 "</b></p>", 400);
        int pieces = 0;
        for (const char *at = got; at != NULL && (at = strstr(at, " span b ")) != NULL; at++)
            pieces++;
        expect("floats: a line broken again starts as it did",
               got != NULL && strstr(got, "      line 0 0 300 20 base 14\n") != NULL &&
                   strstr(got, "text \"" W10 " " W10 " " W10 "\" 20 2 248 16 ") != NULL &&
                   pieces == 2,
               got);
        free(got);
    }

    // One that does not fit goes to the top of the next line: 252 + 200 is
    // past 400, so the first line keeps four words across the whole 400,
    // the float stands at 20, at 200, and the second line is beside it.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".r{float:right;width:200px;height:10px}</style>"
            "<p>" W10 " " W10 " " W10 " <span class=r></span>" W10 " " W10 "</p>", 400);
        expect("floats: a float that does not fit goes below its line",
               got != NULL && strstr(got, "      line 0 0 400 20 base 14\n") != NULL &&
                   strstr(got, "block span 200 20 200 10 float-right\n") != NULL &&
                   strstr(got, "      line 0 20 200 20 base 14\n") != NULL, got);
        free(got);
    }

    // A line taller than its strut asks for its band again over its own
    // height (FLOATS.md, Decision 3). The right float stands at 20, below
    // a 20px block; the paragraph's -20px margin puts its first line back
    // at 0, ABOVE that float's top, where the space's bookmark walks back
    // to. Its strut's band, 0 to 20, is the whole 400, but a 30px picture
    // standing on the baseline makes the line 30 + 6 = 36 tall, and over 0
    // to 36 the float is beside it: the line is broken again in 300 —
    // the picture and three words, 20 + 84 + 84 + 80 = 268 — and the
    // fourth word goes to the next line, at 36, below the float.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".a{height:20px} .r{float:right;width:100px;height:10px}"
            "p.n{margin-top:-20px} .t{display:inline-block;width:20px;height:30px}</style>"
            "<div class=a></div><div class=r></div><p class=n><span class=t></span>"
            W10 " " W10 " " W10 " " W10 "</p>", 400);
        expect("floats: a tall line is broken again in the band its height reaches",
               got != NULL && strstr(got, "block div 300 20 100 10 float-right\n") != NULL &&
                   strstr(got, "      line 0 0 300 36 base 30\n") != NULL &&
                   strstr(got, "      line 0 36 400 20 base 14\n") != NULL, got);
        free(got);
    }
}

static void float_clear_cases(void)
{
    // § 9.5.2: the paragraph clears the left float. The float waits for
    // the body's margins, which resolve at 0 — the paragraph's own 20px
    // margin is not among them — and stands there; where the paragraph
    // would stand, 20, is above its bottom at 50, so its border edge goes
    // to 50, and its clearance is 30. The root holds both, 60.
    cascade_layout_case("floats: a block clears a float",
        FLOAT_STYLE ".l{float:left;width:100px;height:50px} p.c{clear:left;margin-top:20px;"
        "height:10px}</style><div class=l></div><p class=c></p>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    block div 0 0 100 50 float-left\n"
        "    block p 0 50 400 10 clearance=30\n");

    // A block that clears nothing on its side is where it would stand.
    cascade_layout_case("floats: a block that clears the other side",
        FLOAT_STYLE ".l{float:left;width:100px;height:50px} p.c{clear:right;height:10px}"
        "</style><div class=l></div><p class=c></p>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 10\n"
        "    block div 0 0 100 50 float-left\n"
        "    block p 0 0 400 10\n");

    // A FLOAT that clears goes below what it clears: three that would fit
    // side by side stack down a column.
    cascade_layout_case("floats: floats that clear stack down a column",
        FLOAT_STYLE ".s{float:left;clear:left;width:100px;height:20px}</style>"
        "<div class=s></div><div class=s></div><div class=s></div>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 100 20 float-left\n"
        "    block div 0 20 100 20 float-left\n"
        "    block div 0 40 100 20 float-left\n");

    // `<br clear=all>`: the line after it starts below the float, at 50;
    // the line before it is beside it, at 100.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px}</style>"
            "<div class=l></div><p>a<br clear=all>b</p>", 400);
        expect("floats: <br clear> starts the next line below the floats",
               got != NULL && strstr(got, "      line 100 0 300 20 base 14\n") != NULL &&
                   strstr(got, "      line 0 50 400 20 base 14\n") != NULL, got);
        free(got);
    }

    // A table clears as a block does.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px} table{clear:both}</style>"
            "<div class=l></div><table cellspacing=0 cellpadding=0><tr><td>x</table>", 400);
        expect("floats: a table clears a float",
               got != NULL && strstr(got, "    table table 0 50 ") != NULL &&
                   strstr(got, " clearance=50\n") != NULL, got);
        free(got);
    }
}

static void float_root_cases(void)
{
    // A formatting context's root grows to hold its floats (§ 10.6.7): the
    // overflow:hidden div is 50 tall, and the paragraph after it starts at
    // 50. An ordinary div does not: it holds one empty line, 0 tall, and
    // the float hangs out of it over the paragraph after it, 60 to 70. (In
    // the root the float is placed as its line is met, so that empty line
    // is broken again beside it; in the div it waits for margins no empty
    // line resolves.)
    cascade_layout_case("floats: a formatting context's root holds its floats",
        FLOAT_STYLE ".o{overflow:hidden} .l{float:left;width:100px;height:50px}"
        ".p{height:10px}</style>"
        "<div class=o><div class=l></div></div><p class=p></p>"
        "<div><div class=l></div></div><p class=p></p>", 400,
        "page 400 110\n"
        "block html 0 0 400 110\n"
        "  block body 0 0 400 70\n"
        "    block div 0 0 400 50 scroller 0 range 0 0\n"
        "      line 100 0 300 0 base 14 clip 0 0 400 50\n"
        "      block div 0 0 100 50 float-left clip 0 0 400 50\n"
        "    block p 0 50 400 10\n"
        "    block div 0 60 400 0\n"
        "      line 0 60 400 0 base 14\n"
        "      block div 0 60 100 50 float-left\n"
        "    block p 0 60 400 10\n");

    // `display: flow-root` is a block that is a formatting context's root,
    // so it holds its float too.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".r{display:flow-root} .l{float:left;width:100px;height:50px}</style>"
            "<div class=r><div class=l></div></div>", 400);
        expect("floats: a flow-root holds its floats",
               got != NULL && strstr(got, "    block div 0 0 400 50\n") != NULL, got);
        free(got);
        got = cascade_dump_of("<!doctype html><div style=display:flow-root></div>", 800, 600);
        expect("floats: flow-root is a block, and a root",
               got != NULL && strstr(got, "div block flow-root\n") != NULL, got);
        free(got);
    }
}

static void float_table_cases(void)
{
    // A floated table's margin box holds its captions, which its own box
    // leaves out: the 20px grid and the 30px bottom caption under it are
    // 50 tall, so the line at 40, beside the caption alone, is beside the
    // float too (MediaWiki's thumbnails are such tables).
    char *got = cascade_layout_of(
        FLOAT_STYLE ".t{float:right;width:100px} caption{caption-side:bottom}"
        ".c{height:30px} .g{height:20px}</style>"
        "<table class=t cellspacing=0 cellpadding=0><caption><div class=c></div></caption>"
        "<tr><td><div class=g></div></table><p>" W10 " " W10 " " W10 " " W10 " " W10 " "
        W10 " " W10 " " W10 " " W10 " " W10 "</p>", 400);
    expect("floats: a floated table's captions are part of the float",
           got != NULL && strstr(got, "      line 0 40 300 20 base 14\n") != NULL, got);
    free(got);
}

static void float_cell_cases(void)
{
    // A table cell is a formatting context's root: its float is placed in
    // its own space, its line stands beside it (from 30, 70 wide), and it
    // grows to hold it, 40 tall.
    char *got = cascade_layout_of(
        FLOAT_STYLE ".l{float:left;width:30px;height:40px}</style>"
        "<table cellspacing=0 cellpadding=0><tr><td width=100><div class=l></div>x</table>", 400);
    expect("floats: a cell holds its floats",
           got != NULL && strstr(got, "cell td 0 0 100 40\n") != NULL &&
               strstr(got, "line 30 0 70 20 base 14\n") != NULL, got);
    free(got);
}

static void float_avoid_cases(void)
{
    // A box that may not overlap a float stands beside it (FLOATS.md,
    // Decision 4). Beside a 100 x 50 left float: the overflow:hidden
    // block's auto width fills the 300 left, from 100; the flow-root set
    // 350 wide does not fit there and goes below the float, to 50; a
    // block picture 40 wide stands at 100; and a table, shrinking to its
    // content, at 100 too.
    {
        char *got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px} .o{overflow:hidden;height:20px}"
            "</style><div class=l></div><div class=o></div>", 400);
        expect("floats: a scroller stands beside a float, filling the room",
               got != NULL && strstr(got, "    block div 100 0 300 20 scroller") != NULL, got);
        free(got);
        got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px}"
            ".r{display:flow-root;width:350px;height:20px}</style>"
            "<div class=l></div><div class=r></div>", 400);
        expect("floats: a box too wide for the room goes below the float",
               got != NULL && strstr(got, "    block div 0 50 350 20\n") != NULL, got);
        free(got);
        got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px}"
            "img{display:block;width:40px;height:20px}</style>"
            "<div class=l></div><img>", 400);
        expect("floats: a replaced block stands beside a float",
               got != NULL && strstr(got, "    replaced img 100 0 40 20\n") != NULL, got);
        free(got);
        got = cascade_layout_of(
            FLOAT_STYLE ".l{float:left;width:100px;height:50px}</style>"
            "<div class=l></div><table cellspacing=0 cellpadding=0><tr><td>x</table>", 400);
        expect("floats: a table stands beside a float",
               got != NULL && strstr(got, "    table table 100 0 ") != NULL, got);
        free(got);
    }
}

static void float_intrinsic_cases(void)
{
    // FLOATS.md, Decision 5. min-content: the float on the line is a piece
    // of its own and ends the word before it, so `aaaa<float>bbbb` is as
    // narrow as its widest piece, 32 (four letters), not 64; a 1px cell
    // shrinks to that.
    char *got = cascade_layout_of(
        FLOAT_STYLE ".f{float:left;width:30px;height:10px}</style>"
        "<table cellspacing=0 cellpadding=0><tr><td width=1>aaaa<span class=f></span>bbbb</table>",
        400);
    expect("floats: a float ends the word before it in a min-content width",
           got != NULL && strstr(got, "cell td 0 0 32 ") != NULL, got);
    free(got);
    // max-content: the float goes on the line beside the words, 80 + 30 +
    // 4 + 80 = 194, the space between the words counting once.
    got = cascade_layout_of(
        FLOAT_STYLE ".f{float:left;width:30px;height:10px}</style>"
        "<table cellspacing=0 cellpadding=0><tr><td>" W10 " <span class=f></span>" W10 "</table>",
        400);
    expect("floats: a float on a line is summed into its max-content width",
           got != NULL && strstr(got, "cell td 0 0 194 ") != NULL, got);
    free(got);
}

// One page of the bound suite (FLOATS.md § Bounds), holding `n` floats:
// laid out at 800 through its sheets with a budget it cannot reach, under
// an alarm a quadratic walk could not beat; the exclusion spaces' work
// held to `per` steps a float.
static void float_bound(const char *what, const char *html, size_t n, uint64_t per)
{
    signal(SIGALRM, cost_hung);
    s_cost_case = what;
    alarm(60);
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    p.env.max_arena_bytes = (size_t)1 << 30;
    FStyles *st = f_style_build(p.doc, p.page, &p.env);
    FBoxes *bx = st != NULL ? f_boxes_build(p.doc, p.page, st, &p.env) : NULL;
    FLayout *lay = bx != NULL ? f_layout(bx, p.doc, p.page, &p.env, 800) : NULL;
    alarm(0);
    char detail[160];
    snprintf(detail, sizeof(detail), "%llu steps for %zu floats",
             lay != NULL ? (unsigned long long)lay->float_walk : 0ULL, n);
    expect(what, lay != NULL && !lay->incomplete && lay->float_walk <= per * n, detail);
    printf("libflow float bound, %s: %s\n", what, detail);
    f_layout_free(lay);
    f_boxes_free(bx);
    f_style_free(st);
    sheeted_close(&p);
}

// `head`, then each of `n` units `make` writes, then `tail`.
static char *float_page(const char *head, size_t n, size_t (*make)(char *, size_t),
                        const char *tail)
{
    size_t cap = strlen(head) + n * 128 + strlen(tail) + 1;
    char *html = malloc(cap);
    size_t at = (size_t)sprintf(html, "%s", head);
    for (size_t i = 0; i < n; i++)
        at += make(html + at, i);
    strcpy(html + at, tail);
    return html;
}

static size_t unit_beside(char *out, size_t i)
{
    (void)i;
    return (size_t)sprintf(out, "<div class=f style=width:1px;height:10px></div>");
}

static size_t unit_stair(char *out, size_t i)
{
    // Each a pixel narrower than the last, from 800 down, again and again.
    return (size_t)sprintf(out, "<div class=f style=width:%zupx;height:1px></div>", 800 - i % 800);
}

static size_t unit_cleared(char *out, size_t i)
{
    return (size_t)sprintf(out, "<div class=%s></div>", i % 2 ? "c r" : "c");
}

static size_t unit_pixel(char *out, size_t i)
{
    (void)i;
    return (size_t)sprintf(out, "<div class=f style=width:1px;height:1px></div>");
}

static size_t unit_line(char *out, size_t i)
{
    (void)i;
    return (size_t)sprintf(out, "x<br>");
}

static size_t unit_negative(char *out, size_t i)
{
    (void)i;
    return (size_t)sprintf(out, "<div class=f style=width:10px;height:10px></div><p class=n>x</p>");
}

static void float_bound_cases(void)
{
    const size_t n = 10000;
    static const char head[] = FLOAT_STYLE ".f{float:left} .c{float:left;clear:both;width:10px;"
                               "height:1px} .r{float:right} .n{margin-top:-15px;height:10px}</style>";
    char *html = float_page(head, n, unit_beside, "");
    float_bound("bounds: 10000 floats side by side", html, n, 16);
    free(html);
    html = float_page(head, n, unit_stair, "");
    float_bound("bounds: a staircase of 10000 left floats", html, n, 16);
    free(html);
    html = float_page(head, n, unit_cleared, "");
    float_bound("bounds: 10000 alternating floats, each cleared", html, n, 16);
    free(html);
    char *lines = float_page("<p>", n, unit_line, "</p>");
    html = float_page(head, n, unit_pixel, lines);
    float_bound("bounds: 10000 one-pixel floats beside a 10000-line paragraph", html, n, 16);
    free(html);
    free(lines);
    html = float_page(head, n, unit_negative, "");
    float_bound("bounds: 10000 floats in blocks with negative top margins", html, n, 16);
    free(html);
}

// What Chrome 155 answered on tools/float_probe/float-probe.html (the
// cases the spec leaves to the reader; FLOATS.md § Decisions).
static bool has(const char *got, const char *want)
{
    return got != NULL && strstr(got, want) != NULL;
}

static void float_probe_cases(void)
{
    // Case 1c: a float alone in an empty div with a 10px top margin stands
    // at 10, where the div does, not at 25, where the next block's margin
    // takes the margins that collapse through the div.
    char *got = cascade_layout_of(
        FLOAT_STYLE ".d{margin-top:10px} .l{float:left;width:60px;height:20px}"
        ".m{margin-top:25px;height:10px}</style>"
        "<div class=d><div class=l></div></div><div class=m></div>", 400);
    expect("floats: probe 1c, a float in an empty box stands where the box does",
           has(got, "block div 0 10 60 20 float-left\n") && has(got, "    block div 0 25 400 10\n"),
           got);
    free(got);

    // Case 4a: a width=100% table beside a float is 100% of its containing
    // block, which does not fit beside the float: it goes below, 400 wide.
    got = cascade_layout_of(
        FLOAT_STYLE ".l{float:left;width:100px;height:40px} .h{height:10px}</style>"
        "<div class=l></div><table width=100% cellspacing=0 cellpadding=0><tr><td>"
        "<div class=h></div></table>", 400);
    expect("floats: probe 4a, a full-width table goes below the float",
           has(got, "    table table 0 40 400 10\n"), got);
    free(got);

    // Case 5: max-content among blocks. A float (100) and a paragraph
    // (100) are 100, not 200; a float and a table (100) are 200; float,
    // paragraph, float are 100; float, clearing paragraph, 100; and floats
    // of 100 and 40, a clearing block and a float of 30 are 140.
    static const struct {
        const char *what, *cell, *want;
    } kRuns[] = {
        {"a paragraph", "<div class=f></div><p><span class=w></span><span class=w></span></p>",
         "cell td 0 0 100 "},
        {"a table", "<div class=f></div><table cellspacing=0 cellpadding=0><tr><td>"
                    "<span class=w></span><span class=w></span></table>", "cell td 0 0 200 "},
        {"a paragraph and a float", "<div class=f></div><p><span class=w></span>"
                                    "<span class=w></span></p><div class=f></div>",
         "cell td 0 0 100 "},
        {"a clearing paragraph", "<div class=f></div><p style=clear:left><span class=w></span>"
                                 "<span class=w></span></p>", "cell td 0 0 100 "},
        {"a run, a clear and a float", "<div class=f></div><div class=f style=width:40px></div>"
                                       "<div style=clear:left></div>"
                                       "<div class=f style=width:30px></div>",
         "cell td 0 0 140 "},
        // A float that clears starts a line of its own on its side (§
        // 9.5.2), as the clearing block does: 140, not 100 + 40 + 30.
        {"a run and a float that clears", "<div class=f></div><div class=f style=width:40px></div>"
                                          "<div class=f style=width:30px;clear:left></div>",
         "cell td 0 0 140 "},
        // The same among blocks (the empty div makes the cell hold one).
        {"a run among blocks and a float that clears",
         "<div class=f></div><div class=f style=width:40px></div>"
         "<div class=f style=width:30px;clear:left></div><div></div>", "cell td 0 0 140 "},
    };
    for (size_t k = 0; k < sizeof(kRuns) / sizeof(kRuns[0]); k++) {
        char html[1024], name[128];
        snprintf(html, sizeof(html),
                 FLOAT_STYLE ".f{float:left;width:100px;height:10px}"
                 ".w{display:inline-block;width:50px;height:10px}</style>"
                 "<table cellspacing=0 cellpadding=0><tr><td>%s</table>", kRuns[k].cell);
        snprintf(name, sizeof(name), "floats: probe 5, the max-content of a float and %s",
                 kRuns[k].what);
        got = cascade_layout_of(html, 800);
        expect(name, has(got, kRuns[k].want), got);
        free(got);
    }

    // Case 6: a floated root shrinks to fit its content, 5 + 50 + 5.
    got = cascade_layout_of("<!doctype html><html style=float:left><body style=margin:5px>"
                            "<div style=width:50px;height:10px></div>", 400);
    expect("floats: probe 6, a floated root shrinks to fit",
           has(got, "block html 0 0 60 20\n") && has(got, "  block body 5 5 50 10\n"), got);
    free(got);

    // Case 7: a right float wider than its container keeps its right edge
    // at the container's and overflows to the left: 150 + 200 - 300 = 50.
    got = cascade_layout_of(
        FLOAT_STYLE ".c{margin-left:150px;width:200px} .r{float:right;width:300px;height:10px}"
        "</style><div class=c><div class=r></div></div>", 400);
    expect("floats: probe 7, a wide right float overflows to the left",
           has(got, "block div 50 0 300 10 float-right\n"), got);
    free(got);
}

static int seen_at(const Seen *s, const flow_box_t *b)
{
    for (int i = 0; i < s->n; i++)
        if (s->v[i] == b)
            return i;
    return -1;
}

static void float_paint_cases(void)
{
    // Appendix E: a float is painted after the block backgrounds (step 4)
    // and before the inline content (step 7), whole: the paragraph written
    // after it paints its background first, then the float and the float's
    // own text, then its text. The float inside the second paragraph's line
    // is painted once, in the same step.
    Sheeted p;
    sheeted_open(&p, FLOAT_STYLE ".l{float:left;width:100px;height:50px;background:#00f}"
                     " p{background:#f00}</style><div class=l>in</div><p>" W10 "</p>"
                     "<p>x<span class=l>f</span></p>", 400, 600);
    flow_tree_t *t = flow_layout(p.doc, p.page, 400, &p.env);
    const os64_html_node_t *div = p.doc->body->first_child, *para = div->next;
    const os64_html_node_t *span = para->next->first_child->next;
    const flow_box_t *fb = t != NULL ? flow_box_for(t, div) : NULL;
    const flow_box_t *pb = t != NULL ? flow_box_for(t, para) : NULL;
    const flow_box_t *sb = t != NULL ? flow_box_for(t, span) : NULL;
    Seen out = {0};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 600}, kNoScroll, seen_add, &out);
    const flow_box_t *in_text = fb != NULL && fb->first != NULL ? fb->first->first : NULL;
    const flow_box_t *p_text = pb != NULL && pb->first != NULL ? pb->first->first : NULL;
    int at_p = seen_at(&out, pb), at_f = seen_at(&out, fb), at_in = seen_at(&out, in_text);
    int at_text = seen_at(&out, p_text), at_s = seen_at(&out, sb), twice = 0;
    const flow_box_t *s_text = sb != NULL && sb->first != NULL ? sb->first->first : NULL;
    for (int i = 0; i < out.n; i++)
        twice += out.v[i] == s_text;
    expect("floats: a float paints after the blocks and before the inline content",
           fb != NULL && fb->floated == FLOW_FLOAT_LEFT && at_p >= 0 && at_p < at_f &&
               at_f < at_in && at_in < at_text, NULL);
    expect("floats: a float on a line is painted once, with the floats",
           sb != NULL && sb->floated == FLOW_FLOAT_LEFT && at_s > at_p && s_text != NULL &&
               twice == 1, NULL);

    // A click on the float where the paragraph's border box runs under it
    // finds the float — its line there — though the paragraph comes later
    // in the tree.
    const flow_box_t *h = flow_hit(t, 50, 10, kNoScroll);
    while (h != NULL && h != fb)
        h = h->parent;
    expect("floats: the float, not the block under it, takes the click", h == fb && fb != NULL,
           NULL);
    flow_free(t);
    sheeted_close(&p);
}

static void float_cases(void)
{
    float_style_cases();
    float_place_cases();
    float_wait_cases();
    float_wait_end_cases();
    float_relative_cases();
    float_inline_relative_cases();
    float_line_cases();
    float_clear_cases();
    float_root_cases();
    float_avoid_cases();
    float_cell_cases();
    float_table_cases();
    float_paint_cases();
    float_intrinsic_cases();
    float_bound_cases();
    float_probe_cases();
    float_sweep_cases();
}
