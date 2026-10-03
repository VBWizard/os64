// The blend (PILE3.md § The blend): colours keep their alpha, and a box
// below full opacity is a stacking context — stacked even in the flow — and
// a GROUP the walk brackets for a face to composite whole.

typedef struct {
    char log[512];
    size_t n;
    os64_gui_rect_t bounds[4];
    int32_t opens;
} GroupLog;

static void glog(GroupLog *g, const char *what)
{
    size_t len = strlen(what);
    if (g->n + len + 1 < sizeof(g->log)) {
        memcpy(g->log + g->n, what, len);
        g->n += len;
        g->log[g->n++] = ' ';
        g->log[g->n] = '\0';
    }
}

static void glog_visit(void *ctx, const flow_box_t *b)
{
    GroupLog *g = ctx;
    if (b->kind == FLOW_BOX_TEXT)
        glog(g, b->text[0] == 'a' ? "a" : b->text[0] == 'b' ? "b" : "t");
}

static void glog_open(void *ctx, const flow_box_t *b, os64_gui_rect_t bounds)
{
    (void)b;
    GroupLog *g = ctx;
    if (g->opens < 4)
        g->bounds[g->opens] = bounds;
    g->opens++;
    glog(g, "(");
}

static void glog_close(void *ctx, const flow_box_t *b)
{
    (void)b;
    glog(ctx, ")");
}

static void blend_cases(void)
{
    flow_point_t top = {0, 0};
    // A block below full opacity is stacked and not positioned: it stays
    // in its parent's overflow and is painted, as one group, after the
    // paragraph written after it — which it overlaps by a negative margin,
    // so a point on both is the group's.
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     ".h{opacity:0.5;height:20px} .up{margin-top:-20px}</style>"
                     "<div id=h class=h>a</div><p class=up>b</p>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *h = box_with_id(t, q.doc->document, "h");
    GroupLog g = {{0}, 0, {{0}}, 0};
    flow_visitor_t v = {glog_visit, glog_open, glog_close, &g};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &v);
    expect("blend: a block below full opacity is stacked, not positioned, and one group",
           h != NULL && h->stacked && !h->positioned && flow_npositioned(t) == 0 &&
               strcmp(g.log, "b ( a ) ") == 0 && g.opens == 1 && g.bounds[0].y == 0 &&
               g.bounds[0].h == 20 && dump_has(t, "block div 0 0 400 20 stacked"), g.log);
    const flow_box_t *hit = t != NULL ? flow_hit(t, 2, 5, top) : NULL;
    expect("blend: what is painted over the flow is hit over it", inside_of(hit, h), NULL);
    flow_free(t);
    sheeted_close(&q);

    // A positioned group's bounds hold its positioned members, wherever
    // they are: the absolute box 50 below reaches 60. Inside it a nested
    // group opens and closes within it. An inline-block below full opacity
    // is not stacked: its content is painted where its atom is.
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#r{position:relative;opacity:0.5;height:10px}"
                     ".abs{position:absolute;top:50px;width:10px;height:10px}"
                     ".in{opacity:0.25;height:5px} #ib{display:inline-block;opacity:0.5}"
                     "</style><div id=r><div class=in></div><div class=abs></div></div>"
                     "<span id=ib>t</span>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *ib = box_with_id(t, q.doc->document, "ib");
    GroupLog n = {{0}, 0, {{0}}, 0};
    flow_visitor_t nv = {glog_visit, glog_open, glog_close, &n};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &nv);
    expect("blend: a group holds its positioned members, and groups nest",
           n.opens == 2 && strncmp(n.log, "t ( ( ) ) ", 10) == 0 && n.bounds[0].y == 0 &&
               n.bounds[0].h == 60 && n.bounds[1].h == 5, n.log);
    expect("blend: an inline-block below full opacity is not stacked",
           ib != NULL && ib->first != NULL && !ib->first->stacked, NULL);
    flow_free(t);
    sheeted_close(&q);

    // opacity: 0 paints nothing and opens no group; the page's own
    // flow_visit hears no groups at all.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} .o{opacity:0} .h{opacity:0.5}"
                     "</style><div class=o>a</div><div class=h>b</div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    GroupLog z = {{0}, 0, {{0}}, 0};
    flow_visitor_t zv = {glog_visit, glog_open, glog_close, &z};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &zv);
    expect("blend: opacity 0 opens no group and paints nothing",
           strcmp(z.log, "( b ) ") == 0, z.log);
    flow_free(t);
    sheeted_close(&q);
}

// Corners (PILE3.md § The box's edge): a radius resolves against the border
// box, and radii that would overlap along a side are all scaled down
// together until they meet (Backgrounds 3 § 5.5).
static void radius_cases(void)
{
    struct { const char *css; int32_t h, v; } k[] = {
        {"border-radius:50%", 50, 25},              // 100 x 50: half of each
        {"border-radius:40px", 25, 25},             // 80 down a 50 side: 5/8 of 40
        {"border-radius:10px / 20px", 10, 20},      // fits: as written
        {"border-top-left-radius:0 30px", 0, 30},   // kept; a face draws it square
        // Scaled from the radii as written, never capped first (Quinn,
        // #203; Firefox): one 100px corner of a 50px-high box is 50 x 50,
        // not 100 x 50; 100px and 10px corners stay circular at 50/110.
        {"border-top-left-radius:100px", 50, 50},
        {"border-radius:100px 10px", 45, 45},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body{margin:0} div{width:100px;height:50px;%s}</style>"
                 "<div id=d></div>",
                 k[i].css);
        Sheeted q;
        sheeted_open(&q, html, 400, 600);
        flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *d = box_with_id(t, q.doc->document, "d");
        int32_t r[4][2] = {{0}};
        flow_box_radii(d, r);
        char got[64];
        snprintf(got, sizeof(got), "%s: %d %d", k[i].css, r[0][0], r[0][1]);
        expect("radius: a corner resolved and held to its sides",
               d != NULL && r[0][0] == k[i].h && r[0][1] == k[i].v, got);
        flow_free(t);
        sheeted_close(&q);
    }
}
