// `filter` and `backdrop-filter` (FILTER.md): either other than none makes
// a box a stacking context and the containing block of its absolute and
// fixed descendants; a `filter` makes it a group whose bounds are grown by
// what the filter spreads (flow_filter_reach), and a change inside one
// repaints all of it. The expected rects are worked by hand: a blur of
// standard deviation s reaches 3s (three box blurs of radius s); a drop
// shadow's side reaches its blur's reach less its offset toward the other
// side, never below 0.

// One group's walk at the top of the page: its log and the first bounds.
static GroupLog filter_walk(const char *html, int32_t width)
{
    Sheeted q;
    GroupLog g = {{0}, 0, {{0}}, 0};
    sheeted_open(&q, html, width, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, width, &q.env);
    flow_visitor_t v = {glog_visit, glog_open, glog_close, &g, NULL};
    if (t != NULL)
        flow_visit_groups(t, (os64_gui_rect_t){0, 0, width, 600}, (flow_point_t){0, 0}, &v);
    flow_free(t);
    sheeted_close(&q);
    return g;
}

static void filter_group_cases(void)
{
    char detail[640];
#define FILTER_PAGE(rule) "<!doctype html><style>body{margin:0} div{margin:0}" rule \
                          "</style><div id=f class=f>a</div>"
    // blur(2px): 3 x 2 = 6 every side of the 400 x 20 box.
    GroupLog g = filter_walk(FILTER_PAGE(".f{filter:blur(2px);height:20px}"), 400);
    snprintf(detail, sizeof(detail), "%s %d %d %d %d", g.log, g.bounds[0].x, g.bounds[0].y,
             g.bounds[0].w, g.bounds[0].h);
    expect("filter: a blur is a group, its bounds grown by three deviations",
           g.opens == 1 && strcmp(g.log, "( a ) ") == 0 && rect_is(g.bounds[0], -6, -6, 412, 32),
           detail);

    // A drop shadow cast 4 right, unblurred, reaches 4 past the right side
    // and nothing past the left.
    g = filter_walk(FILTER_PAGE(".f{filter:drop-shadow(red 4px 0px);height:20px}"), 400);
    expect("filter: a drop shadow reaches only where it is cast",
           g.opens == 1 && rect_is(g.bounds[0], 0, 0, 404, 20), g.log);

    // Cast 4 left and 3 down, a deviation of 1 (the third length is a
    // standard deviation, as blur()'s is: reach 3): top 3 - 3 = 0, right
    // 3 - 4 < 0, bottom 3 + 3, left 3 + 4.
    g = filter_walk(FILTER_PAGE(".f{filter:drop-shadow(red -4px 3px 1px);height:20px}"), 400);
    snprintf(detail, sizeof(detail), "%d %d %d %d", g.bounds[0].x, g.bounds[0].y, g.bounds[0].w,
             g.bounds[0].h);
    expect("filter: a blurred drop shadow reaches by its blur and its offsets",
           g.opens == 1 && rect_is(g.bounds[0], -7, 0, 407, 26), detail);

    // In order: the blur (reach 3) and then the shadow of what it made
    // (cast 10 right, unblurred): right 3 + 10, the rest 3.
    g = filter_walk(FILTER_PAGE(".f{filter:blur(1px) drop-shadow(red 10px 0);height:20px}"), 400);
    snprintf(detail, sizeof(detail), "%d %d %d %d", g.bounds[0].x, g.bounds[0].y, g.bounds[0].w,
             g.bounds[0].h);
    expect("filter: each function spreads what the ones before it made",
           g.opens == 1 && rect_is(g.bounds[0], -3, -3, 416, 26), detail);

    // A calc() amount is worked out, not refused: the filter is drawn.
    g = filter_walk(FILTER_PAGE(".f{filter:invert(calc(1)) brightness(calc(-10%));height:20px}"), 400);
    expect("filter: calc() amounts are worked out", g.opens == 1, g.log);

    // A colour filter spreads nothing, and is a group all the same.
    g = filter_walk(FILTER_PAGE(".f{filter:grayscale(1);height:20px}"), 400);
    expect("filter: a colour filter is a group that spreads nothing",
           g.opens == 1 && rect_is(g.bounds[0], 0, 0, 400, 20), g.log);

    // A list holding a reference draws unfiltered: still a group, spreading
    // nothing.
    g = filter_walk(FILTER_PAGE(".f{filter:url(#x) blur(5px);height:20px}"), 400);
    expect("filter: a list with a reference spreads nothing",
           g.opens == 1 && rect_is(g.bounds[0], 0, 0, 400, 20), g.log);

    // A blur past FLOW_FILTER_BLUR_MAX reaches as that one does.
    g = filter_walk(FILTER_PAGE(".f{filter:blur(500px);height:20px}"), 400);
    expect("filter: a blur past the most is held at it",
           g.opens == 1 && g.bounds[0].x == -3 * FLOW_FILTER_BLUR_MAX, g.log);

    // A backdrop-filter needs no layer: no group.
    g = filter_walk(FILTER_PAGE(".f{backdrop-filter:blur(4px);height:20px}"), 400);
    expect("filter: a backdrop-filter opens no group", g.opens == 0, g.log);

    // A plain inline's filter is not drawn: no group.
    g = filter_walk("<!doctype html><p><span style=\"filter:blur(1px)\">t</span>", 400);
    expect("filter: a plain inline's filter opens no group", g.opens == 0, g.log);
    g = filter_walk("<!doctype html><p><span style=\"display:inline-block;filter:blur(1px)\">t</span>",
                    400);
    expect("filter: an inline-block's filter opens no group", g.opens == 0, g.log);
#undef FILTER_PAGE

    // Members a filter's blur reaches into the view from outside it are
    // painted: the view the group's boxes are culled against is grown by
    // the spread. The member is 5px past the view's right edge; the blur
    // reaches 30.
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#f{filter:blur(10px);position:relative;height:20px}"
                     "#m{position:absolute;left:405px;top:0;width:10px;height:10px}</style>"
                     "<div id=f><div id=m><span>b</span></div></div>",
                 800, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 800, &q.env);
    GroupLog c = {{0}, 0, {{0}}, 0};
    flow_visitor_t cv = {glog_visit, glog_open, glog_close, &c, NULL};
    if (t != NULL)
        flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, (flow_point_t){0, 0}, &cv);
    expect("filter: a member the blur reaches into the view is painted",
           strcmp(c.log, "( b ) ") == 0, c.log);
    flow_free(t);
    sheeted_close(&q);
}

static void filter_layout_cases(void)
{
    flow_point_t top = {0, 0};
    // The containing block of an absolute and of a fixed box inside it,
    // and the fixed one scrolls with it: placed from its top, 100 down.
    static const char *const kRules[] = {"filter:grayscale(1)", "backdrop-filter:blur(2px)"};
    for (int i = 0; i < 2; i++) {
        char html[600];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body{margin:0} div{margin:0}"
                 "#f{%s;height:10px} .p{width:10px;height:10px}"
                 "#a{position:absolute;top:5px} #x{position:fixed;top:7px}</style>"
                 "<div style=\"height:100px\"></div>"
                 "<div id=f><div id=a class=p></div><div id=x class=p></div></div>"
                 "<div style=\"height:2000px\"></div>",
                 kRules[i]);
        Sheeted q;
        sheeted_open(&q, html, 400, 600);
        flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *f = box_with_id(t, q.doc->document, "f");
        const flow_box_t *a = box_with_id(t, q.doc->document, "a");
        const flow_box_t *x = box_with_id(t, q.doc->document, "x");
        flow_point_t down = {0, 50};
        bool ok = f != NULL && a != NULL && x != NULL && f->stacked && !f->positioned &&
                  flow_box_doc_rect(a, top).y == 105 && flow_box_doc_rect(x, top).y == 107 &&
                  !x->fixed && flow_box_doc_rect(x, down).y == 107;
        char what[120];
        snprintf(what, sizeof(what),
                 "filter: %s is a stacking context and contains absolute and fixed boxes",
                 kRules[i]);
        char detail[120];
        snprintf(detail, sizeof(detail), "a %d x %d fixed %d",
                 a != NULL ? flow_box_doc_rect(a, top).y : -1,
                 x != NULL ? flow_box_doc_rect(x, top).y : -1, x != NULL && x->fixed);
        expect(what, ok, detail);
        flow_free(t);
        sheeted_close(&q);
    }
}

// The root's filter makes it no containing block (Filter Effects 1 § 5):
// a fixed header stays fixed to the viewport.
static void filter_root_cases(void)
{
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>html{filter:grayscale(1)} body{margin:0}"
                     "#h{position:fixed;top:0;width:200px;height:20px}</style>"
                     "<div id=h></div><div style=\"height:3000px\"></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *h = box_with_id(t, q.doc->document, "h");
    expect("filter: the root's filter leaves a fixed box fixed",
           h != NULL && h->fixed && flow_box_doc_rect(h, (flow_point_t){0, 500}).y == 500, NULL);
    flow_free(t);
    sheeted_close(&q);
}

static void filter_computed_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>"
                     "#f{color:rgb(9,9,9);filter:blur(2px) grayscale(50%) hue-rotate(0.5turn) "
                     "drop-shadow(1px 2px 3px) url(#q) invert() opacity(200%)}"
                     "#b{backdrop-filter:brightness(1.5) saturate(180%)}"
                     "#n{filter:none}</style>"
                     "<div id=f></div><div id=b></div><div id=n></div>",
                 800, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    const os64_html_node_t *root = p.doc->document;
    flow_computed_t *c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "f"), NULL)
                                      : NULL;
    computed_is(c, "computed: filter, each function as computed", GARB_FILTER,
                "blur(2px) grayscale(0.5) hue-rotate(180deg) drop-shadow(rgb(9, 9, 9) 1px 2px 3px) "
                "url(\"#q\") invert(1) opacity(1)");
    computed_is(c, "computed: no backdrop-filter", GARB_BACKDROP_FILTER, "none");
    flow_computed_free(c);
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "b"), NULL) : NULL;
    computed_is(c, "computed: backdrop-filter", GARB_BACKDROP_FILTER, "brightness(1.5) saturate(1.8)");
    flow_computed_free(c);
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "n"), NULL) : NULL;
    computed_is(c, "computed: filter none", GARB_FILTER, "none");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
}

// A change to a box inside a filtered group repaints all the filter
// spreads it over: the group's bounds, with the group's box.
static void filter_mark_cases(void)
{
    static const char kPage[] = MOTION_STYLE "@keyframes bg{to{background-color:blue}}"
        "#f{filter:blur(2px)} #c{animation:bg 1s linear infinite}</style>"
        "<div id=f><div id=c></div></div>";
    const flow_point_t o = {0, 0};
    Moving m;
    moving_open_for(&m, kPage, 0, true);
    const flow_box_t *f = moving_box(&m, "f");
    TickMarks k = {.n = 0};
    (void)flow_motion_tick(m.tree, 500, o, tick_mark, &k, NULL);
    expect("tick marks: a box in a filtered group, the group's spread bounds",
           f != NULL && marked(&k, f, (os64_gui_rect_t){-6, -6, 112, 52}), NULL);
    moving_close(&m);

    moving_open(&m, kPage, 0);
    f = moving_box(&m, "f");
    flow_tree_t *now = moving_beside(&m, 500);
    k.n = 0;
    bool bounded = now != NULL && flow_layout_parts(m.tree, now, o, tick_mark, &k, NULL);
    expect("layout parts: a box in a filtered group, the group's spread bounds",
           f != NULL && bounded && marked(&k, f, (os64_gui_rect_t){-6, -6, 112, 52}), NULL);
    flow_free(now);
    moving_close(&m);
}

// An animated box outside the view that an ancestor's blur spreads into
// it is seen: its ticks are due (box_seen sums every filtered ancestor's
// reach). Codex #445 round 1.
static void filter_seen_cases(void)
{
    static const char kPage[] = MOTION_STYLE "@keyframes bg{to{background-color:blue}}"
        "#f{filter:blur(10px);position:relative;width:300px}"
        "#c{position:absolute;left:205px;top:0;width:10px;height:10px;"
        "animation:bg 1s linear infinite}</style><div id=f><div id=c></div></div>";
    Moving m;
    moving_open_for(&m, kPage, 0, true);
    uint64_t due = flow_motion_tick_due(m.tree, (os64_gui_rect_t){0, 0, 200, 600}, (flow_point_t){0, 0});
    expect("filter: an animation an ancestor's blur spreads into view is seen", due != FLOW_NEVER,
           NULL);
    moving_close(&m);
}

// A backdrop reads what lies within its reach of its border box, so a
// part repaint it reads takes it in whole (FILTER.md § 7): blur(4px)
// reaches 12, so a change 5 below a 60-high header joins it, one 40 below
// does not.
static void filter_backdrop_damage_cases(void)
{
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#h{position:fixed;top:0;width:400px;height:60px;backdrop-filter:blur(4px)}</style>"
                     "<div id=h></div><div style=\"height:3000px\"></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    flow_point_t top = {0, 0};
    os64_gui_rect_t near = flow_backdrop_damage(t, (os64_gui_rect_t){10, 65, 20, 5}, top);
    os64_gui_rect_t far = flow_backdrop_damage(t, (os64_gui_rect_t){10, 100, 20, 5}, top);
    char detail[96];
    snprintf(detail, sizeof(detail), "near %d %d %d %d far %d %d %d %d", near.x, near.y, near.w,
             near.h, far.x, far.y, far.w, far.h);
    expect("backdrop: a part within the reach takes the box in, one beyond does not",
           rect_is(near, 0, 0, 400, 70) && rect_is(far, 10, 100, 20, 5), detail);
    // Transitive: B beside A reads what A's repaint changes (the cold
    // read's first finding). A blur(10px) reaches 30, B's blur(8px) 24; a
    // point inside A joins A, A's repaint reaches B, so B is joined too.
    Sheeted two;
    sheeted_open(&two, "<!doctype html><style>body{margin:0}"
                       "#a{position:fixed;left:0;top:0;width:100px;height:100px;backdrop-filter:blur(10px)}"
                       "#b{position:fixed;left:80px;top:0;width:100px;height:100px;backdrop-filter:blur(8px)}"
                       "</style><div id=a></div><div id=b></div>",
                 400, 600);
    flow_tree_t *t2 = flow_layout(two.doc, two.page, 400, &two.env);
    os64_gui_rect_t both = flow_backdrop_damage(t2, (os64_gui_rect_t){50, 50, 1, 1}, top);
    snprintf(detail, sizeof(detail), "%d %d %d %d", both.x, both.y, both.w, both.h);
    expect("backdrop: a box drawn again joins the backdrop beside it", rect_is(both, 0, 0, 180, 100),
           detail);
    flow_free(t2);
    sheeted_close(&two);
    // A box's own shadow lies in its backdrop's margin: it is joined too.
    Sheeted sh;
    sheeted_open(&sh, "<!doctype html><style>body{margin:0}"
                      "#c{position:fixed;left:100px;top:100px;width:100px;height:100px;"
                      "backdrop-filter:blur(4px);box-shadow:0 0 0 10px black}</style><div id=c></div>",
                 400, 600);
    flow_tree_t *t3 = flow_layout(sh.doc, sh.page, 400, &sh.env);
    os64_gui_rect_t shadowed = flow_backdrop_damage(t3, (os64_gui_rect_t){150, 150, 1, 1}, top);
    snprintf(detail, sizeof(detail), "%d %d %d %d", shadowed.x, shadowed.y, shadowed.w, shadowed.h);
    expect("backdrop: a box's own shadow is joined with it", rect_is(shadowed, 90, 90, 120, 120), detail);
    flow_free(t3);
    sheeted_close(&sh);
    // Scrolled, the fixed header is where the scroll puts it.
    os64_gui_rect_t scrolled = flow_backdrop_damage(t, (os64_gui_rect_t){10, 565, 20, 5}, (flow_point_t){0, 500});
    expect("backdrop: a fixed box's damage at the scroll", rect_is(scrolled, 0, 500, 400, 70), NULL);
    flow_free(t);
    sheeted_close(&q);
}

// mix-blend-mode and isolation (MIX_BLEND.md): a blended block is a
// stacking context and a group; an isolated one a stacking context and no
// group; a plain inline's blend does nothing; both are read back.
static void blend_mode_cases(void)
{
    GroupLog g = filter_walk("<!doctype html><style>body{margin:0}"
                             ".b{mix-blend-mode:multiply;height:20px}</style><div class=b>a</div>", 400);
    expect("blend: a blended block is one group", g.opens == 1 && strcmp(g.log, "( a ) ") == 0, g.log);
    g = filter_walk("<!doctype html><style>body{margin:0}"
                    ".i{isolation:isolate;height:20px}</style><div class=i>a</div>", 400);
    expect("blend: an isolated block opens no group", g.opens == 0, g.log);
    g = filter_walk("<!doctype html><p><span style=\"mix-blend-mode:screen\">t</span>", 400);
    expect("blend: a plain inline's blend opens no group", g.opens == 0, g.log);
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} div{height:10px}"
                     ".b{mix-blend-mode:difference} .i{isolation:isolate}</style>"
                     "<div id=b class=b></div><div id=i class=i></div><div id=n></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *b = box_with_id(t, q.doc->document, "b");
    const flow_box_t *i = box_with_id(t, q.doc->document, "i");
    const flow_box_t *n = box_with_id(t, q.doc->document, "n");
    expect("blend: blended and isolated blocks are stacked, a plain one is not",
           b != NULL && i != NULL && n != NULL && b->stacked && i->stacked && !n->stacked, NULL);
    const os64_html_node_t *root = q.doc->document;
    flow_computed_t *c = t != NULL ? flow_computed_new(t, &q.env, computed_find(root, "b"), NULL) : NULL;
    computed_is(c, "computed: mix-blend-mode", GARB_MIX_BLEND_MODE, "difference");
    computed_is(c, "computed: isolation auto", GARB_ISOLATION, "auto");
    flow_computed_free(c);
    c = t != NULL ? flow_computed_new(t, &q.env, computed_find(root, "i"), NULL) : NULL;
    computed_is(c, "computed: isolation isolate", GARB_ISOLATION, "isolate");
    computed_is(c, "computed: mix-blend-mode normal", GARB_MIX_BLEND_MODE, "normal");
    flow_computed_free(c);
    flow_free(t);
    sheeted_close(&q);
}

static void filter_cases(void)
{
    filter_group_cases();
    filter_layout_cases();
    filter_root_cases();
    filter_computed_cases();
    filter_mark_cases();
    filter_seen_cases();
    blend_mode_cases();
    filter_backdrop_damage_cases();
}
