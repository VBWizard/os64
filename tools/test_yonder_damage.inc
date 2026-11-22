/* N3, THE PAINT FOLLOWS THE CHANGE (LAYOUT_NEEDS_N3.md): a script's change
 * repaints what it changed. Each case paints the window whole, makes a
 * change to the document, rebuilds as a script's task would
 * (script_rebuild), and paints what the rebuild marked with --damage-check
 * on: the whole view painted the ordinary way must agree with the parts
 * to the pixel, a rebuild that marked nothing included, and the rebuild
 * must have been painted as the case says (s_change_said). */

static os64_draw_ctx_t damage_screen;

static const char kDamagePage[] =
    "<!doctype html><style>body{margin:0}#box{width:100px;height:30px;background:#ccc}"
    "#clock{display:inline-block}#box.on{background:red}"
    "#bar{width:300px;height:20px;display:flex}#bar>div{width:50px;background:blue}"
    "#list{height:100px}#ol{height:80px}#gen::before{content:attr(data-x)}"
    "#wrap{width:50px;opacity:.5}#wide{width:200px;background:green}"
    "#posbox{position:relative;height:40px}#abs{position:absolute;left:0;top:0}"
    "#filler{height:900px}#back{position:absolute;left:0;top:0;width:860px;height:420px;z-index:-1}#mover{width:40px;height:16px;background:purple;transform:translateX(200px)}"
    ".hot circle{opacity:.2}#wrap2{width:50px}#wide2{width:200px;background:green}</style>"
    "<div id=back></div><div id=box>box</div>"
    "<div id=mover></div>"
    "<div id=sw><svg width=40 height=24><circle cx=12 cy=12 r=10 fill=red></circle></svg>"
    "<svg id=sv width=60 height=24><rect x=1 y=1 width=8 height=8 fill=red></rect></svg></div>"
    "<div id=wrap2><div id=wide2>wide wide wide</div></div>"
    "<p>time <span id=clock>0:01</span> left</p>"
    "<div id=bar><div id=b1>a</div><div id=b2>b</div></div>"
    "<div id=meter aria-valuenow=1 data-v=1>meter</div>"
    "<div id=list><div>one</div></div>"
    "<div id=gen data-x=one></div>"
    "<div id=wrap><div id=wide>wide wide wide</div></div>"
    "<div id=posbox><div id=abs>abs</div></div>"
    "<svg id=pic width=40 height=40><circle id=dot cx=20 cy=20 r=10 fill=red></circle></svg>"
    "<ol id=ol><li id=li1>one</li><li>two</li></ol>"
    "<a id=link href=#x>a link</a>"
    "<div id=filler></div>";

/* The page shown and painted whole, nothing marked since. */
static void damage_begin(const char *html)
{
    probe_page(html, false);
    g.page.diag = diag_new_claimed("https://fixture.test/damage", 1);
    g.hover_link = g.pressed_link = -1;
    s_layout_profile = true;
    // The window given a size and laid out, and the page laid out at the
    // view that leaves: a rebuild that moves the window's bars (the badge
    // a record's missing features show) lays the window out again, and
    // must find it the size it was.
    g.root.bounds = (os64_gui_rect_t){0, 0, 900, 720};
    layout();
    page_lay_out(&g.page, g.view.bounds.w, g.view.bounds.h, g.zoom, &g.page);
    g.page.model_version = g.page.rendered_version = os64_html_version(g.page.way.doc);
    damage_screen = (os64_draw_ctx_t){0};
    damage_screen.surf.width = damage_screen.surf.pitch_px = 900;
    damage_screen.surf.height = 720;
    damage_screen.surf.pixels = os64_calloc(900u * 720u, sizeof(uint32_t));
    g.paint_cache = yonder_paint_cache_new(32u << 20);
    g.paint_scratch = yonder_scratch_new();
    g.ui.dirty = g.view.bounds;
    view_paint(&g.view, &damage_screen, NULL);
    g.ui.dirty = (os64_gui_rect_t){0, 0, 0, 0};
}

static void damage_end(void)
{
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache = NULL;
    yonder_scratch_free(g.paint_scratch);
    g.paint_scratch = NULL;
    os64_free(damage_screen.surf.pixels);
    s_layout_profile = false;
    probe_drop();
}

typedef struct {
    uint64_t compared, differed;
    bool whole;
    const char *said;
} DamageSaid;

/* The rebuild after a change, and the paint of what it marked, checked. */
static DamageSaid damage_step(void)
{
    uint64_t compared = s_damage_compared, differed = s_damage_differed;
    s_damage_check = 1;
    s_damage_counted = 0;
    s_change_said = "(no rebuild)";
    script_rebuild();
    DamageSaid said = {0, 0, false, s_change_said};
    os64_gui_rect_t v = g.view.bounds, d = g.ui.dirty;
    said.whole = d.x <= v.x && d.y <= v.y && d.x + d.w >= v.x + v.w && d.y + d.h >= v.y + v.h;
    view_paint(&g.view, &damage_screen, NULL);
    s_damage_check = 0;
    s_damage_due = false;
    g.ui.dirty = (os64_gui_rect_t){0, 0, 0, 0};
    said.compared = s_damage_compared - compared;
    said.differed = s_damage_differed - differed;
    return said;
}

/* The step painted as `expect` says (NULL: anything but the whole view),
 * and, when it was painted in parts or not at all, checked and agreeing. */
static void damage_expect(const char *what, DamageSaid s, const char *expect)
{
    bool parts = os64_streq(s.said, "in parts") || os64_streq(s.said, "nothing to paint");
    bool ok = (expect != NULL ? os64_streq(s.said, expect) : parts) && s.differed == 0 &&
              (!parts || (s.compared == 1 && !s.whole));
    check(ok, what);
    if (!ok)
        printf("  %s: said %s, whole %d, compared %lu, differed %lu\n", what, s.said, (int)s.whole,
               (unsigned long)s.compared, (unsigned long)s.differed);
}

static void damage_style(const char *id, const char *style)
{
    os64_html_set_attr(g.page.way.doc, probe_id(id), "style", style, strlen(style));
}

static void damage_attr(const char *id, const char *name, const char *value)
{
    os64_html_set_attr(g.page.way.doc, probe_id(id), name, value, strlen(value));
}

static void damage_text(const char *id, const char *text)
{
    os64_html_set_text(g.page.way.doc, probe_id(id)->first_child, text, strlen(text));
}

/* A new `tag` holding `text`, put under `parent` before `before` (NULL: last). */
static void damage_insert(const char *parent, const char *tag, const char *text, const char *before)
{
    int64_t status = 0;
    os64_html_document_t *doc = g.page.way.doc;
    os64_html_node_t *el = os64_html_create_element(doc, OS64_HTML_NS_HTML, tag, &status);
    os64_html_node_t *t = os64_html_create_text(doc, text, strlen(text), &status);
    os64_html_insert(doc, el, t, NULL);
    os64_html_insert(doc, probe_id(parent), el, before != NULL ? probe_id(before) : NULL);
}

/* THE RANDOM WALK (N3): from a fixed seed, a few changes of every kind a
 * script makes (paint-only and placing styles, classes, text, aria and data
 * attributes, an svg's shapes, generated text, children added and taken,
 * list items put first), then a rebuild and the paint of what it marked,
 * every one checked against the whole view. None may differ, and most must
 * be painted in parts or not at all. */
static const char kDamageWalkPage[] =
    "<!doctype html><style>body{margin:0}.on{background:red}.wide{width:220px}.pos{position:relative;left:3px}"
    ".f{filter:blur(1px)}.box{height:24px}#c1,#c2{height:90px}#g1::before{content:attr(data-x)}"
    "#flex{display:flex;width:400px;height:24px}#o1{height:120px}#tail{height:900px}</style>"
    "<div id=t1 class=box>alpha</div><div id=t2 class=box>beta</div>"
    "<p>at <span id=t3>0:00</span> of <span id=t4>9:59</span></p>"
    "<div id=flex><div id=t5>e</div><div id=t6>f</div></div>"
    "<div id=c1><div>one</div></div><div id=c2><span>two</span></div>"
    "<div id=g1 data-x=g></div>"
    "<svg id=s1 width=60 height=30><rect id=r1 x=2 y=2 width=20 height=20 fill=red></rect>"
    "<circle id=k1 cx=40 cy=15 r=8 fill=blue></circle></svg>"
    "<ol id=o1><li id=l1>one</li></ol>"
    "<div id=tail></div>";

static uint32_t damage_seed;
static uint32_t damage_r(uint32_t n)
{
    damage_seed = damage_seed * 1103515245u + 12345u;
    return (damage_seed >> 16) % n;
}

static void damage_walk_change(void)
{
    static const char *const kEls[] = {"t1", "t2", "t3", "t4", "t5", "t6", "c1", "c2", "g1"};
    static const char *const kPaint[] = {"color:red", "color:blue", "background:#abc", "opacity:.5", "opacity:.9",
                                         "box-shadow:0 0 4px red", "outline:2px solid green", ""};
    static const char *const kPlace[] = {"width:60px", "width:140px", "padding-left:9px", "font-size:20px"};
    static const char *const kClass[] = {"box", "box on", "box wide", "box pos", "box f", "on f"};
    static const char *const kText[] = {"0:01", "0:02", "10:00", "1:23:45", "x", "a longer text"};
    const char *id = kEls[damage_r(sizeof(kEls) / sizeof(kEls[0]))];
    char v[32];
    switch (damage_r(10)) {
    case 0: damage_style(id, kPaint[damage_r(sizeof(kPaint) / sizeof(kPaint[0]))]); break;
    case 1: damage_style(id, kPlace[damage_r(sizeof(kPlace) / sizeof(kPlace[0]))]); break;
    case 2: damage_attr(id, "class", kClass[damage_r(sizeof(kClass) / sizeof(kClass[0]))]); break;
    case 3: {
        const char *t = damage_r(2) ? "t3" : "t4";
        damage_text(t, kText[damage_r(sizeof(kText) / sizeof(kText[0]))]);
        break;
    }
    case 4:
        snprintf(v, sizeof(v), "%u", damage_r(100));
        damage_attr(id, damage_r(2) ? "aria-valuenow" : "data-n", v);
        break;
    case 5:
        snprintf(v, sizeof(v), "%u", 5 + damage_r(20));
        if (damage_r(2))
            damage_attr("r1", damage_r(2) ? "fill" : "width", damage_r(2) ? "green" : v);
        else
            damage_attr("k1", damage_r(2) ? "cx" : "r", v);
        break;
    case 6: damage_insert(damage_r(2) ? "c1" : "c2", damage_r(2) ? "div" : "span", "more", NULL); break;
    case 7: {
        os64_html_node_t *c = probe_id(damage_r(2) ? "c1" : "c2");
        if (c->first_child != NULL && c->first_child->next != NULL)
            os64_html_remove(g.page.way.doc, c->first_child);
        break;
    }
    case 8:
        snprintf(v, sizeof(v), "%.*s", 1 + (int)damage_r(6), "ghijkl");
        damage_attr("g1", "data-x", v);
        break;
    default: damage_insert("o1", "li", "first", NULL); break;
    }
}

static void damage_walk_cases(void)
{
    damage_begin(kDamageWalkPage);
    damage_seed = 0x5eed;
    uint64_t differed = 0, compared = 0, parts = 0, nothing = 0, whole = 0;
    struct { const char *why; unsigned n; } whys[16];
    int nwhys = 0;
    for (int step = 0; step < 300; step++) {
        for (uint32_t k = 1 + damage_r(3); k > 0; k--)
            damage_walk_change();
        DamageSaid s = damage_step();
        differed += s.differed;
        compared += s.compared;
        parts += os64_streq(s.said, "in parts");
        nothing += os64_streq(s.said, "nothing to paint");
        if (!os64_streq(s.said, "in parts") && !os64_streq(s.said, "nothing to paint")) {
            whole++;
            int k = 0;
            while (k < nwhys && !os64_streq(whys[k].why, s.said))
                k++;
            if (k == nwhys && nwhys < 16)
                whys[nwhys++] = (__typeof__(whys[0])){s.said, 0};
            if (k < nwhys)
                whys[k].n++;
        }
        if (s.differed > 0)
            printf("  damage walk step %d differed (said %s)\n", step, s.said);
    }
    printf("  damage walk: %lu in parts, %lu nothing to paint, %lu whole; %lu compared, %lu differed\n",
           (unsigned long)parts, (unsigned long)nothing, (unsigned long)whole, (unsigned long)compared,
           (unsigned long)differed);
    for (int k = 0; k < nwhys; k++)
        printf("    whole, %s: %u\n", whys[k].why, whys[k].n);
    check(differed == 0 && compared == parts + nothing && parts > 100,
          "damage walk: every rebuild painted in parts or not at all agrees with the whole view, and most are");
    damage_end();
}

static void damage_cases(void)
{
    damage_begin(kDamagePage);
    // The first rebuild puts up the window's badge, which lays the window
    // out and marks it whole: a step of its own, before the cases.
    damage_attr("meter", "data-v", "0");
    (void)damage_step();
    damage_style("box", "color:blue");
    damage_expect("damage: a paint-only style written in place is painted in parts", damage_step(), "in parts");
    damage_attr("box", "class", "on");
    damage_expect("damage: a class that changes a background is painted in parts", damage_step(), "in parts");
    damage_style("mover", "transform:translateX(10px)");
    damage_expect("damage: a transform written in place marks where the box was and where it is", damage_step(),
                  "in parts");
    damage_attr("sw", "class", "hot");
    damage_expect("damage: a class that restyles an svg's shape marks the svg that draws it", damage_step(),
                  "in parts");
    {
        int64_t status = 0;
        os64_html_node_t *rect = os64_html_create_element(g.page.way.doc, OS64_HTML_NS_SVG, "rect", &status);
        os64_html_set_attr(g.page.way.doc, rect, "x", "20", 2);
        os64_html_set_attr(g.page.way.doc, rect, "width", "20", 2);
        os64_html_set_attr(g.page.way.doc, rect, "height", "20", 2);
        os64_html_insert(g.page.way.doc, probe_id("sv"), rect, NULL);
    }
    damage_expect("damage: a shape added to an svg marks the svg that draws it", damage_step(), "in parts");
    damage_style("wrap2", "overflow:clip");
    damage_expect("damage: a clip that cuts what overflows a box marks all it painted", damage_step(), "in parts");
    damage_text("clock", "0:02");
    damage_expect("damage: a clock's text of the same width is painted in parts", damage_step(), "in parts");
    damage_text("clock", "10:00");
    damage_expect("damage: a clock's text that widens is a move inside its line, painted in parts", damage_step(),
                  "in parts");
    damage_attr("meter", "aria-valuenow", "2");
    damage_attr("meter", "data-v", "2");
    damage_expect("damage: aria and data attributes on a box paint nothing, and that is checked", damage_step(),
                  "nothing to paint");
    damage_style("b1", "width:80px");
    damage_expect("damage: a flex item that widens moves its sibling inside the bar, painted in parts",
                  damage_step(), "in parts");
    damage_insert("list", "div", "two", NULL);
    damage_expect("damage: a child added inside a box of a set height is painted in parts", damage_step(),
                  "in parts");
    damage_attr("gen", "data-x", "two");
    damage_expect("damage: a data attribute a generated text reads is painted in parts", damage_step(), "in parts");
    damage_style("wrap", "opacity:.8");
    damage_expect("damage: an opacity marks everything inside it, what overflows it too", damage_step(),
                  "in parts");
    damage_attr("dot", "fill", "blue");
    damage_expect("damage: an svg shape's attribute marks the svg that draws it", damage_step(), "in parts");
    damage_insert("ol", "li", "zero", "li1");
    damage_expect("damage: an item put first renumbers the list's markers, painted in parts", damage_step(),
                  "in parts");
    damage_style("back", "background:#fee");
    damage_expect("damage: parts that reach across more than half the view are the whole view", damage_step(),
                  "parts past half the view");
    damage_style("abs", "left:30px");
    damage_expect("damage: a positioned box that moves is the whole view", damage_step(), kRefused[FLOW_PARTS_MOVED]);
    damage_insert("filler", "div", "past the end", NULL);
    damage_style("filler", "height:1400px");
    damage_expect("damage: a page that grows is the whole view", damage_step(), kRefused[FLOW_PARTS_MOVED]);
    g.hover_link = 0;
    damage_style("box", "color:green");
    damage_expect("damage: a link lit when the rebuild puts it out paints nothing of its own", damage_step(),
                  "in parts");
    g.hover_link = -1;
    damage_style("box", "");
    os64_html_set_attr(g.page.way.doc, (os64_html_node_t *)os64_html_document_element(g.page.way.doc), "style",
                       "background:yellow", 17);
    damage_expect("damage: a background the canvas takes is the whole view", damage_step(),
                  kRefused[FLOW_PARTS_CANVAS]);
    damage_end();
    damage_walk_cases();
}
