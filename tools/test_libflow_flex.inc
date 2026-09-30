// Flexible boxes (FLEX.md, F1): every expectation worked by hand from
// Flexbox 1 § 9 with the test font's metrics (test_libflow_fonts.c: at
// 16px a line is 20 high, its baseline 14 down, `x` 8 wide, `w` 12, `l` 4).
// A viewport 400 wide and 600 high, the body's margin 0.

static const flow_box_t *flex_box(const Sheeted *q, const flow_tree_t *t, const char *id)
{
    return t != NULL ? box_with_id(t, q->doc->document, id) : NULL;
}

// One page laid out, and the rects of up to four ids checked against
// `want` (x, y, w, h each; -1 for "not checked").
static void flex_rects(const char *name, const char *page, const char *const ids[4],
                       const int32_t want[4][4])
{
    Sheeted q;
    char html[4096];
    snprintf(html, sizeof(html), "<!doctype html><style>body{margin:0} p{margin:0}%s", page);
    sheeted_open(&q, html, 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    bool ok = t != NULL;
    char got[256] = "";
    size_t at = 0;
    for (int i = 0; i < 4 && ids[i] != NULL && ok; i++) {
        const flow_box_t *b = flex_box(&q, t, ids[i]);
        if (b == NULL) {
            ok = false;
            break;
        }
        const int32_t r[4] = {b->rect.x, b->rect.y, b->rect.w, b->rect.h};
        at += (size_t)snprintf(got + at, sizeof(got) - at, "%s %d,%d %dx%d; ", ids[i], r[0], r[1],
                               r[2], r[3]);
        for (int k = 0; k < 4; k++)
            ok &= want[i][k] < 0 || want[i][k] == r[k];
    }
    expect(name, ok, got);
    flow_free(t);
    sheeted_close(&q);
}

#define FLEX_IDS(...) (const char *const[4]){__VA_ARGS__}
#define FLEX_WANT(...) (const int32_t[4][4]){__VA_ARGS__}

static void flex_row_cases(void)
{
    // Three `flex: 1` items share 400 from a basis of 0: 133.33 each, the
    // 26.6 remainder carried so the three meet the end — 0..133, 133..267,
    // 267..400 once rounded — and each as tall as its 20px.
    flex_rects("flex: a row of flex: 1 shares the line exactly",
               ".c{display:flex} .c div{flex:1;height:20px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 133, 20}, {133, 0, 134, 20}, {267, 0, 133, 20}));

    // Three 50px items that do not flex leave 250 free, and
    // justify-content places it: § 8.2.
    static const struct { const char *how; int32_t x[3]; } kJustify[] = {
        {"flex-start", {0, 50, 100}},
        {"flex-end", {250, 300, 350}},
        {"center", {125, 175, 225}},
        {"space-between", {0, 175, 350}},
        // 250 / 6 = 41.67 either side of each: 41.67, 175, 308.33.
        {"space-around", {42, 175, 308}},
        // 250 / 4 = 62.5 between and at the ends: 62.5, 175, 287.5, ties up.
        {"space-evenly", {63, 175, 288}},
    };
    for (size_t i = 0; i < sizeof(kJustify) / sizeof(kJustify[0]); i++) {
        char page[512], name[96];
        snprintf(page, sizeof(page),
                 ".c{display:flex;justify-content:%s} .c div{flex:none;width:50px;height:10px}"
                 "</style><div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
                 kJustify[i].how);
        snprintf(name, sizeof(name), "flex: justify-content %s", kJustify[i].how);
        flex_rects(name, page, FLEX_IDS("a", "b", "c"),
                   FLEX_WANT({kJustify[i].x[0], 0, 50, 10}, {kJustify[i].x[1], 0, 50, 10},
                             {kJustify[i].x[2], 0, 50, 10}));
    }
    // row-reverse starts at the right: the first item at 350.
    flex_rects("flex: row-reverse places the first item at the right",
               ".c{display:flex;flex-direction:row-reverse} .c div{width:50px;height:10px}"
               "</style><div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({350, 0, 50, 10}, {300, 0, 50, 10}));
    // Items that overflow: space-between starts them at the start edge,
    // center overflows both ways (FLEX.md, decision 4; the probe's case 1).
    flex_rects("flex: overflowing items start at the start, or centre both ways",
               ".c{display:flex;width:300px} .c div{flex:none;width:200px;height:10px}"
               ".b{justify-content:space-between} .m{justify-content:center}</style>"
               "<div class='c b'><div id=a></div><div></div></div>"
               "<div class='c m'><div id=b></div><div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 200, 10}, {-50, 10, 200, 10}));

    // Growing, frozen at a limit: two flex: 1 from 0 take 200 each; A's
    // max-width holds it at 100, a negative violation, so A is frozen and
    // B shares the rest: 300 (§ 9.7).
    flex_rects("flex: an item frozen at its max-width, the other takes the rest",
               ".c{display:flex} .c div{flex:1;height:10px} #a{max-width:100px}</style>"
               "<div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 100, 10}, {100, 0, 300, 10}));
    // Shrinking, floored at the content: two items 150 wide in 200 each
    // hold ten `w`s (120). Shrinking by their bases gives each 100, below
    // the automatic minimum — the smaller of the content's 120 and the
    // width's 150 — so both freeze at 120 and overflow (§ 4.5).
    flex_rects("flex: shrinking stops at the automatic minimum",
               ".c{display:flex;width:200px} .c div{width:150px}</style>"
               "<div class=c><div id=a>wwwwwwwwww</div><div id=b>wwwwwwwwww</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 120, 20}, {120, 0, 120, 20}));
    // A scroll container has no automatic minimum: it shrinks to 100.
    flex_rects("flex: a scroll container shrinks below its content",
               ".c{display:flex;width:200px} .c div{width:150px;overflow:hidden}</style>"
               "<div class=c><div id=a>wwwwwwwwww</div><div id=b>wwwwwwwwww</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 100, 20}, {100, 0, 100, 20}));
    // Grow weighted: 1 and 3 share 400 from bases of 0: 100 and 300.
    flex_rects("flex: grow factors weigh the shares",
               ".c{display:flex} .c div{height:10px} #a{flex:1} #b{flex:3}</style>"
               "<div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 100, 10}, {100, 0, 300, 10}));
    // Factors that sum below one share only that fraction: 0.25 of 400.
    flex_rects("flex: grow factors below one share only their fraction",
               ".c{display:flex} #a{flex:0.25;height:10px}</style>"
               "<div class=c><div id=a></div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 100, 10}));
    // A gap between items: 50, gap 10, 50, gap 10, 50.
    flex_rects("flex: gap between the items",
               ".c{display:flex;gap:10px} .c div{width:50px;height:10px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 50, 10}, {60, 0, 50, 10}, {120, 0, 50, 10}));
    // margin-left: auto takes the free space: the last item at the end.
    flex_rects("flex: margin-left auto pushes an item to the end",
               ".c{display:flex} .c div{width:50px;height:10px} #b{margin-left:auto}</style>"
               "<div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 50, 10}, {350, 0, 50, 10}));
    // order: B (order 1) before A (order 2), whatever the markup says.
    flex_rects("flex: order places the items",
               ".c{display:flex} .c div{width:50px;height:10px} #a{order:2} #b{order:1}</style>"
               "<div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({50, 0, 50, 10}, {0, 0, 50, 10}));
    // An auto basis with no width is the content's max-content: `xx` is 16.
    flex_rects("flex: an auto basis is the content's width",
               ".c{display:flex}</style><div class=c><div id=a>xx</div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 16, 20}));
}

static void flex_cross_cases(void)
{
    // The line is its tallest item, 50. A stretches to it; B is 50.
    flex_rects("flex: stretch makes an item as tall as the line",
               ".c{display:flex} #b{height:50px;width:10px}</style>"
               "<div class=c><div id=a>x</div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 8, 50}, {8, 0, 10, 50}));
    // A item with a height of its own does not stretch; center and
    // flex-end move a 20-high item in a line of 50: 15 and 30.
    flex_rects("flex: align-items center",
               ".c{display:flex;align-items:center} #b{height:50px;width:10px}</style>"
               "<div class=c><div id=a>x</div><div id=b></div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 15, 8, 20}));
    flex_rects("flex: align-self flex-end",
               ".c{display:flex} #a{align-self:flex-end} #b{height:50px;width:10px}</style>"
               "<div class=c><div id=a>x</div><div id=b></div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 30, 8, 20}));
    // Baselines: A's is 14 down, B's 20 + 14 = 34 (padding-top 20). A moves
    // down 20; the line is 34 above and 6 below: 40.
    flex_rects("flex: align-items baseline lines up the first lines",
               ".c{display:flex;align-items:baseline} #b{padding-top:20px}</style>"
               "<div class=c id=c><div id=a>x</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 20, 8, 20}, {8, 0, 8, 40}, {0, 0, 400, 40}));
    // Auto margins on the cross axis centre: (50 - 20) / 2 = 15.
    flex_rects("flex: margin auto centres on the cross axis",
               ".c{display:flex;height:50px} #a{margin:auto 0}</style>"
               "<div class=c><div id=a>x</div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 15, 8, 20}));
    // A container given a height is the line's: stretch to 80.
    flex_rects("flex: a container's height is its line's",
               ".c{display:flex;height:80px}</style><div class=c><div id=a>x</div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 8, 80}));
}

static void flex_column_cases(void)
{
    // A column 300 high: B is 50, A grows from 0 into the other 250.
    flex_rects("flex: a column grows an item into its height",
               ".c{display:flex;flex-direction:column;height:300px} #a{flex:1}"
               " #b{height:50px}</style><div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 250}, {0, 250, 400, 50}));
    // Without a height it is as tall as its items: 20 + 20.
    flex_rects("flex: a column with no height is its items' height",
               ".c{display:flex;flex-direction:column}</style>"
               "<div class=c id=c><div id=a>x</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 400, 20}, {0, 20, 400, 20}, {0, 0, 400, 40}));
    // justify-content center in 300: two 20s leave 260, 130 above.
    flex_rects("flex: a column centres its items",
               ".c{display:flex;flex-direction:column;justify-content:center;height:300px}"
               "</style><div class=c><div id=a>x</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 130, 400, 20}, {0, 150, 400, 20}));
    // align-items center sizes an item to its content (`xx`, 16) and
    // centres it across: (400 - 16) / 2 = 192.
    flex_rects("flex: a column's item centred across is as wide as its content",
               ".c{display:flex;flex-direction:column;align-items:center}</style>"
               "<div class=c><div id=a>xx</div></div>",
               FLEX_IDS("a"), FLEX_WANT({192, 0, 16, 20}));
    // column-reverse in 100: the first item at the bottom.
    flex_rects("flex: column-reverse places the first item at the bottom",
               ".c{display:flex;flex-direction:column-reverse;height:100px}</style>"
               "<div class=c><div id=a>x</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 80, 400, 20}, {0, 60, 400, 20}));
    // min-height on the column: its items grow into it, the sticky-footer
    // idiom: A (flex: 1) takes what B's 20 leaves of 200.
    flex_rects("flex: a column's min-height is room its items grow into",
               ".c{display:flex;flex-direction:column;min-height:200px} #a{flex:1}</style>"
               "<div class=c><div id=a>x</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 180}, {0, 180, 400, 20}));
}

static void flex_box_cases(void)
{
    // Items are blockified: a span becomes a block that remembers it was
    // inline; a run of text is an anonymous item; nothing is a line of
    // the container's own.
    {
        char *got = cascade_dump_of("<!doctype html><style>.c{display:flex}</style>"
                                    "<div class=c><span>a</span>text</div>", 400, 600);
        expect("flex: an item span is blockified",
               got != NULL && strstr(got, "span block specified-inline") != NULL, got);
        free(got);
    }
    // An inline flex container is an atom as wide as its items: `x` and
    // `yy`, 8 + 16 (FLEX.md, decision 3).
    flex_rects("flex: an inline-flex is as wide as its items side by side",
               ".f{display:inline-flex}</style><p>a<span class=f id=f><b>x</b><b>yy</b></span>b",
               FLEX_IDS("f"), FLEX_WANT({8, 0, 24, 20}));
    // A container of nothing but text: one anonymous item, as wide as the
    // text (`hello`: 8 + 8 + 4 + 4 + 8), at the start.
    flex_rects("flex: a container of text is one item as wide as the text",
               ".c{display:flex;justify-content:flex-end}</style>"
               "<div class=c id=c>hello</div>",
               FLEX_IDS("c"), FLEX_WANT({0, 0, 400, 20}));
    {
        Sheeted q;
        sheeted_open(&q, "<!doctype html><style>body{margin:0} .c{display:flex;"
                         "justify-content:flex-end}</style><div class=c id=c>hello</div>",
                     400, 600);
        flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *c = flex_box(&q, t, "c");
        const flow_box_t *item = c != NULL ? c->first : NULL;
        expect("flex: its text is an anonymous item at the end",
               item != NULL && item->node == NULL && item->kind == FLOW_BOX_BLOCK &&
                   item->rect.x == 368 && item->rect.w == 32, NULL);
        flow_free(t);
        sheeted_close(&q);
    }
    // An absolute child is no item: it sits at the content box's corner.
    flex_rects("flex: an absolute child is not an item",
               ".c{display:flex;position:relative;padding:10px} #a{position:absolute}"
               " .c div{width:50px;height:10px}</style>"
               "<div class=c><div id=b></div><div id=a></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({10, 10, 50, 10}, {10, 10, 50, 10}));
    // The nav of danlegt.com: six links, flex: 1, space-around (which a
    // line with no free space leaves as it is): 400 / 6 each.
    flex_rects("flex: a nav bar of six equal links",
               "nav{display:flex;justify-content:space-around} nav a{flex:1;display:inline-block}"
               "</style><nav><a id=a href=x>home</a><a href=x>b</a><a href=x>c</a>"
               "<a href=x>d</a><a href=x>e</a><a id=f href=x>f</a></nav>",
               FLEX_IDS("a", "f"), FLEX_WANT({0, 0, 67, 20}, {333, 0, 67, 20}));
}

static void flex_wrap_cases(void)
{
    // Cards 150 wide in 400: two fit on a line (300; a third would make
    // 450), the third on the next, 20 down.
    flex_rects("flex: wrap puts what does not fit on the next line",
               ".c{display:flex;flex-wrap:wrap} .c div{width:150px;height:20px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 150, 20}, {150, 0, 150, 20}, {0, 20, 150, 20}));
    // With gap 10: 150 + 10 + 150 = 310 fits, 470 does not; the next line
    // is 10 below the first.
    flex_rects("flex: gap between items and between lines",
               ".c{display:flex;flex-wrap:wrap;gap:10px} .c div{width:150px;height:20px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 150, 20}, {160, 0, 150, 20}, {0, 30, 150, 20}));
    // Each line flexes on its own: two from 150 grow to 200 each, the
    // third alone to 400.
    flex_rects("flex: each wrapped line grows on its own",
               ".c{display:flex;flex-wrap:wrap} .c div{flex:1 1 150px;height:20px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 200, 20}, {200, 0, 200, 20}, {0, 20, 400, 20}));
    // align-content in a container 100 high: two lines of 20 leave 60.
    static const struct { const char *how; int32_t ay, ah, cy, ch; } kContent[] = {
        {"normal", 0, 50, 50, 50},          // stretch: each line 50, the items with them
        {"flex-start", 0, 20, 20, 20},
        {"flex-end", 60, 20, 80, 20},
        {"center", 30, 20, 50, 20},
        {"space-between", 0, 20, 80, 20},
        {"space-around", 15, 20, 65, 20},   // 60 / 4 about each line
        {"space-evenly", 20, 20, 60, 20},   // 60 / 3 between and at the ends
    };
    for (size_t i = 0; i < sizeof(kContent) / sizeof(kContent[0]); i++) {
        char page[512], name[96];
        snprintf(page, sizeof(page),
                 ".c{display:flex;flex-wrap:wrap;height:100px;align-content:%s}"
                 " .c div{width:300px}</style><div class=c><div id=a>x</div><div id=c>x</div></div>",
                 kContent[i].how);
        snprintf(name, sizeof(name), "flex: align-content %s", kContent[i].how);
        flex_rects(name, page, FLEX_IDS("a", "c"),
                   FLEX_WANT({0, kContent[i].ay, 300, kContent[i].ah},
                             {0, kContent[i].cy, 300, kContent[i].ch}));
    }
    // wrap-reverse stacks the lines from the bottom: the first line at 20
    // in a container 40 high, the second at 0.
    flex_rects("flex: wrap-reverse puts the first line at the far side",
               ".c{display:flex;flex-wrap:wrap-reverse} .c div{width:150px;height:20px}</style>"
               "<div class=c><div id=a></div><div id=b></div><div id=c></div></div>",
               FLEX_IDS("a", "c"), FLEX_WANT({0, 20, 150, 20}, {0, 0, 150, 20}));
    // A wrapping column 100 high: two items of 40 per line (a third would
    // make 120). Each line is as wide as its item, 8 (`x`), and the 384
    // left across is shared, 192 each, so each line and each stretched
    // item is 200 wide.
    flex_rects("flex: a wrapping column makes lines side by side",
               ".c{display:flex;flex-direction:column;flex-wrap:wrap;height:100px}"
               " .c div{height:40px}</style>"
               "<div class=c><div id=a>x</div><div id=b>x</div><div id=c>x</div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 200, 40}, {0, 40, 200, 40}, {200, 0, 200, 40}));
}

// Codex's first round on F1 (PR #181): each case the one its finding
// describes, worked by hand.
static void flex_review_cases(void)
{
    // A column item's automatic minimum is its CONTENT's height, not a
    // height the page gave it: two empty 300px items shrink into 400, 200
    // each (§ 4.5).
    flex_rects("flex: a column's automatic minimum is the content, not the height",
               ".c{display:flex;flex-direction:column;height:400px} .c div{height:300px}"
               "</style><div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 200}, {0, 200, 400, 200}));
    // `flex-basis: content` in a column is the content's height (one line,
    // 20), whatever height the page gave; not flexing, it stays 20.
    flex_rects("flex: a column's content basis ignores the height",
               ".c{display:flex;flex-direction:column;height:400px}"
               " .c div{height:300px;flex:0 0 content}</style>"
               "<div class=c><div id=a>x</div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 400, 20}));
    // An absolute box in an item is placed against the item's FINAL size:
    // stretched to the row's 100, and grown down a 100px column.
    flex_rects("flex: an absolute box in a stretched item sits at its bottom",
               ".c{display:flex;height:100px} .c div{position:relative;width:50px}"
               " .c span{position:absolute;bottom:0;height:10px;width:10px}</style>"
               "<div class=c><div id=i><span id=a></span></div></div>",
               FLEX_IDS("i", "a"), FLEX_WANT({0, 0, 50, 100}, {0, 90, 10, 10}));
    flex_rects("flex: an absolute box in a grown column item sits at its bottom",
               ".c{display:flex;flex-direction:column;height:100px} .c div{position:relative;flex:1}"
               " .c span{position:absolute;bottom:0;height:10px;width:10px}</style>"
               "<div class=c><div id=i><span id=a></span></div></div>",
               FLEX_IDS("i", "a"), FLEX_WANT({0, 0, 400, 100}, {0, 90, 10, 10}));
    // A relative item in a column keeps its offset when the column moves it
    // to its place: 10 down from 0 and from 20.
    flex_rects("flex: a column item keeps its relative offset",
               ".c{display:flex;flex-direction:column} .c div{height:20px;position:relative;top:10px}"
               "</style><div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 10, 400, 20}, {0, 30, 400, 20}));
    // `left` on a column is `start`: column-reverse puts it at the top, the
    // first item lowest of the two (a 20 down, b at 0).
    flex_rects("flex: justify-content left on a column-reverse is its start",
               ".c{display:flex;flex-direction:column-reverse;height:100px;justify-content:left}"
               " .c div{height:20px}</style><div class=c><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 20, 400, 20}, {0, 0, 400, 20}));
    // An inline flex container is as wide as items that cannot shrink ask
    // by their basis (Chrome's measure): two empty `flex: 0 0 100px`, 200;
    // and an item that can shrink asks its content, 200 past a 100px basis.
    flex_rects("flex: an inline flex container holds its unshrinking bases",
               ".x{display:inline-flex}</style><div><span class=x id=g>"
               "<div style='flex:0 0 100px'></div><div style='flex:0 0 100px'></div></span></div>",
               FLEX_IDS("g"), FLEX_WANT({0, -1, 200, -1}));
    flex_rects("flex: a shrinkable basis asks its content",
               ".x{display:inline-flex}</style><div><span class=x id=g>"
               "<div style='flex:0 1 100px'><div style='width:200px;height:10px'></div></div>"
               "</span></div>",
               FLEX_IDS("g"), FLEX_WANT({0, -1, 200, -1}));

    // A thousand `flex: 0 .001 .75px` items (750px) shrink into a 10px
    // row. Their factors sum to one, so all of the overflow is shared
    // (§ 9.7), by weights of a factor times a base that are both tiny: in
    // whole numbers of 26.6 each product rounds to nothing, and the row
    // kept its 750.
    {
        static char page[16384];
        strcpy(page, ".c{display:flex;width:10px} .c div{flex:0 .001 .75px;min-width:0;height:5px}"
                     "</style><div class=c>");
        for (int i = 0; i < 999; i++)
            strcat(page, "<div></div>");
        strcat(page, "<div id=z></div></div>");
        Sheeted q;
        static char html[20000];
        snprintf(html, sizeof(html), "<!doctype html><style>body{margin:0}%s", page);
        sheeted_open(&q, html, 400, 600);
        flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *z = flex_box(&q, t, "z");
        char got[64];
        snprintf(got, sizeof(got), "last item %d..%d", z ? z->rect.x : -1,
                 z ? z->rect.x + z->rect.w : -1);
        expect("flex: tiny factors on bases under a pixel still shrink", z != NULL &&
               z->rect.x + z->rect.w <= 11, got);
        flow_free(t);
        sheeted_close(&q);
    }

    // A layout that stops inside a later item withdraws the earlier ones:
    // they have only their provisional places. The third item's text needs
    // far more line memory than the three boxes need box memory, so a
    // budget between the two stops in layout with the box tree whole;
    // every budget up to the whole is tried, and a placed item is always
    // where the whole layout puts it.
    {
        static char page[40000];
        strcpy(page, "<!doctype html><style>body{margin:0}.c{display:flex;flex-direction:column;"
                     "height:300px} .c p{margin:0;flex:1}</style><div class=c>"
                     "<p id=a>one<p id=b>two<p id=c>");
        for (int i = 0; i < 3000; i++)
            strcat(page, "w ");
        strcat(page, "</div>");
        Sheeted q;
        sheeted_open(&q, page, 400, 600);
        flow_tree_t *whole = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *wa = flex_box(&q, whole, "a"), *wb = flex_box(&q, whole, "b");
        bool ok = wa != NULL && wb != NULL, stopped = false;
        for (size_t budget = 8 * 1024; ok && budget < 4u * 1024 * 1024; budget += 8 * 1024) {
            q.env.max_arena_bytes = budget;
            flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
            if (t == NULL)
                continue;
            bool incomplete = flow_incomplete(t);
            stopped |= incomplete && flex_box(&q, t, "c") == NULL;
            const flow_box_t *a = flex_box(&q, t, "a"), *b = flex_box(&q, t, "b");
            ok = (a == NULL || memcmp(&a->rect, &wa->rect, sizeof(a->rect)) == 0) &&
                 (b == NULL || memcmp(&b->rect, &wb->rect, sizeof(b->rect)) == 0);
            flow_free(t);
            if (!incomplete)
                break;
        }
        expect("flex: a layout stopped in a later item withdraws the earlier ones", ok && stopped,
               stopped ? NULL : "no budget stopped inside the third item");
        flow_free(whole);
        sheeted_close(&q);
    }
}

// The same findings at the sites wrapping added (PR #182): a wrapping
// column moves its items ACROSS to their lines, and widens a stretched
// one to its line.
static void flex_wrap_review_cases(void)
{
    // Two lines of one 40px item each in a 50px-high column: the second
    // line starts at 30 (the first line is its widest item, 30), and the
    // relative item keeps its 5px on top of that.
    flex_rects("flex: a wrapping column item keeps its relative offset across",
               ".c{display:flex;flex-direction:column;flex-wrap:wrap;height:50px;align-content:start}"
               " .c div{height:40px;width:30px}</style><div class=c><div id=a></div>"
               "<div id=b style=position:relative;left:5px></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 30, 40}, {35, 0, 30, 40}));
    // A stretched item in a wrapping column is widened to its line (the
    // 80px of its sibling), and its absolute box, right:0, sits at the
    // widened edge: 80 - 10.
    flex_rects("flex: an absolute box in a widened wrapping-column item sits at its edge",
               ".c{display:flex;flex-direction:column;flex-wrap:wrap;height:100px;align-content:start}"
               " .c span{position:absolute;right:0;top:0;width:10px;height:10px}</style>"
               "<div class=c><div id=i style=position:relative;height:20px><span id=a></span></div>"
               "<div style=width:80px;height:20px></div></div>",
               FLEX_IDS("i", "a"), FLEX_WANT({0, 0, 80, 20}, {70, 0, 10, 10}));
}

// Nested containers, bases and limits (Quinn's round on #181; Chrome's
// answers).
static void flex_nested_cases(void)
{
    // An item that is itself a flex container places its items against
    // the height its container gave it: stretched to 100 in a row, grown
    // to 100 down a column, its 20px child centred at 40.
    flex_rects("flex: a stretched flex item centres against its stretched height",
               "#o{display:flex;width:200px;height:100px} #i{display:flex;width:100px;align-items:center}"
               " #k{width:20px;height:20px}</style><div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("i", "k"), FLEX_WANT({0, 0, 100, 100}, {0, 40, 20, 20}));
    flex_rects("flex: a grown column item centres against its grown height",
               "#o{display:flex;flex-direction:column;width:200px;height:100px}"
               " #i{flex:1;display:flex;flex-direction:column;justify-content:center}"
               " #k{width:20px;height:20px}</style><div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("i", "k"), FLEX_WANT({0, 0, 200, 100}, {0, 40, 20, 20}));
    // A flex container's content height is what its items ask, not a
    // height the page gave it: its content basis is its child's 20, and
    // two 300px ones shrink into 400, 200 each, down to that content.
    flex_rects("flex: a nested flex container's content basis is its items'",
               "#o{display:flex;flex-direction:column;width:200px}"
               " #i{display:flex;flex-direction:column;height:300px;flex:0 0 content}"
               " #k{width:20px;height:20px}</style><div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("o", "i"), FLEX_WANT({0, 0, 200, 20}, {0, 0, 200, 20}));
    flex_rects("flex: nested flex containers shrink down to their items",
               "#o{display:flex;flex-direction:column;height:400px} #o>div{display:flex;height:300px}"
               " .k{width:20px;height:20px}</style><div id=o><div id=a><div class=k></div></div>"
               "<div id=b><div class=k></div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 200}, {0, 200, 400, 200}));
    // A table's content height is its rows': as a column item it keeps
    // its 60, and the item after it starts there.
    flex_rects("flex: a table in a column keeps its rows' height",
               "#o{display:flex;flex-direction:column} table{border-spacing:0} td{padding:0}"
               "</style><div id=o><table id=t><tr><td><div style=height:60px></div></table>"
               "<div id=n style=height:10px></div></div>",
               FLEX_IDS("t", "n"), FLEX_WANT({0, 0, -1, 60}, {0, 60, 400, 10}));
    // An auto basis is the content's size before the item's limits, which
    // hold only its hypothetical size: two 200px bases shrink into 150 by
    // equal weights, 75 each, the first's 100px limit never reached.
    flex_rects("flex: an auto basis in a row is unclamped",
               "#o{display:flex;width:150px} #o>div{min-width:0} #a{max-width:100px}"
               " .k{width:200px;height:20px}</style><div id=o><div id=a><div class=k></div></div>"
               "<div id=b><div class=k></div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 75, -1}, {75, 0, 75, -1}));
    flex_rects("flex: an auto basis in a column is unclamped",
               "#o{display:flex;flex-direction:column;height:150px} #o>div{min-height:0}"
               " #a{max-height:100px} .k{height:200px}</style><div id=o><div id=a><div class=k></div>"
               "</div><div id=b><div class=k></div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 75}, {0, 75, 400, 75}));
    // A picture flexed narrower keeps its ratio: 40x30 into 20 is 20x15.
    flex_rects("flex: a flexed picture keeps its ratio",
               "#o{display:flex;width:20px;align-items:flex-start} img{min-width:0}</style>"
               "<div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i", "o"), FLEX_WANT({0, 0, 20, 15}, {0, 0, 20, 15}));
    // An unshrinking basis is held to the item's limits before the
    // container counts it: an inline flex container of one 20px item.
    flex_rects("flex: an unshrinking basis is held to its limits",
               ".x{display:inline-flex}</style><div><span class=x id=g>"
               "<div id=a style='flex:0 0 100px;min-width:0;max-width:20px;height:20px'></div>"
               "</span></div>",
               FLEX_IDS("g", "a"), FLEX_WANT({0, -1, 20, -1}, {0, -1, 20, 20}));
    // Blank text between items is no item, even where it is preserved.
    flex_rects("flex: preserved blanks between items are no items",
               "#o{display:flex;gap:10px;white-space:pre} #a,#b{width:50px;height:20px}</style>"
               "<div id=o>  <div id=a></div>  <div id=b></div>  </div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 50, 20}, {60, 0, 50, 20}));
    // A wrapped row's items are placed again in their own lines: the
    // second line of a stretched inner row centres its item in that
    // line's half of 100.
    flex_rects("flex: a stretched wrapping row places its lines again",
               "#o{display:flex;width:200px;height:100px} #i{display:flex;flex-wrap:wrap;width:100px;"
               "align-items:center} #i div{width:60px;height:20px}</style>"
               "<div id=o><div id=i><div id=a></div><div id=b></div></div></div>",
               FLEX_IDS("i", "a", "b"), FLEX_WANT({0, 0, 100, 100}, {0, 15, 60, 20}, {0, 65, 60, 20}));
    // A wrapping column stretched to 100 wraps again against it: its two
    // 60px items take a line each, the lines sized from the items' own
    // 50px widths (not the 200 the first placement stretched them to)
    // and stretched to 100 each.
    flex_rects("flex: a stretched wrapping column forms its lines again",
               "#o{display:flex;height:100px} #i{display:flex;flex-direction:column;flex-wrap:wrap;"
               "width:200px} #i>div{height:60px} .k{width:50px;height:10px}</style>"
               "<div id=o><div id=i><div id=a><div class=k></div></div><div id=b><div class=k></div>"
               "</div></div></div>",
               FLEX_IDS("i", "a", "b"), FLEX_WANT({0, 0, 200, 100}, {0, 0, 100, 60}, {100, 0, 100, 60}));
}

// Pictures and baselines (Quinn's second round on #181; Chrome's answers,
// with a 40x30 picture as the host's `known` is).
static void flex_picture_cases(void)
{
    // A picture's content width is its own 40, not a max-width's 20: two
    // bases of 40 shrink into 30 by equal weights, 15 each.
    flex_rects("flex: a picture's basis is its own width, not its limit",
               "#o{display:flex;width:30px;align-items:flex-start} img{min-width:0} #a{max-width:20px}"
               "</style><div id=o><img id=a src=known.png><img id=b src=known.png></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 15, -1}, {15, 0, 15, -1}));
    // A content basis in a column is the picture's own height, 30, not the
    // 300 the page gave; its width follows the height by its ratio.
    flex_rects("flex: a picture's content basis in a column is its own height",
               "#o{display:flex;flex-direction:column;align-items:flex-start}"
               " img{height:300px;flex:0 0 content}</style><div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({0, 0, 40, 30}));
    // Shrunk down a column to 15, a picture is 20 wide by its ratio, and
    // placed across at that width: at the start, and centred in 100.
    flex_rects("flex: a picture shrunk down a column keeps its ratio",
               "#o{display:flex;flex-direction:column;width:100px;height:15px;align-items:flex-start}"
               " img{min-height:0}</style><div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({0, 0, 20, 15}));
    flex_rects("flex: a picture shrunk down a column is centred at its new width",
               "#o{display:flex;flex-direction:column;width:100px;height:15px;align-items:center}"
               " img{min-height:0}</style><div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({40, 0, 20, 15}));
    // An inline flex container's baseline is its first item's, made from
    // its border box's bottom when it holds no text: 20, so the 20px
    // inline-block beside it sits at 0 — and the first item is the first
    // in `order`, 60 tall, which puts it at 40.
    flex_rects("flex: an inline flex container's baseline is its first item's",
               "#o{display:inline-flex;align-items:flex-start} #a{width:10px;height:20px}"
               " #b{width:10px;height:60px} #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div id=a></div><div id=b></div></div><div id=c></div></div>",
               FLEX_IDS("c"), FLEX_WANT({20, 0, 10, 20}));
    flex_rects("flex: an inline flex container's first item is first in order",
               "#o{display:inline-flex;align-items:flex-start} #a{width:10px;height:20px;order:2}"
               " #b{width:10px;height:60px} #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div id=a></div><div id=b></div></div><div id=c></div></div>",
               FLEX_IDS("c"), FLEX_WANT({20, 40, 10, 20}));
    // In a wrapping column, a picture shrunk to 15 is 20 wide, and its
    // line is: the next line starts at 20.
    flex_rects("flex: a picture shrunk down a wrapping column sizes its line",
               "#o{display:flex;flex-direction:column;flex-wrap:wrap;height:15px;width:100px;"
               "align-content:flex-start;align-items:flex-start} img{min-height:0}"
               " #d{width:10px;height:10px}</style><div id=o><img id=i src=known.png><div id=d></div></div>",
               FLEX_IDS("i", "d"), FLEX_WANT({0, 0, 20, 15}, {20, 0, 10, 10}));
    // Wrapped, only the first line's items count: the baseline-aligned
    // item on the second line does not, and the first item's bottom, 20,
    // is the container's baseline.
    flex_rects("flex: a wrapped container's baseline is its first line's",
               "#o{display:inline-flex;flex-wrap:wrap;width:20px;align-items:flex-start}"
               " #a{width:10px;height:20px} #b{width:10px;height:60px}"
               " #x{width:20px;height:30px;align-self:baseline}"
               " #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div id=a></div><div id=b></div><div id=x></div></div><div id=c></div></div>",
               FLEX_IDS("c", "x"), FLEX_WANT({20, 0, 10, 20}, {0, 60, 20, 30}));
    // A reverse direction puts its last item at the start, and that is the
    // one whose baseline counts: the 60px item, in a row and a column.
    flex_rects("flex: a row-reverse baseline is its startmost item's",
               "#o{display:inline-flex;align-items:flex-start;flex-direction:row-reverse}"
               " #a{width:10px;height:20px} #b{width:10px;height:60px}"
               " #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div id=a></div><div id=b></div></div><div id=c></div></div>",
               FLEX_IDS("c"), FLEX_WANT({20, 40, 10, 20}));
    flex_rects("flex: a column-reverse baseline is its startmost item's",
               "#o{display:inline-flex;align-items:flex-start;flex-direction:column-reverse}"
               " #a{width:10px;height:20px} #b{width:10px;height:60px}"
               " #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div id=a></div><div id=b></div></div><div id=c></div></div>",
               FLEX_IDS("c"), FLEX_WANT({10, 40, 10, 20}));
    // A picture given no width grows from 0 to 40 and takes its height
    // from its ratio, not from the 0 it started at.
    flex_rects("flex: a picture grown from width 0 keeps its ratio",
               "#o{display:flex;width:40px;align-items:flex-start} img{width:0;min-width:0;flex:1}"
               "</style><div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i", "o"), FLEX_WANT({0, 0, 40, 30}, {0, 0, 40, 30}));
    // Flexed to 40 down a column, a border-box picture keeps its 30px
    // border-box max-width.
    flex_rects("flex: a picture's width from its ratio keeps its box-sizing",
               "#o{display:flex;flex-direction:column;width:100px;height:40px;align-items:flex-start}"
               " img{flex:1;min-height:0;box-sizing:border-box;border:5px solid;max-width:30px}"
               "</style><div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({0, 0, 30, 40}));
    // Stretched, a picture's cross size is the line's, and its ratio
    // carries it into its basis: 100 wide down a column is 75 tall, and 60
    // tall in a 60px row is 80 wide.
    flex_rects("flex: a picture stretched across a column takes its height from it",
               "#o{display:flex;flex-direction:column;width:100px}</style>"
               "<div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i", "o"), FLEX_WANT({0, 0, 100, 75}, {0, 0, 100, 75}));
    flex_rects("flex: a picture stretched down a row takes its width from it",
               "#o{display:flex;width:100px;height:60px}</style>"
               "<div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({0, 0, 80, 60}));
    // A content basis counts a percentage padding at the container's
    // width: 30 of content and 20 each side, 70.
    flex_rects("flex: a content basis counts a percentage padding",
               "#o{display:flex;width:200px} #a{flex:none;padding:10%} #k{width:30px;height:20px}"
               " #b{width:50px;height:20px}</style>"
               "<div id=o><div id=a><div id=k></div></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 70, -1}, {70, 0, 50, 20}));
    // The same padding in the basis alone (no automatic minimum to lean
    // on), and in the automatic minimum alone (a basis of 0).
    // A wrapping row's line is not fixed by its height, so a stretched
    // picture keeps its own width there and is only stretched down (Chrome).
    flex_rects("flex: a picture in a wrapping row is not widened by its stretch",
               "#o{display:flex;flex-wrap:wrap;width:100px;height:60px}</style>"
               "<div id=o><img id=i src=known.png></div>",
               FLEX_IDS("i"), FLEX_WANT({0, 0, 40, 60}));
    flex_rects("flex: a content basis alone counts a percentage padding",
               "#o{display:flex;width:200px} #a{flex:none;padding:10%;min-width:0}"
               " #k{width:30px;height:20px}</style><div id=o><div id=a><div id=k></div></div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 70, -1}));
    flex_rects("flex: an automatic minimum counts a percentage padding",
               "#o{display:flex;width:200px} #a{flex:0 1 0px;padding:10%}"
               " #k{width:30px;height:20px}</style><div id=o><div id=a><div id=k></div></div></div>",
               FLEX_IDS("a"), FLEX_WANT({0, 0, 70, -1}));
}

static void flex_cases(void)
{
    flex_row_cases();
    flex_cross_cases();
    flex_column_cases();
    flex_box_cases();
    flex_wrap_cases();
    flex_review_cases();
    flex_wrap_review_cases();
    flex_nested_cases();
    flex_picture_cases();
}
