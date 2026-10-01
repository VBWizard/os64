// Grid layout (GRID.md, G1): every expectation worked by hand from Grid 2
// § 8 and § 11–§ 12 with the test font's metrics (test_libflow_fonts.c: at
// 16px a space and `l` are 4 wide, `x` 8, a line 20 high). flex_rects
// lays the page out in a viewport 400 wide; a grid wider than that says so.

static void grid_track_cases(void)
{
    // danlegt.com's home: six 1fr columns, rows of 150, gaps of 10. At
    // 1200 a column is (1200 - 50) / 6 = 191.67: four and three gaps are
    // 796.67, the next starts past a gap at 806.67, and two and a gap are
    // 393.33. Five rows and four gaps are 790.
    flex_rects("grid: danlegt's six columns at 1200",
               ".g{display:grid;width:1200px;grid-template-columns:repeat(6,1fr);"
               "grid-template-rows:repeat(12,150px);gap:10px}"
               ".a{grid-column:span 4;grid-row:span 5} .b{grid-column:span 2;grid-row:span 5}"
               "</style><div class=g><div class=a id=a></div><div class=b id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 797, 790}, {807, 0, 393, 790}));
    // At 400 a column is 350 / 6 = 58.33: 263.33, then 273.33 and 126.67.
    flex_rects("grid: danlegt's six columns at 400",
               ".g{display:grid;grid-template-columns:repeat(6,1fr);"
               "grid-template-rows:repeat(12,150px);gap:10px}"
               ".a{grid-column:span 4;grid-row:span 5} .b{grid-column:span 2;grid-row:span 5}"
               "</style><div class=g><div class=a id=a></div><div class=b id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 263, 790}, {273, 0, 127, 790}));

    // repeat(auto-fill, minmax(104px, 1fr)), gaps of 8 (§ 7.2.3.2): each
    // repeat is 104 and a gap, and n of them fit when 112n - 8 <= the
    // width. At 400, three: (400 - 16) / 3 = 128 each; the fourth item
    // wraps to the next row, 30 + 8 down.
    static const char *const kTiles =
        ".g{display:grid;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:8px}"
        ".g div div{height:30px}</style>";
    char page[2048];
    snprintf(page, sizeof(page),
             "%s<div class=g><div id=a><div></div></div><div id=b><div></div></div>"
             "<div><div></div></div><div id=d><div></div></div></div>", kTiles);
    flex_rects("grid: auto-fill at 400 makes three tracks", page, FLEX_IDS("a", "b", "d"),
               FLEX_WANT({0, 0, 128, 30}, {136, 0, 128, 30}, {0, 38, 128, 30}));
    // At 250, two: (250 - 8) / 2 = 121.
    snprintf(page, sizeof(page),
             "%s<div class=g style=width:250px><div id=a><div></div></div>"
             "<div id=b><div></div></div></div>", kTiles);
    flex_rects("grid: auto-fill at 250 makes two tracks", page, FLEX_IDS("a", "b"),
               FLEX_WANT({0, 0, 121, 30}, {129, 0, 121, 30}));
    // At 90, not even one fits: there is still one, at its minimum 104,
    // and the flexible share of a width it already passes adds nothing.
    snprintf(page, sizeof(page),
             "%s<div class=g style=width:90px><div id=a><div></div></div></div>", kTiles);
    flex_rects("grid: auto-fill narrower than one track still makes one", page,
               FLEX_IDS("a"), FLEX_WANT({0, 0, 104, 30}));
    // auto-fit drops the repeats nobody is placed in, and their gaps: two
    // items share 400 - 8.
    flex_rects("grid: auto-fit collapses the empty repeats",
               ".g{display:grid;grid-template-columns:repeat(auto-fit,minmax(104px,1fr));gap:8px}"
               "</style><div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, -1, 196, -1}, {204, -1, 196, -1}));

    // A label beside its value: 92px 1fr. Each row as tall as its tallest
    // item, every item stretched to its row.
    flex_rects("grid: 92px 1fr, rows from the content",
               ".g{display:grid;width:300px;grid-template-columns:92px 1fr;row-gap:4px}"
               "</style><div class=g><div id=a><div style=height:10px></div></div>"
               "<div id=b><div style=height:25px></div></div>"
               "<div id=c><div style=height:10px></div></div><div><div style=height:5px></div></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 92, 25}, {92, 0, 208, 25}, {0, 29, 92, 10}));
    // minmax(140px, 42%) 1fr: at 400 the room grows the first track from
    // 140 to its limit, 168; at 300 its limit, 126, is below its minimum,
    // which holds (§ 12.4).
    flex_rects("grid: minmax(140px, 42%) grows to 42%",
               ".g{display:grid;grid-template-columns:minmax(140px,42%) 1fr}"
               "</style><div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, -1, 168, -1}, {168, -1, 232, -1}));
    flex_rects("grid: minmax(140px, 42%) holds its minimum",
               ".g{display:grid;width:300px;grid-template-columns:minmax(140px,42%) 1fr}"
               "</style><div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, -1, 140, -1}, {140, -1, 160, -1}));

    // `auto 1fr` over text: "xx xxxx" is 32 at its narrowest and 52 on one
    // line; the auto track starts at 32 and the room grows it to its 52,
    // and 1fr takes the other 148 (§ 12.5, § 12.6, § 12.7).
    flex_rects("grid: an auto track takes its max-content, 1fr the rest",
               ".g{display:grid;width:200px;grid-template-columns:auto 1fr}"
               "</style><div class=g><div id=a>xx xxxx</div><div id=b>x</div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 52, 20}, {52, 0, 148, 20}));
    // 1fr's minimum is auto: a 150-wide item holds its track open, and the
    // other 1fr takes what is left, 50, rather than half (§ 12.7.1).
    flex_rects("grid: 1fr keeps its content's min-content",
               ".g{display:grid;width:200px;grid-template-columns:1fr 1fr}"
               "</style><div class=g><div id=a><div style=width:150px></div></div>"
               "<div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, -1, 150, -1}, {150, -1, 50, -1}));
    // Two auto tracks stretch into the room left, equally: 50 + 100 and
    // 150 + 100 (§ 12.8). Asked to start instead they keep their sizes;
    // centred, a gap of 10 between, they start at (400 - 210) / 2.
    flex_rects("grid: auto tracks stretch into the room",
               ".g{display:grid;grid-template-columns:auto auto}"
               "</style><div class=g><div id=a><div style=width:50px></div></div>"
               "<div id=b><div style=width:150px></div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, -1, 150, -1}, {150, -1, 250, -1}));
    flex_rects("grid: justify-content center places the tracks",
               ".g{display:grid;grid-template-columns:auto auto;justify-content:center;column-gap:10px}"
               "</style><div class=g><div id=a><div style=width:50px></div></div>"
               "<div id=b><div style=width:150px></div></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({95, -1, 50, -1}, {155, -1, 150, -1}));
    // An item across two auto tracks asks 300 of them; they hold 10 and
    // 100, so each takes half of the 190 it lacks: 105 and 195 (decision 2).
    flex_rects("grid: a spanning item shares its extra equally",
               ".g{display:grid;grid-template-columns:auto auto;justify-content:start}"
               "</style><div class=g><div id=a><div style=width:10px></div></div>"
               "<div id=b><div style=width:100px></div></div>"
               "<div id=c style=\"grid-column:span 2\"><div style=width:300px></div></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, -1, 105, -1}, {105, -1, 195, -1}, {0, -1, 300, -1}));
    // One fr, in an inline grid, is what the first track's item asks of
    // its 0.001fr: ten of the widest boxes a length can make, side by
    // side. The second track's million fr times that is past anything
    // int64_t holds, and is held instead (the sanitizers are the check).
    char wide[640] = "";
    for (int i = 0; i < 10; i++)
        strcat(wide, "<i style=display:inline-block;width:40000000px></i>");
    snprintf(page, sizeof(page),
             ".g{display:inline-grid;grid-template-columns:0.001fr 1000000fr}"
             "</style><div><span class=g id=g><div><nobr>%s</nobr></div><div id=b></div></span></div>",
             wide);
    flex_rects("grid: huge factors times a huge fr are held", page,
               FLEX_IDS("g", "b"), FLEX_WANT({-1, -1, -1, -1}, {-1, -1, -1, -1}));
}

static void grid_place_cases(void)
{
    char page[1024];
    // Four columns of 100, two rows of 50. 1 / -1 is every column; -2 is
    // the last; 2 / span 2 finds the first row free in columns 2 and 3.
    flex_rects("grid: line numbers, negative lines and spans",
               ".g{display:grid;grid-template-columns:repeat(4,100px);grid-template-rows:50px 50px}"
               "</style><div class=g><div id=a style=grid-column:1/-1></div>"
               "<div id=b style=grid-column:-2;grid-row:2></div>"
               "<div id=c style=\"grid-column:2/span 2\"></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 400, 50}, {300, 50, 100, 50}, {100, 50, 200, 50}));
    // Named areas; the 1fr row is 300 - 40 - 30.
    flex_rects("grid: template areas place by name",
               ".g{display:grid;height:300px;grid-template-columns:100px 1fr;"
               "grid-template-rows:40px 1fr 30px;grid-template-areas:'h h' 's m' 'f f'}"
               "</style><div class=g><div id=m style=grid-area:m></div>"
               "<div id=h style=grid-area:h></div><div id=f style=grid-area:f></div>"
               "<div id=s style=grid-area:s></div></div>",
               FLEX_IDS("m", "h", "f", "s"),
               FLEX_WANT({100, 40, 300, 230}, {0, 0, 400, 40}, {0, 270, 400, 30}, {0, 40, 100, 230}));
    // An area's edges are lines of their own: a-start / b-end is all three.
    // A name nobody defines is the first line past the explicit grid,
    // beyond an empty implicit column that takes nothing.
    flex_rects("grid: area edges as lines, and a name nobody defines",
               ".g{display:grid;width:300px;grid-template-columns:100px 100px 100px;"
               "grid-template-areas:'a a b'}"
               "</style><div class=g><div id=a style=\"grid-column:a-start/b-end;grid-row:2\"></div>"
               "<div id=b style=grid-column:b></div><div id=c style=grid-column:nobody></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, -1, 300, -1}, {200, -1, 100, -1}, {300, -1, 0, -1}));

    // Auto-placement across three columns of 100, rows of 20: two spans of
    // two and two singles. Sparse, the cursor never goes back: the first
    // span in row 1, the second in row 2, the singles after it. Dense,
    // each search starts over, and the first single fills row 1's hole.
    static const char *const kFill =
        "<div class=g><div style=\"grid-column:span 2\"></div><div style=\"grid-column:span 2\"></div>"
        "<div id=c></div><div id=d></div></div>";
    snprintf(page, sizeof(page),
             ".g{display:grid;grid-template-columns:repeat(3,100px);grid-auto-rows:20px}</style>%s",
             kFill);
    flex_rects("grid: sparse auto-placement", page, FLEX_IDS("c", "d"),
               FLEX_WANT({200, 20, 100, 20}, {0, 40, 100, 20}));
    snprintf(page, sizeof(page),
             ".g{display:grid;grid-template-columns:repeat(3,100px);grid-auto-rows:20px;"
             "grid-auto-flow:row dense}</style>%s", kFill);
    flex_rects("grid: dense auto-placement fills the holes", page, FLEX_IDS("c", "d"),
               FLEX_WANT({200, 0, 100, 20}, {200, 20, 100, 20}));
    // grid-auto-flow: column fills down two rows of 20 first, each new
    // column 50 wide.
    flex_rects("grid: column flow",
               ".g{display:grid;grid-template-rows:repeat(2,20px);grid-auto-columns:50px;"
               "grid-auto-flow:column}</style><div class=g><div></div><div id=b></div>"
               "<div id=c></div><div></div><div id=e></div></div>",
               FLEX_IDS("b", "c", "e"),
               FLEX_WANT({0, 20, 50, 20}, {50, 0, 50, 20}, {100, 0, 50, 20}));
    // A row given, no column: placed before the items with neither, each
    // at the first free column of its row past the ones before it; the
    // first item, placed after them, takes the column they left.
    flex_rects("grid: an item fixed to a row finds its column",
               ".g{display:grid;grid-template-columns:repeat(3,100px);grid-auto-rows:20px}"
               "</style><div class=g><div id=a></div><div id=b style=grid-row:1></div>"
               "<div id=c style=grid-row:1></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({200, 0, 100, 20}, {0, 0, 100, 20}, {100, 0, 100, 20}));
    // `order` orders the placement: -1 first, then tree order, 2 last.
    flex_rects("grid: order places the items",
               ".g{display:grid;grid-template-columns:repeat(3,100px)}"
               "</style><div class=g><div id=a style=order:2></div><div id=b></div>"
               "<div id=c style=order:-1></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({200, -1, -1, -1}, {100, -1, -1, -1}, {0, -1, -1, -1}));
    // A line past the bound (decision 4) is held to the last track, which
    // is the only one with anything in it: the rows before are empty and
    // take nothing, so the item is at the top.
    flex_rects("grid: a line past the bound is held to the last track",
               ".g{display:grid;grid-template-columns:10px}"
               "</style><div class=g id=g><div id=a style=grid-row:5000><div style=height:10px></div>"
               "</div></div>",
               FLEX_IDS("a", "g"), FLEX_WANT({0, 0, 10, 10}, {0, 0, 400, 10}));
}

static void grid_align_cases(void)
{
    // Three 1fr columns, rows sized from their items: 50, then 10.
    flex_rects("grid: rows from the items' heights",
               ".g{display:grid;width:300px;grid-template-columns:1fr 1fr}"
               "</style><div class=g><div id=a><div style=height:30px></div></div>"
               "<div id=b><div style=height:50px></div></div>"
               "<div id=c style=\"grid-column:span 2\"><div style=height:10px></div></div></div>",
               FLEX_IDS("a", "b", "c"),
               FLEX_WANT({0, 0, 150, 50}, {150, 0, 150, 50}, {0, 50, 300, 10}));
    // 1fr 3fr in a height of 200, a gap of 20: 45 and 135.
    flex_rects("grid: fr rows share a given height",
               ".g{display:grid;height:200px;grid-template-rows:1fr 3fr;row-gap:20px}"
               "</style><div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 400, 45}, {0, 65, 400, 135}));
    // justify-items center, and each item's own justify-self: a 20-wide
    // item in 100.
    flex_rects("grid: justify-self in the columns",
               ".g{display:grid;grid-template-columns:repeat(4,100px);justify-items:center}"
               ".g div div{width:20px;height:10px}</style><div class=g>"
               "<div id=a><div></div></div><div id=b style=justify-self:start><div></div></div>"
               "<div id=c style=justify-self:end><div></div></div>"
               "<div id=d style=justify-self:stretch><div></div></div></div>",
               FLEX_IDS("a", "b", "c", "d"),
               FLEX_WANT({40, -1, 20, -1}, {100, -1, 20, -1}, {280, -1, 20, -1}, {300, -1, 100, -1}));
    // align-self in a row of 60: stretched by default, then start, end,
    // centre for a 10-tall item.
    flex_rects("grid: align-self in the rows",
               ".g{display:grid;grid-template-columns:repeat(4,100px);grid-template-rows:60px}"
               ".g div div{height:10px}</style><div class=g>"
               "<div id=a><div></div></div><div id=b style=align-self:start><div></div></div>"
               "<div id=c style=align-self:end><div></div></div>"
               "<div id=d style=align-self:center><div></div></div></div>",
               FLEX_IDS("a", "b", "c", "d"),
               FLEX_WANT({-1, 0, -1, 60}, {-1, 0, -1, 10}, {-1, 50, -1, 10}, {-1, 25, -1, 10}));
    // align-content end: two rows of 30 at the bottom of 200.
    flex_rects("grid: align-content places the rows",
               ".g{display:grid;height:200px;grid-template-rows:30px 30px;align-content:end}"
               "</style><div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({-1, 140, -1, 30}, {-1, 170, -1, 30}));
    // An inline grid is as wide as its columns at max-content: the auto
    // track 30; one fr is the most the flexible tracks ask, 40 / 1 against
    // 50 / 2, so 1fr is 40 and 2fr 80; and two gaps of 5.
    flex_rects("grid: an inline grid's width",
               ".g{display:inline-grid;grid-template-columns:auto 1fr 2fr;gap:5px}"
               "</style><div><span class=g id=g><div id=a><div style=width:30px></div></div>"
               "<div><div style=width:40px></div></div><div id=c><div style=width:50px></div></div>"
               "</span></div>",
               FLEX_IDS("g", "a", "c"),
               FLEX_WANT({0, -1, 160, -1}, {0, -1, 30, -1}, {80, -1, 80, -1}));
}

// Codex's first round on flex (PR #181) found classes grid shares: an
// item's absolute boxes against its final size, a relative item's offset
// kept when its rows move it, and a layout that stops withdrawing items
// that have only their provisional places.
static void grid_review_cases(void)
{
    // A 100px row stretches the item; its bottom:0 box sits at 100 - 10.
    flex_rects("grid: an absolute box in a stretched item sits at its bottom",
               ".g{display:grid;grid-template-columns:50px;grid-template-rows:100px}"
               " .g div{position:relative} .g span{position:absolute;bottom:0;height:10px;width:10px}"
               "</style><div class=g><div id=i><span id=a></span></div></div>",
               FLEX_IDS("i", "a"), FLEX_WANT({0, 0, 50, 100}, {0, 90, 10, 10}));
    // Rows of 20; each relative item 10 below its row.
    flex_rects("grid: a relative item keeps its offset in its row",
               ".g{display:grid;grid-template-columns:100px;grid-auto-rows:20px}"
               " .g div{position:relative;top:10px}</style>"
               "<div class=g><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 10, 100, 20}, {0, 30, 100, 20}));
    // A budget that stops inside the third item's lines, with the box tree
    // whole: the first two items are withdrawn, or where the whole layout
    // puts them (flex_review_cases' sweep, for a grid).
    {
        static char page[40000];
        strcpy(page, "<!doctype html><style>body{margin:0}.g{display:grid;"
                     "grid-template-columns:1fr 1fr 1fr} .g p{margin:0}</style><div class=g>"
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
            stopped |= incomplete;
            const flow_box_t *a = flex_box(&q, t, "a"), *b = flex_box(&q, t, "b");
            ok = (a == NULL || memcmp(&a->rect, &wa->rect, sizeof(a->rect)) == 0) &&
                 (b == NULL || memcmp(&b->rect, &wb->rect, sizeof(b->rect)) == 0);
            flow_free(t);
            if (!incomplete)
                break;
        }
        expect("grid: a layout stopped in a later item withdraws the earlier ones", ok && stopped,
               stopped ? NULL : "no budget stopped the layout");
        flow_free(whole);
        sheeted_close(&q);
    }
}
// Nested containers in and around a grid (the flex slice's refit, for a

// Minimums, refits, baselines, empty axes, gaps and repeats (Quinn's
// round on #183; Chrome's answers).
static void grid_round1_cases(void)
{
    // An item's automatic minimum in its row is nothing under a
    // min-height of 0 or a scrolling overflow: two 1fr rows of 50 in 100.
    flex_rects("grid: a min-height of 0 lets a row be smaller than its content",
               "#o{display:grid;width:100px;height:100px;grid-template-rows:1fr 1fr} #a{min-height:0}"
               " #x{height:150px} #b{height:20px}</style>"
               "<div id=o><div id=a><div id=x></div></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 100, 50}, {0, 50, 100, 20}));
    flex_rects("grid: a scrolling item lets a row be smaller than its content",
               "#o{display:grid;width:100px;height:100px;grid-template-rows:1fr 1fr} #a{overflow:auto}"
               " #x{height:150px} #b{height:20px}</style>"
               "<div id=o><div id=a><div id=x></div></div><div id=b></div></div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 100, 50}, {0, 50, 100, 20}));
    // A refit that plans the grid again moves its items across too: rows
    // of 20 fill the 100 it is stretched to, so all three items share the
    // first column.
    flex_rects("grid: a refit's new plan moves items across",
               "#o{display:grid;width:100px;grid-template-rows:100px} #i{display:grid;grid-auto-flow:column;"
               "grid-template-rows:repeat(auto-fill,20px);grid-auto-columns:20px}"
               " #i>div{height:10px;width:10px}</style>"
               "<div id=o><div id=i><div id=p></div><div id=q></div><div id=r></div></div></div>",
               FLEX_IDS("p", "q", "r"), FLEX_WANT({0, 0, 10, 10}, {0, 20, 10, 10}, {0, 40, 10, 10}));
    // An inline grid's baseline is its first item's, made from its bottom
    // edge when it holds no text: 20, so the inline-block beside sits at 0.
    flex_rects("grid: an inline grid's baseline is its first item's",
               "#o{display:inline-grid;grid-template-columns:40px;grid-template-rows:20px 60px}"
               " #c{display:inline-block;width:10px;height:20px}</style>"
               "<div><div id=o><div></div><div></div></div><div id=c></div></div>",
               FLEX_IDS("c"), FLEX_WANT({40, 0, 10, 20}));
    // A grid with no items and no rows has none: grid-auto-rows makes no
    // row of its own.
    flex_rects("grid: an empty grid has no implicit row",
               "#o{display:grid;width:100px;grid-auto-rows:20px} #s{height:10px}</style>"
               "<div id=o></div><div id=s></div>",
               FLEX_IDS("o", "s"), FLEX_WANT({0, 0, 100, 0}, {0, 0, -1, 10}));
    // A percentage row gap is nothing while the height is found (40), and
    // half of it after: the second row at 40.
    flex_rects("grid: a percentage row gap is of the height it found",
               "#o{display:grid;width:100px;grid-template-rows:20px 20px;row-gap:50%}</style>"
               "<div id=o><div id=a></div><div id=b></div></div>",
               FLEX_IDS("o", "b"), FLEX_WANT({0, 0, 100, 40}, {0, 40, 100, 20}));
    // An auto-repeat of 0px counts each track as 1px: 100 columns, both
    // items on the first row.
    flex_rects("grid: an auto-repeat of nothing counts each track as a pixel",
               "#o{display:grid;width:100px;grid-template-columns:repeat(auto-fill,0px);"
               "grid-auto-rows:20px} #o>div{width:10px;height:10px}</style>"
               "<div id=o><div id=a></div><div id=b></div></div>",
               FLEX_IDS("a", "b", "o"), FLEX_WANT({0, 0, 10, 10}, {0, 0, 10, 10}, {0, 0, 100, 20}));
    // And one of half a pixel counts as a pixel: 20 in 20px, the last
    // column at 9.5 (10, as libflow rounds it).
    flex_rects("grid: an auto-repeat under a pixel counts as a pixel",
               "#o{display:grid;width:20px;grid-template-columns:repeat(auto-fill,0.5px)}"
               " #l{grid-column:-2;height:5px}</style><div id=o><div id=l></div></div>",
               FLEX_IDS("l"), FLEX_WANT({10, 0, -1, 5}));
}

// grid; Chrome's answers).
static void grid_nested_cases(void)
{
    // A flex container stretched down a grid row, and a grid container
    // stretched across a flex row, place their items against 100: the
    // 20px child centred at 40.
    flex_rects("grid: a stretched flex item in a grid centres against its row",
               "#o{display:grid;width:200px;height:100px} #i{display:flex;width:100px;align-items:center}"
               " #k{width:20px;height:20px}</style><div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("i", "k"), FLEX_WANT({0, 0, 100, 100}, {0, 40, 20, 20}));
    flex_rects("grid: a stretched grid in a flex row centres against its height",
               "#o{display:flex;width:200px;height:100px} #i{display:grid;width:100px;align-items:center}"
               " #k{width:20px;height:20px}</style><div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("i", "k"), FLEX_WANT({0, 0, 100, 100}, {0, 40, 20, 20}));
    // A grid in a grid's 100px row: its own row stretches to 100 and
    // `align-items: end` puts the child at 80.
    flex_rects("grid: a stretched grid in a grid places its rows again",
               "#o{display:grid;width:200px;grid-template-rows:100px} #i{display:grid;width:100px;"
               "align-items:end} #k{width:20px;height:20px}</style>"
               "<div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("i", "k"), FLEX_WANT({0, 0, 100, 100}, {0, 80, 20, 20}));
    // A grid's content height is its rows' as its items ask them, not the
    // height the page gave it: a content basis of 20.
    flex_rects("grid: a grid's content basis is its items'",
               "#o{display:flex;flex-direction:column;width:200px} #i{display:grid;height:300px;"
               "flex:0 0 content} #k{width:20px;height:20px}</style>"
               "<div id=o><div id=i><div id=k></div></div></div>",
               FLEX_IDS("o", "i"), FLEX_WANT({0, 0, 200, 20}, {0, 0, 200, 20}));
    // An absolute box in a nested grid's item waits for the outermost
    // container: the item stretched to 100 twice over, its bottom:0 box at
    // 90.
    flex_rects("grid: an absolute box in a nested grid's item sits at its final bottom",
               "#o{display:flex;height:100px} #i{display:grid;width:100px} #j{position:relative}"
               " #a{position:absolute;bottom:0;width:10px;height:10px}</style>"
               "<div id=o><div id=i><div id=j><span id=a></span></div></div></div>",
               FLEX_IDS("j", "a"), FLEX_WANT({0, 0, 100, 100}, {0, 90, 10, 10}));
    // Blank text between grid items is no item, even where it is preserved.
    flex_rects("grid: preserved blanks between items are no items",
               "#o{display:grid;white-space:pre;grid-template-columns:50px 50px;column-gap:10px}"
               " #a,#b{height:20px}</style><div id=o>  <div id=a></div>  <div id=b></div>  </div>",
               FLEX_IDS("a", "b"), FLEX_WANT({0, 0, 50, 20}, {60, 0, 50, 20}));
}


static void grid_cases(void)
{
    grid_track_cases();
    grid_place_cases();
    grid_align_cases();
    grid_review_cases();
    grid_nested_cases();
    grid_round1_cases();
}
