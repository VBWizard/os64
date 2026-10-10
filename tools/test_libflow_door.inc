// The door (LAYOUT.md § The one door) and the gauntlet (§ Proof): hit
// testing, lookups, the painting walk, the lists, and the three proofs a
// page cannot break it — an allocation sweep with the partial tree's
// relation to the whole one, fuzzing, and the corpus dumps.

typedef struct {
    const flow_box_t *v[64];
    int n;
} Seen;

static void seen_add(void *ctx, const flow_box_t *b)
{
    Seen *s = ctx;
    if (s->n < 64)
        s->v[s->n++] = b;
}

static const char *kind_name(flow_box_kind_t k)
{
    static const char *const names[] = {"block", "replaced", "table", "caption", "column-group",
        "column", "row-group", "row", "cell", "line", "span", "text", "atomic", "marker"};
    return names[k];
}

// "go " 20, "here" 28, " " 4, a 40x30 picture, " ", an unmeasured control
// (ten ems: 160, one 20px line), " ", and a missing picture laid out as
// its alt text (alt 16 + 4 + text 24). On one 36px line — the picture's 30
// above the baseline, the text's 6 below — whose baseline is at 46.
static const char *kDoorPage =
    "<!doctype html><p>go <a href=x>here</a> <img src=known.png alt=pic> "
    "<input name=q> <img src=missing.png alt=\"alt text\">";

static void door_cases(void)
{
    os64_html_document_t *doc = parse(kDoorPage);
    os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
    flow_tree_t *t = flow_layout(doc, page, 400, &kEnv);
    expect("door: laid out", t != NULL && !flow_incomplete(t), NULL);
    if (t == NULL)
        return;

    const flow_box_t *h = flow_hit(t, 30, 40, kNoScroll);
    expect("door: a click on a link's text is that text, in that link",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 4 &&
           memcmp(h->text, "here", 4) == 0 && h->link == 0, NULL);
    h = flow_hit(t, 70, 20, kNoScroll);
    expect("door: a click on a picture is its atom",
           h != NULL && h->kind == FLOW_BOX_ATOMIC && h->rect.x == 60 && h->rect.y == 16 &&
           h->rect.w == 40 && h->rect.h == 30, NULL);
    h = flow_hit(t, 390, 20, kNoScroll);
    expect("door: a click past the line's content is the line",
           h != NULL && h->kind == FLOW_BOX_LINE, NULL);
    expect("door: a click off the page is nothing", flow_hit(t, 500, 20, kNoScroll) == NULL, NULL);

    const os64_html_node_t *p = doc->body->first_child;
    const os64_html_node_t *a = p->first_child->next;
    const flow_box_t *pb = flow_box_for(t, p), *ab = flow_box_for(t, a);
    expect("door: box_for a paragraph is its block",
           pb != NULL && pb->kind == FLOW_BOX_BLOCK && pb->node == p && pb->rect.y == 16, NULL);
    expect("door: box_for a link is its first piece",
           ab != NULL && ab->kind == FLOW_BOX_SPAN && ab->link == 0 && ab->rect.x == 28, NULL);
    expect("door: box_for a node with no box is NULL",
           flow_box_for(t, doc->head) == NULL, NULL);

    expect("door: the pictures, each once, the missing one by its alt text",
           flow_nimages(t) == 2 && flow_image(t, 0)->kind == FLOW_BOX_ATOMIC &&
           flow_image(t, 1)->kind == FLOW_BOX_TEXT && flow_image(t, 1)->length == 8 &&
           flow_image(t, 2) == NULL, NULL);
    expect("door: the controls",
           flow_ncontrols(t) == 1 && flow_control(t, 0)->control == 0 &&
           flow_control(t, 0)->rect.x == 104 && flow_control(t, 0)->rect.w == 160 &&
           flow_control(t, 0)->rect.h == 20, NULL);

    Seen all = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 1000}, kNoScroll, seen_add, &all);
    char order[512] = "";
    for (int i = 0; i < all.n; i++) {
        strcat(order, kind_name(all.v[i]->kind));
        strcat(order, " ");
    }
    expect("door: painting order — blocks, then spans, then content",
           strcmp(order, "block block block span text text text atomic text atomic text text ")
               == 0, order);
    Seen some = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){250, 0, 20, 100}, kNoScroll, seen_add, &some);
    order[0] = '\0';
    for (int i = 0; i < some.n; i++) {
        strcat(order, kind_name(some.v[i]->kind));
        strcat(order, " ");
    }
    expect("door: the walk prunes what the viewport does not meet",
           strcmp(order, "block block block atomic text text ") == 0, order);
    Seen none = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){0, 100, 400, 50}, kNoScroll, seen_add, &none);
    expect("door: below the page, nothing", none.n == 0, NULL);
    flow_free(t);

    // A word wider than its line: the paragraph's OVERFLOW holds it, so
    // the walk and the hit test still reach it past the paragraph's edge.
    os64_html_document_t *doc2 = parse("<!doctype html><p>abcdefghij");
    os64_page_t *page2 = os64_page_build(doc2, kPage, NULL, NULL);
    t = flow_layout(doc2, page2, 50, &kEnv);
    const flow_box_t *root = flow_root(t);
    expect("door: overflow reaches past the box",
           root != NULL && root->rect.w == 50 && root->overflow.w == 76 &&
           flow_width(t) == 76, NULL);
    h = flow_hit(t, 70, 20, kNoScroll);
    expect("door: a click on the overflowing word finds it",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 10, NULL);
    flow_free(t);
    os64_page_free(page2);
    os64_html_document_free(doc2);

    // A table wider than the page hangs out of the html and body boxes: a
    // viewport that meets only the part hanging out still reaches it.
    doc2 = parse("<!doctype html><table width=200><tr><td>x</table>");
    page2 = os64_page_build(doc2, kPage, NULL, NULL);
    t = flow_layout(doc2, page2, 50, &kEnv);
    Seen out = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){100, 0, 10, 100}, kNoScroll, seen_add, &out);
    bool table_seen = false;
    for (int i = 0; i < out.n; i++)
        table_seen |= out.v[i]->kind == FLOW_BOX_TABLE;
    expect("door: the walk reaches a table hanging out of its ancestors", table_seen, NULL);
    flow_free(t);
    os64_page_free(page2);
    os64_html_document_free(doc2);
    os64_page_free(page);
    os64_html_document_free(doc);
}

// flow_replaced_fixed is the layout's own rule: for every way a page can
// spell a picture's width and height, it answers yes exactly when the box
// comes out the same whether the picture has arrived (known.png, 40x30) or
// not (missing.png). A face that trusts it to skip a layout on arrival is
// then never wrong in the direction that leaves a picture squashed.
static void fixed_cases(void)
{
    static const char *const sides[] = {"", "width=50", "width=auto", "width=abc", "width=\"\"",
                                        "width=50%", "width=\"50 %\""};
    static const char *const heights[] = {"", "height=60", "height=auto", "height=\"\"",
                                          "height=60%", "height=\" 60\""};
    for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++)
        for (size_t k = 0; k < sizeof(heights) / sizeof(heights[0]); k++) {
            os64_gui_rect_t rect[2];
            bool fixed = false, laid = true;
            for (int arrived = 0; arrived < 2; arrived++) {
                char html[256];
                snprintf(html, sizeof(html), "<!doctype html><p><img src=%s %s %s alt=pic>",
                         arrived ? "known.png" : "missing.png", sides[i], heights[k]);
                os64_html_document_t *doc = parse(html);
                os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
                flow_tree_t *t = flow_layout(doc, page, 400, &kEnv);
                const os64_html_node_t *img = doc->body->first_child->first_child;
                const flow_box_t *b = t != NULL ? flow_box_for(t, img) : NULL;
                laid &= b != NULL;
                if (b != NULL) {
                    rect[arrived] = b->rect;
                    // The style is the element's either way: the two answers agree.
                    if (arrived)
                        laid &= fixed == flow_replaced_fixed(b->style);
                    fixed = flow_replaced_fixed(b->style);
                }
                flow_free(t);
                os64_page_free(page);
                os64_html_document_free(doc);
            }
            bool same = laid && rect[0].x == rect[1].x && rect[0].y == rect[1].y &&
                        rect[0].w == rect[1].w && rect[0].h == rect[1].h;
            char what[160];
            snprintf(what, sizeof(what), "fixed: <img %s %s> is fixed exactly when arrival moves nothing",
                     sides[i], heights[k]);
            expect(what, laid && fixed == same, NULL);
        }
    expect("fixed: no style is not fixed", !flow_replaced_fixed(NULL), NULL);
}

// ── The allocation sweep: rules (a) to (d) of LAYOUT.md § Proof ─────────

typedef struct {
    const flow_box_t **v;
    int32_t *parent;
    size_t n, cap;
} Flat;

// The IN-FLOW tree in pre-order (POSITION.md § Proof): an out-of-flow box
// is all or nothing, laid out once its containing block is finished, so a
// partial tree can hold boxes after where one of those would be; the
// relations are asserted without them, and a placed one is held only to
// being where it is. So is a float (FLOATS.md § Proof): one met on a line
// hangs under its context after all of the context's lines, but is laid
// out as its line is built, so a tree cut in a later line holds it where
// the whole one holds that line.
static bool out_of_flow_box(const flow_box_t *b)
{
    return (b->positioned && b->style->position == FLOW_POSITION_ABSOLUTE) ||
           b->floated != FLOW_FLOAT_NONE;
}

static void flatten(Flat *f, const flow_box_t *b, int32_t parent)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 256;
        f->v = realloc(f->v, f->cap * sizeof(*f->v));
        f->parent = realloc(f->parent, f->cap * sizeof(*f->parent));
    }
    int32_t me = (int32_t)f->n++;
    f->v[me] = b;
    f->parent[me] = parent;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (!out_of_flow_box(c))
            flatten(f, c, me);
}

static bool same_item(const flow_box_t *a, const flow_box_t *b)
{
    return a->kind == b->kind && a->node == b->node && a->begin == b->begin &&
           (a->unfinished || b->unfinished || a->length == b->length);
}

static bool has_unfinished_below(const Flat *f, size_t i)
{
    for (size_t k = i + 1; k < f->n; k++) {
        int32_t up = f->parent[k];
        while (up >= 0 && (size_t)up > i)
            up = f->parent[up];
        if (up != (int32_t)i)
            break;
        if (f->v[k]->unfinished)
            return true;
    }
    return false;
}

static const char kSweepPage[] =
    "<!doctype html><h1>T</h1><p>one <b>two</b> three<br>four <img src=known.png>"
    "<ul><li>a<li>b</ul><table border=1><tr><td>c1<td>c2</table><pre>x\ty</pre>"
    "<font face=arial><p>in</p>out</font><p>last words here</p>"
    "<table><caption>c</caption><tr><td rowspan=2>r<td>x<tr><td>y</table>"
    "<marquee>m <b>q</b></marquee><ol><li><p>para</p><li>item</ol><q>quoted</q>";

// Every kind of positioned box, and every kind of containing block, none
// clipped (a clipped box reaches past the page on purpose): relative ones
// nested, an absolute one under an inline, one among blocks, one against
// the viewport, one in a table cell, in an inline-block, and the dialog.
static const char kPositionedSweepPage[] =
    "<!doctype html><style>.r{position:relative;left:3px;top:2px}"
    ".a{position:absolute;right:0;top:0} .b{position:absolute;bottom:0;left:10%;width:50%}"
    "</style><h1 class=r>T<span class=a>x</span></h1><p>one <a href=x class=r>two"
    "<b class=a>badge</b></a> three</p><div class=a>among blocks</div>"
    "<div class=r><div class=r>nested <i class=b>deep</i></div></div>"
    "<table><tr><td class=r>c<span class=a>in cell</span><td>d</table>"
    "<p>x<span class=r style='display:inline-block'>ib<span class=a>in</span></span></p>"
    "<div class=b>viewport</div><dialog open>modal <b>text</b></dialog><p>last words</p>"
    "<div style='position:fixed;top:0;right:0;width:40%'>held <i class=a>on it</i></div>";

static bool unfinished_within(const flow_box_t *b)
{
    if (b->unfinished)
        return true;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        if (unfinished_within(c))
            return true;
    return false;
}

// Relation (b) for the out-of-flow boxes and floats the in-flow walk left
// out: one placed whole, under no unfinished table or line, is where the
// whole layout puts it — laid out once its containing block was finished,
// or placed once the margins above it resolved, it does not move after.
// A box whose children's geometry comes from the children it has: a
// table's columns from its cells, a line's from its pieces, a flex or grid
// container's items from each other. Unfinished, what it holds may be
// placed differently from the whole layout, and is not compared.
static bool sized_by_its_children(const flow_box_t *b)
{
    flow_display_t d = b->style != NULL ? b->style->display : FLOW_DISPLAY_BLOCK;
    return b->kind == FLOW_BOX_TABLE || b->kind == FLOW_BOX_LINE ||
           (b->kind == FLOW_BOX_BLOCK && (d == FLOW_DISPLAY_FLEX || d == FLOW_DISPLAY_INLINE_FLEX ||
                                          d == FLOW_DISPLAY_GRID || d == FLOW_DISPLAY_INLINE_GRID));
}

static void placed_out_of_flow_stay(const flow_box_t *b, bool exempt, const flow_tree_t *whole,
                                    bool *same, size_t *compared, char *detail, size_t cap)
{
    if (out_of_flow_box(b) && !exempt && !unfinished_within(b)) {
        const flow_box_t *twin = flow_box_for(whole, b->node);
        bool here = twin != NULL && twin->rect.x == b->rect.x && twin->rect.y == b->rect.y &&
                    twin->rect.w == b->rect.w && twin->rect.h == b->rect.h;
        if (!here && *same)
            snprintf(detail, cap, "%s at %d %d %d %d, whole at %d %d %d %d",
                     b->node != NULL && b->node->name != NULL ? b->node->name : "-",
                     b->rect.x, b->rect.y, b->rect.w, b->rect.h,
                     twin != NULL ? twin->rect.x : -1, twin != NULL ? twin->rect.y : -1,
                     twin != NULL ? twin->rect.w : -1, twin != NULL ? twin->rect.h : -1);
        *same &= here;
        (*compared)++;
    }
    exempt |= b->unfinished && sized_by_its_children(b);
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        placed_out_of_flow_stay(c, exempt, whole, same, compared, detail, cap);
}

// How far the whole tree reaches, out-of-flow boxes and all.
static void reach_of(const flow_box_t *b, int64_t *x, int64_t *y)
{
    const os64_gui_rect_t *r = &b->rect;
    *y = r->y + r->h > *y ? r->y + r->h : *y;
    *x = r->x + r->w > *x ? r->x + r->w : *x;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        reach_of(c, x, y);
}

// Beside (a) to (d), a partial page is as tall as the blocks it holds, so
// what did lay out can be scrolled to.
static void layout_relation_sweep(const char *what, const char *html, int32_t width)
{
    // With the page's own sheets, cascaded before the sweep counts: the
    // allocations failed are the layout's.
    Sheeted sh;
    sheeted_open(&sh, html, width, 600);
    const flow_env_t *env = &sh.env;
    os64_html_document_t *doc = sh.doc;
    os64_page_t *page = sh.page;
    flow_tree_t *whole = flow_layout(doc, page, width, env);
    Flat w = {0};
    flatten(&w, flow_root(whole), -1);
    size_t base_live = live;
    allocations = 0;
    flow_tree_t *probe = flow_layout(doc, page, width, env);
    size_t count = allocations;
    flow_free(probe);
    size_t partial = 0, nulls = 0, compared = 0;
    fail_single = true;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        flow_tree_t *t = flow_layout(doc, page, width, env);
        fail_at = 0;
        if (t == NULL) {
            nulls++;
        } else if (flow_incomplete(t) && flow_root(t) == NULL) {
            partial++;      // nothing built at all: the empty prefix
        } else if (flow_incomplete(t)) {
            partial++;
            Flat p = {0};
            flatten(&p, flow_root(t), -1);
            char name[96];
            snprintf(name, sizeof(name), "sweep %zu: (a) a prefix of the whole", at);
            bool prefix = p.n <= w.n;
            size_t i = 0;
            for (; prefix && i < p.n; i++)
                prefix = same_item(p.v[i], w.v[i]);
            char detail[160] = "";
            if (!prefix && p.n <= w.n) {
                i--;
                snprintf(detail, sizeof(detail), "box %zu: %s %s +%u/%u%s, whole has %s %s +%u/%u",
                         i, kind_name(p.v[i]->kind),
                         p.v[i]->node != NULL && p.v[i]->node->name != NULL ? p.v[i]->node->name : "-",
                         p.v[i]->begin, p.v[i]->length, p.v[i]->unfinished ? " unfinished" : "",
                         kind_name(w.v[i]->kind),
                         w.v[i]->node != NULL && w.v[i]->node->name != NULL ? w.v[i]->node->name : "-",
                         w.v[i]->begin, w.v[i]->length);
            }
            expect(name, prefix, detail);
            snprintf(name, sizeof(name), "sweep %zu: (b) what finished is where it will be", at);
            bool same = true;
            for (size_t i = 0; prefix && i < p.n; i++) {
                if (p.v[i]->unfinished || has_unfinished_below(&p, i))
                    continue;
                // Under an unfinished box sized by its children, geometry is
                // not compared (sized_by_its_children).
                bool exempt = false;
                for (int32_t up = p.parent[i]; up >= 0 && !exempt; up = p.parent[up])
                    exempt = p.v[up]->unfinished && sized_by_its_children(p.v[up]);
                if (exempt)
                    continue;
                const os64_gui_rect_t *x = &p.v[i]->rect, *y = &w.v[i]->rect;
                if (x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h)
                    same = false;
                compared++;
            }
            expect(name, same, NULL);
            snprintf(name, sizeof(name), "sweep %zu: (b) a placed out-of-flow box stays", at);
            bool stays = true;
            detail[0] = '\0';
            placed_out_of_flow_stay(flow_root(t), false, whole, &stays, &compared, detail,
                                    sizeof(detail));
            expect(name, stays, detail);
            snprintf(name, sizeof(name), "sweep %zu: (c) the unfinished lead to the last", at);
            bool path = true;
            for (size_t i = 0; i < p.n; i++) {
                if (!p.v[i]->unfinished)
                    continue;
                int32_t up = (int32_t)p.n - 1;
                while (up >= 0 && (size_t)up != i)
                    up = p.parent[up];
                path &= up == (int32_t)i;
            }
            expect(name, path, NULL);
            snprintf(name, sizeof(name), "sweep %zu: the page is as tall as what it holds", at);
            int64_t bottom = 0;
            // A fixed box is where it is on the glass, and the page is never
            // scrolled to reach it.
            for (size_t i = 0; i < p.n; i++)
                if (p.v[i]->kind == FLOW_BOX_BLOCK && !p.v[i]->fixed &&
                    p.v[i]->rect.y + p.v[i]->rect.h > bottom)
                    bottom = p.v[i]->rect.y + p.v[i]->rect.h;
            snprintf(detail, sizeof(detail), "height %d, a block reaches %lld, root %d+%d%s",
                     flow_height(t), (long long)bottom, p.v[0]->rect.y, p.v[0]->rect.h,
                     p.v[0]->unfinished ? " unfinished" : "");
            expect(name, flow_height(t) >= bottom, detail);
            // (d) and no taller or wider than what it holds: nothing that is
            // not placed reaches the page's edges. In pixels, give or take
            // one: a rectangle's edges round to the nearest pixel and the
            // page's size rounds up, so the canvas holds all of what it has.
            snprintf(name, sizeof(name), "sweep %zu: (d) nothing unplaced reaches an edge", at);
            int64_t reach_y = 0, reach_x = width;
            reach_of(flow_root(t), &reach_x, &reach_y);
            snprintf(detail, sizeof(detail), "page %d x %d, holds %lld x %lld", flow_width(t),
                     flow_height(t), (long long)reach_x, (long long)reach_y);
            expect(name, flow_height(t) <= reach_y + 1 && flow_width(t) <= reach_x + 1, detail);
            free(p.v);
            free(p.parent);
        } else {
            int64_t n1 = flow_dump(t, NULL, 0), n2 = flow_dump(whole, NULL, 0);
            char name[96];
            snprintf(name, sizeof(name), "sweep %zu: a layout that says it is whole is whole", at);
            expect(name, n1 == n2, NULL);
        }
        flow_free(t);
        if (live != base_live) {
            expect("sweep: nothing leaked", false, NULL);
            break;
        }
    }
    fail_single = false;
    printf("libflow relation sweep, %s: %zu failures: %zu NULL, %zu partial trees held to (a)-(d), "
           "%zu finished boxes compared\n", what, count, nulls, partial, compared);
    free(w.v);
    free(w.parent);
    flow_free(whole);
    sheeted_close(&sh);
}

// ── Fuzz ────────────────────────────────────────────────────────────────

static uint64_t s_rng = 0x0F10D5EED1234567ull;

static uint64_t rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static size_t fuzz_trees;

// The invariants of any laid-out tree, whatever the page did. SIZES are
// never negative; origins may be, anywhere (a negative margin, `left:
// -9999px`). CONTAINMENT as the door states it: what of every in-flow
// child is DRAWN — its overflow rect met with its clip, since a box that
// clips reaches no further than its border box on that axis — inside its
// parent's overflow rect; and every positioned box's subtree inside its
// own, a positioned box being reached from the list and not through its
// parent, so checked as a root of its own. Unscrolled, a scroll
// container's content stands where the container's frame does, so what
// clips that frame from outside — the container's padding box — clips it.
static os64_gui_rect_t met_with(os64_gui_rect_t drawn, os64_gui_rect_t clip)
{
    int64_t x0 = drawn.x > clip.x ? drawn.x : clip.x;
    int64_t y0 = drawn.y > clip.y ? drawn.y : clip.y;
    int64_t x1 = (int64_t)drawn.x + drawn.w, y1 = (int64_t)drawn.y + drawn.h;
    x1 = x1 < (int64_t)clip.x + clip.w ? x1 : (int64_t)clip.x + clip.w;
    y1 = y1 < (int64_t)clip.y + clip.h ? y1 : (int64_t)clip.y + clip.h;
    return (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 > x0 ? x1 - x0 : 0),
                             (int32_t)(y1 > y0 ? y1 - y0 : 0)};
}

static void check_tree(const char *what, const flow_box_t *b, const flow_box_t *parent)
{
    bool ok = b->rect.w >= 0 && b->rect.h >= 0 && b->overflow.w >= 0 && b->overflow.h >= 0;
    os64_gui_rect_t drawn = b->overflow;
    if (b->clipped) {
        drawn = met_with(drawn, b->clip);
        for (const flow_frame_t *f = b->frame; f != NULL && f->kind == F_FRAME_SCROLL;
             f = f->outer)
            if (f->clips)
                drawn = met_with(drawn, f->clip);
    }
    if (parent != NULL && drawn.w > 0 && drawn.h > 0) {
        const os64_gui_rect_t *o = &drawn, *po = &parent->overflow;
        ok &= o->x >= po->x && o->y >= po->y && (int64_t)o->x + o->w <= (int64_t)po->x + po->w &&
              (int64_t)o->y + o->h <= (int64_t)po->y + po->h;
    }
    if (b->kind == FLOW_BOX_TEXT && b->node != NULL && b->node->kind == OS64_HTML_TEXT)
        ok &= b->begin + b->length <= b->node->text_len;
    if (!ok) {
        char detail[128];
        snprintf(detail, sizeof(detail), "%s box %s at %d,%d %dx%d", what, kind_name(b->kind),
                 b->rect.x, b->rect.y, b->rect.w, b->rect.h);
        expect("fuzz: a tree's invariants", false, detail);
        return;
    }
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        check_tree(what, c, c->positioned ? NULL : b);
}

// The positioned list is every positioned box, each once: counted in the
// tree, and each found in the list exactly once.
static void positioned_in_order(const flow_box_t *b, const flow_tree_t *t, int32_t *at, bool *ok)
{
    if (b->positioned) {
        int32_t seen = 0;
        for (int32_t i = 0; i < flow_npositioned(t); i++)
            seen += flow_positioned(t, i) == b;
        *ok &= seen == 1;
        (*at)++;
    }
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        positioned_in_order(c, t, at, ok);
}

// Every sticky box, at any scroll, is moved from the frame it is in by no
// more than its containing block leaves room for, on either axis — and a
// room always holds 0, where the flow put it.
static void sticky_held(const flow_box_t *b, bool *ok)
{
    const flow_frame_t *s = b->frame;
    if (s != NULL && s->box == b) {
        static const flow_point_t kScrolls[] = {{0, 0}, {0, 317}, {0, 100000}, {50, 2000},
                                                {INT32_MAX / 2, INT32_MAX / 2}};
        *ok &= s->lo[0] <= 0 && s->hi[0] >= 0 && s->lo[1] <= 0 && s->hi[1] >= 0;
        for (size_t k = 0; k < sizeof(kScrolls) / sizeof(kScrolls[0]); k++) {
            flow_point_t at = flow_box_doc_offset(b, kScrolls[k]);
            flow_point_t from = s->outer != NULL ? flow_box_doc_offset(s->outer->box, kScrolls[k])
                              : b->fixed ? kScrolls[k] : (flow_point_t){0, 0};
            int64_t dx = (int64_t)at.x - from.x, dy = (int64_t)at.y - from.y;
            *ok &= dx >= s->lo[0] && dx <= s->hi[0] && dy >= s->lo[1] && dy <= s->hi[1];
        }
    }
    for (const flow_box_t *c = b->first; c != NULL; c = c->next)
        sticky_held(c, ok);
}

static void intrinsic_once(const FBox *b, bool *ok)
{
    // Measured at most once, and laid out at most once (FLEX.md: a flex
    // item as much as any other box).
    if (b->intrinsic_computed > 1 || b->laid > 1)
        *ok = false;
    for (const FBox *c = b->first; c != NULL; c = c->next)
        intrinsic_once(c, ok);
    for (const FLine *ln = b->lines; ln != NULL; ln = ln->next)
        for (const FFrag *fr = ln->frags; fr != NULL; fr = fr->next)
            if (fr->kind == FF_ATOMIC && fr->item->content != NULL)
                intrinsic_once(fr->item->content, ok);
}

static void fuzz_one(const char *what, const char *html, size_t len)
{
    char *copy = malloc(len + 1);
    memcpy(copy, html, len);
    copy[len] = '\0';
    os64_html_document_t *doc = parse(copy);
    os64_page_t *page = os64_page_build(doc, kPage, NULL, NULL);
    // Through a cascade, which is where positioning comes from: the style
    // attributes the soup writes, and the corpus page's own sheets.
    garb_parsed_t sheets[8];
    garb_sheet_in_t in[8];
    int32_t nsheets = 0;
    for (int32_t i = 0; i < os64_page_nsheets(page) && nsheets < 8; i++) {
        const os64_page_sheet_t *sh = os64_page_sheet(page, i);
        if (sh->linked)
            continue;
        garb_parse_style_element(sh->node, &sheets[nsheets]);
        in[nsheets] = (garb_sheet_in_t){.sheet = &sheets[nsheets], .media = sh->media};
        nsheets++;
    }
    garb_cascade_t *cascade = garb_cascade(in, nsheets, doc, (garb_env_t){.width = 800, .height = 600});
    flow_env_t env = kEnv;
    env.cascade = cascade;
    env.viewport_height = 600;
    static const int32_t widths[] = {800, 200, 0};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        flow_tree_t *t = flow_layout(doc, page, widths[w], &env);
        fuzz_trees++;
        // Whole exactly when the bytes were: a document the parser refused
        // (a mutation nested past its limit), or a model libpage could not
        // finish, lays out as the incomplete prefix it is.
        bool cut = doc == NULL || doc->refusal != 0 || os64_page_incomplete(page);
        if (t == NULL || flow_incomplete(t) != cut) {
            char detail[160];
            snprintf(detail, sizeof(detail), "%s (refusal %lld, model incomplete %d)", what,
                     doc != NULL ? (long long)doc->refusal : -1LL,
                     (int)os64_page_incomplete(page));
            expect("fuzz: laid out whole unless the input was cut", false, detail);
            flow_free(t);
            continue;
        }
        if (flow_root(t) != NULL) {
            check_tree(what, flow_root(t), NULL);
            int32_t at = 0;
            bool listed = true;
            positioned_in_order(flow_root(t), t, &at, &listed);
            expect("fuzz: the positioned list is every positioned box, each once",
                   listed && at == flow_npositioned(t), what);
            bool held = true;
            sticky_held(flow_root(t), &held);
            expect("fuzz: a sticky box moves only as far as its containing block allows",
                   held, what);
        }
        flow_free(t);
    }
    FStyles *st = f_style_build(doc, page, &env);
    FBoxes *bx = f_boxes_build(doc, page, st, &env);
    FLayout *lay = f_layout(bx, doc, page, &env, 300);
    bool once = true;
    if (bx != NULL && bx->root != NULL)
        intrinsic_once(bx->root, &once);
    expect("fuzz: every box measured at most once, and laid out at most once", once, what);
    f_layout_free(lay);
    f_boxes_free(bx);
    f_style_free(st);
    garb_cascade_free(cascade);
    for (int32_t k = 0; k < nsheets; k++)
        garb_free(&sheets[k]);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(copy);
}

// A positioned element with a style drawn at random (POSITION.md § Proof):
// its position, the four insets (negative, huge, a percentage, auto),
// overflow, z-index, a width and a height, and opacity — whatever the
// equations can be handed.
static size_t positioned_soup(char *out, size_t cap)
{
    static const char *const kTags[] = {"div", "span", "p", "b", "table", "td", "li", "a href=x",
                                        "img src=known.png", "dialog open"};
    static const char *const kPosition[] = {"static", "relative", "absolute", "fixed", "sticky"};
    static const char *const kInset[] = {"auto", "0", "-5px", "10px", "50%", "-30%", "1e6px",
                                         "-9999px", "calc(100% - 1em)"};
    static const char *const kSide[] = {"top", "right", "bottom", "left"};
    static const char *const kSize[] = {"auto", "0", "10px", "150%", "30%", "fit-content", "1e6px"};
    static const char *const kOverflow[] = {"visible", "hidden", "clip", "auto"};
    size_t n = (size_t)snprintf(out, cap, "<%s style=\"position:%s", kTags[rng() % 10],
                                kPosition[rng() % 5]);
    for (int k = 0; k < 4; k++)
        if (rng() % 2)
            n += (size_t)snprintf(out + n, cap - n, ";%s:%s", kSide[k], kInset[rng() % 9]);
    if (rng() % 2)
        n += (size_t)snprintf(out + n, cap - n, ";width:%s", kSize[rng() % 7]);
    if (rng() % 2)
        n += (size_t)snprintf(out + n, cap - n, ";height:%s", kSize[rng() % 7]);
    if (rng() % 3 == 0)
        n += (size_t)snprintf(out + n, cap - n, ";overflow:%s", kOverflow[rng() % 4]);
    if (rng() % 4 == 0)
        n += (size_t)snprintf(out + n, cap - n, ";z-index:%d", (int)(rng() % 7) - 3);
    if (rng() % 4 == 0)
        n += (size_t)snprintf(out + n, cap - n, ";opacity:%s", rng() % 2 ? "0" : "0.5");
    if (rng() % 3 == 0)
        n += (size_t)snprintf(out + n, cap - n, ";margin:%s", rng() % 2 ? "auto" : "-7px 3%");
    // Flexible boxes and grids (FLEX.md, GRID.md § Proof): a container's
    // properties, and an item's own.
    static const char *const kItemy[] = {
        "display:flex", "display:inline-flex", "flex-direction:column",
        "flex-direction:row-reverse", "flex-direction:column-reverse", "flex-wrap:wrap",
        "justify-content:space-between", "justify-content:center", "justify-content:space-evenly",
        "align-items:baseline", "align-items:center", "align-self:flex-end", "flex:1",
        "flex:0 0 0", "flex:1e9", "flex:2 3 30%", "flex-basis:content", "order:-3", "order:2",
        "gap:7px 3%", "min-width:0", "max-width:40px", "margin-left:auto",
        // The track kinds, placement from every side, and lines past the
        // bound.
        "display:grid", "display:inline-grid", "grid-template-columns:repeat(auto-fill,minmax(30px,1fr))",
        "grid-template-columns:1fr 2fr auto", "grid-template-columns:fit-content(40px) min-content 9%",
        "grid-template-rows:repeat(3,10px) 1fr", "grid-auto-rows:minmax(5px,auto) 7px",
        "grid-template-areas:'a a' 'b c'", "grid-area:a", "grid-column:span 3", "grid-row:2/-1",
        "grid-column:1/1000000", "grid-row:-7", "grid-auto-flow:column dense", "grid-auto-flow:dense",
        "justify-items:center", "justify-self:end", "place-self:center stretch",
        "place-content:space-around end"};
    for (int k = 0; k < 3; k++)
        if (rng() % 2)
            n += (size_t)snprintf(out + n, cap - n, ";%s",
                                  kItemy[rng() % (sizeof(kItemy) / sizeof(kItemy[0]))]);
    n += (size_t)snprintf(out + n, cap - n, "\">");
    return n;
}

static const char *const kSoup[] = {
    "<table>", "<tr>", "<td>", "</td>", "</table>", "<p>", "</p>", "<b>", "</b>", "<font face=x>",
    "</font>", "<ul><li>", "<br>", "<pre>\t", "<img src=known.png>", "<input>", "<div>",
    "<a href=x>", "</a>", "<td colspan=3 rowspan=0>", "<caption>", "<center>", "<marquee>",
    "<q>", "<ruby>r<rt>t", "<svg><foreignObject>", "<hr>", "<wbr>", "<nobr>", "&nbsp;",
};

// Every page, or only `only`; share k of n (n = 0: the sampled run). The
// mutations are drawn whether or not this share lays them out, so every
// share sees the same sequence and together the shares are the whole run.
static void fuzz_share(const char *only, size_t k, size_t n)
{
    bool full = n != 0;
    size_t turn = 0;
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); i++) {
        if (only != NULL && strcmp(only, kCorpus[i]) != 0)
            continue;
        char path[256];
        snprintf(path, sizeof(path), "tools/html_corpus/%s.html", kCorpus[i]);
        size_t len = 0;
        char *html = slurp(path, &len);
        if (html == NULL)
            continue;
        // Truncated at every 64 bytes (full) or at 24 points spread over it.
        size_t step = full ? 64 : (len / 24 > 64 ? len / 24 : 64);
        for (size_t cut = step; cut < len; cut += step)
            if (!full || turn++ % n == k)
                fuzz_one(kCorpus[i], html, cut);
        size_t mutations = full ? 400 : 12;
        for (size_t m = 0; m < mutations; m++) {
            size_t len2 = len;
            char *buf = malloc(len * 2 + 256);
            memcpy(buf, html, len);
            for (int edit = 0; edit < 4; edit++) {
                size_t at = len2 > 0 ? (size_t)(rng() % len2) : 0;
                switch (rng() % 5) {
                case 0: {   // cut a range
                    size_t span = len2 > at ? (size_t)(rng() % (len2 - at + 1)) % 512 : 0;
                    memmove(buf + at, buf + at + span, len2 - at - span);
                    len2 -= span;
                    break;
                }
                case 1: {   // insert a tag
                    const char *tag = kSoup[rng() % (sizeof(kSoup) / sizeof(kSoup[0]))];
                    size_t tl = strlen(tag);
                    if (len2 + tl < len * 2 + 200) {
                        memmove(buf + at + tl, buf + at, len2 - at);
                        memcpy(buf + at, tag, tl);
                        len2 += tl;
                    }
                    break;
                }
                case 2: {   // repeat a range
                    size_t span = len2 > at ? (size_t)(rng() % 64) : 0;
                    if (at + span <= len2 && len2 + span < len * 2 + 200) {
                        memmove(buf + at + span, buf + at, len2 - at);
                        len2 += span;
                    }
                    break;
                }
                case 4: {   // a positioned element
                    char tag[512];
                    size_t tl = positioned_soup(tag, sizeof(tag));
                    if (len2 + tl < len * 2 + 200) {
                        memmove(buf + at + tl, buf + at, len2 - at);
                        memcpy(buf + at, tag, tl);
                        len2 += tl;
                    }
                    break;
                }
                default:    // one byte of markup where it does not belong
                    if (len2 > 0)
                        buf[at % len2] = "<>/\"'= \n&"[rng() % 9];
                    break;
                }
            }
            if (!full || turn++ % n == k)
                fuzz_one(kCorpus[i], buf, len2);
            free(buf);
        }
        free(html);
    }
    printf("libflow fuzz: %zu trees laid out and checked (%s)\n", fuzz_trees,
           !full ? "sampled; --fuzz for all" : n == 1 ? "every 64 bytes, 400 mutations a page"
                                                  : "one share of the full run");
}

// ── The corpus dumps ────────────────────────────────────────────────────
//
// Regression, not proof: the proof is the hand-computed cases above. These
// say a change moved a real page, and by how much, as a diff to read.

static void corpus_dumps(bool write)
{
    static const struct { int32_t width; const char *suffix; } w[] = {{800, "boxes"},
                                                                         {400, "400.boxes"}};
    size_t same = 0;
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "tools/html_corpus/%s.html", kCorpus[i]);
        size_t len = 0;
        char *html = slurp(path, &len);
        if (html == NULL)
            continue;
        for (size_t k = 0; k < 2; k++) {
            char *got = layout_dump_of(html, w[k].width);
            char out[256];
            snprintf(out, sizeof(out), "tools/html_corpus/%s.%s", kCorpus[i], w[k].suffix);
            if (write) {
                FILE *f = fopen(out, "wb");
                if (f != NULL) {
                    fputs(got, f);
                    fclose(f);
                }
            } else {
                size_t elen = 0;
                char *expected = slurp(out, &elen);
                bool ok = expected != NULL && got != NULL && strcmp(expected, got) == 0;
                expect(out, ok, expected == NULL ? "missing (--write-corpus)" : "differs");
                same += ok;
                free(expected);
            }
            free(got);
        }
        free(html);
    }
    if (!write)
        printf("libflow corpus: %zu dumps match\n", same);
}
