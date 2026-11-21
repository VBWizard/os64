// The walk told a cover (flow_visit_groups_from; VIDEO.md § 5, The paint
// beneath the picture): it hands over exactly what the ordinary walk hands
// over from the cover's own visit on, and before it only the opens of the
// groups open at that visit, which flow_paint_path names. Held for every
// box a page's ordinary walk hands over, as the cover in turn.

typedef enum { CEV_VISIT, CEV_OPEN, CEV_CLOSE, CEV_OUTLINE } CoverEventKind;

typedef struct {
    CoverEventKind kind;
    const flow_box_t *box;
} CoverEvent;

typedef struct {
    CoverEvent ev[4096];
    int32_t n;
    bool overflow;
} CoverLog;

static void clog_put(CoverLog *l, CoverEventKind kind, const flow_box_t *b)
{
    if (l->n == (int32_t)(sizeof(l->ev) / sizeof(l->ev[0]))) {
        l->overflow = true;
        return;
    }
    l->ev[l->n++] = (CoverEvent){kind, b};
}

static void clog_visit(void *ctx, const flow_box_t *b) { clog_put(ctx, CEV_VISIT, b); }
static void clog_open(void *ctx, const flow_box_t *b, os64_gui_rect_t bounds)
{
    (void)bounds;
    clog_put(ctx, CEV_OPEN, b);
}
static void clog_close(void *ctx, const flow_box_t *b) { clog_put(ctx, CEV_CLOSE, b); }
static void clog_outline(void *ctx, const flow_box_t *b) { clog_put(ctx, CEV_OUTLINE, b); }

static void cover_walk(const flow_tree_t *t, os64_gui_rect_t view, const flow_box_t *from, CoverLog *log,
                       bool *found)
{
    log->n = 0;
    log->overflow = false;
    flow_visitor_t v = {clog_visit, clog_open, clog_close, log, clog_outline, NULL};
    if (from == NULL)
        flow_visit_groups(t, view, (flow_point_t){0, 0}, &v);
    else
        flow_visit_groups_from(t, view, (flow_point_t){0, 0}, &v, from, found);
}

// Every box the ordinary walk of `t` over `view` hands over, as the cover in
// turn: the property above, and flow_paint_path's groups. `what` names the
// page in a failure.
static void cover_page_holds(const char *what, const flow_tree_t *t, os64_gui_rect_t view)
{
    static CoverLog plain, covered;
    cover_walk(t, view, NULL, &plain, NULL);
    int checked = 0, wrong = 0, path_wrong = 0, not_found = 0;
    char first[160] = "";
    for (int32_t at = 0; at < plain.n; at++) {
        if (plain.ev[at].kind != CEV_VISIT)
            continue;
        const flow_box_t *cover = plain.ev[at].box;
        // A box handed over twice (an inline's pieces) is a cover at its
        // first visit only.
        bool earlier = false;
        for (int32_t k = 0; k < at && !earlier; k++)
            earlier = plain.ev[k].kind == CEV_VISIT && plain.ev[k].box == cover;
        if (earlier)
            continue;
        // The groups open at the cover's visit, outermost first.
        const flow_box_t *open[64];
        int32_t nopen = 0;
        for (int32_t k = 0; k < at; k++) {
            if (plain.ev[k].kind == CEV_OPEN && nopen < 64)
                open[nopen++] = plain.ev[k].box;
            else if (plain.ev[k].kind == CEV_CLOSE && nopen > 0)
                nopen--;
        }
        bool found = false;
        cover_walk(t, view, cover, &covered, &found);
        checked++;
        bool same = !covered.overflow && covered.n == nopen + (plain.n - at);
        for (int32_t k = 0; same && k < nopen; k++)
            same = covered.ev[k].kind == CEV_OPEN && covered.ev[k].box == open[k];
        for (int32_t k = 0; same && k < plain.n - at; k++)
            same = covered.ev[nopen + k].kind == plain.ev[at + k].kind && covered.ev[nopen + k].box == plain.ev[at + k].box;
        if (!found)
            not_found++;
        if (!same) {
            wrong++;
            if (first[0] == '\0')
                snprintf(first, sizeof(first), "%s: cover at event %d of %d (kind %d), %d groups open, %d events",
                         what, (int)at, (int)plain.n, (int)cover->kind, (int)nopen, (int)covered.n);
        }
        const flow_box_t *path[64];
        int32_t npath = flow_paint_path(t, cover, path, 64);
        bool path_same = npath == nopen;
        for (int32_t k = 0; path_same && k < npath; k++)
            path_same = path[k] == open[k];
        if (!path_same)
            path_wrong++;
    }
    char said[400];
    snprintf(said, sizeof(said), "%s: %d covers, %d wrong (%s), %d paths wrong, %d not found", what, checked, wrong,
             first[0] ? first : "-", path_wrong, not_found);
    expect("cover: the walk told a cover hands over the ordinary walk from the cover on, and before it only the "
           "opens of the groups that hold it",
           checked > 0 && wrong == 0 && !plain.overflow, said);
    expect("cover: flow_paint_path names the groups open at the cover's visit", checked > 0 && path_wrong == 0, said);
    expect("cover: the walk says it found the cover", checked > 0 && not_found == 0, said);
}

static void cover_cases(void)
{
    // A page of the shapes the rule must hold through: groups nested and
    // side by side, positive and negative z-indices, a float, an atom, an
    // outline, a scroller, and an absolutely positioned box whose box
    // ancestor groups and clips but whose containing block is outside it —
    // painted in the root's context, not inside the ancestor's group.
    static const char *const pages[] = {
        "<!doctype html><style>body{margin:0} .g{opacity:.5} .z{position:relative;z-index:2}"
        ".n{position:relative;z-index:-1} .f{float:left;width:30px;height:20px}"
        ".o{outline:2px solid red} .s{overflow:auto;height:40px}</style>"
        "<div class=g><p>one</p><div class=g><p>two</p><span class=o>three</span></div></div>"
        "<div class=n>under</div><div class=z>over <b class=g>bold</b></div>"
        "<div class=f>float</div><span style='display:inline-block' class=g>atom</span>"
        "<div class=s><p>a</p><p>b</p><p>c</p><p>d</p></div><p>tail</p>",
        "<!doctype html><style>body{margin:0}</style>"
        "<div style='opacity:.5;overflow:hidden;height:20px'>"
        "<img id=v style='position:absolute;top:30px;left:0;width:40px;height:30px'>"
        "<p>in the group</p></div><p>after</p>"
        "<div style='position:relative;z-index:1;opacity:.8'><div style='position:absolute;z-index:-1'>back</div>"
        "front</div>",
        "<!doctype html><style>body{margin:0} div{width:100px;height:40px}</style>"
        "<div style='position:relative;z-index:1;background:red'>a</div>"
        "<div style='position:relative;z-index:1;margin-top:-20px;background:blue;opacity:.5'>b</div>"
        "<div style='transform:translateX(10px);opacity:.9'>c <i style='filter:blur(2px)'>d</i></div>",
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        Sheeted p;
        sheeted_open(&p, pages[i], 400, 600);
        flow_tree_t *t = flow_layout(p.doc, p.page, 400, &p.env);
        char what[32];
        snprintf(what, sizeof(what), "page %d", (int)i);
        cover_page_holds(what, t, (os64_gui_rect_t){0, 0, 400, 600});
        // A part of the view as well, as a paint of a small R walks.
        snprintf(what, sizeof(what), "page %d, a part", (int)i);
        cover_page_holds(what, t, (os64_gui_rect_t){5, 10, 120, 35});
        flow_free(t);
        sheeted_close(&p);
    }
    // A box no part paints, and none at all.
    Sheeted p;
    sheeted_open(&p, "<!doctype html><p>x</p>", 400, 600);
    flow_tree_t *t = flow_layout(p.doc, p.page, 400, &p.env);
    flow_box_t stray = {0};
    bool found = true;
    CoverLog *log = calloc(1, sizeof(*log));
    cover_walk(t, (os64_gui_rect_t){0, 0, 400, 600}, &stray, log, &found);
    expect("cover: a box no part paints has no path, and the walk says it was not found",
           flow_paint_path(t, &stray, NULL, 0) == -1 && !found, NULL);
    expect("cover: no tree, no path", flow_paint_path(NULL, flow_root(t), NULL, 0) == -1, NULL);
    free(log);
    flow_free(t);
    sheeted_close(&p);
}
