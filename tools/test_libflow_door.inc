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
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *t = flow_layout(doc, page, 400, &kEnv);
    expect("door: laid out", t != NULL && !flow_incomplete(t), NULL);
    if (t == NULL)
        return;

    const flow_box_t *h = flow_hit(t, 30, 40);
    expect("door: a click on a link's text is that text, in that link",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 4 &&
           memcmp(h->text, "here", 4) == 0 && h->link == 0, NULL);
    h = flow_hit(t, 70, 20);
    expect("door: a click on a picture is its atom",
           h != NULL && h->kind == FLOW_BOX_ATOMIC && h->rect.x == 60 && h->rect.y == 16 &&
           h->rect.w == 40 && h->rect.h == 30, NULL);
    h = flow_hit(t, 390, 20);
    expect("door: a click past the line's content is the line",
           h != NULL && h->kind == FLOW_BOX_LINE, NULL);
    expect("door: a click off the page is nothing", flow_hit(t, 500, 20) == NULL, NULL);

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
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 1000}, seen_add, &all);
    char order[512] = "";
    for (int i = 0; i < all.n; i++) {
        strcat(order, kind_name(all.v[i]->kind));
        strcat(order, " ");
    }
    expect("door: painting order — blocks, then spans, then content",
           strcmp(order, "block block block span text text text atomic text atomic text text ")
               == 0, order);
    Seen some = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){250, 0, 20, 100}, seen_add, &some);
    order[0] = '\0';
    for (int i = 0; i < some.n; i++) {
        strcat(order, kind_name(some.v[i]->kind));
        strcat(order, " ");
    }
    expect("door: the walk prunes what the viewport does not meet",
           strcmp(order, "block block block atomic text text ") == 0, order);
    Seen none = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){0, 100, 400, 50}, seen_add, &none);
    expect("door: below the page, nothing", none.n == 0, NULL);
    flow_free(t);

    // A word wider than its line: the paragraph's OVERFLOW holds it, so
    // the walk and the hit test still reach it past the paragraph's edge.
    os64_html_document_t *doc2 = parse("<!doctype html><p>abcdefghij");
    os64_page_t *page2 = os64_page_build(doc2, kPage, NULL);
    t = flow_layout(doc2, page2, 50, &kEnv);
    const flow_box_t *root = flow_root(t);
    expect("door: overflow reaches past the box",
           root != NULL && root->rect.w == 50 && root->overflow.w == 76 &&
           flow_width(t) == 76, NULL);
    h = flow_hit(t, 70, 20);
    expect("door: a click on the overflowing word finds it",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 10, NULL);
    flow_free(t);
    os64_page_free(page2);
    os64_html_document_free(doc2);

    // A table wider than the page hangs out of the html and body boxes: a
    // viewport that meets only the part hanging out still reaches it.
    doc2 = parse("<!doctype html><table width=200><tr><td>x</table>");
    page2 = os64_page_build(doc2, kPage, NULL);
    t = flow_layout(doc2, page2, 50, &kEnv);
    Seen out = {{0}, 0};
    flow_visit(t, (os64_gui_rect_t){100, 0, 10, 100}, seen_add, &out);
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

// ── The allocation sweep: rules (a), (b), (c) of LAYOUT.md § Proof ────────

typedef struct {
    const flow_box_t **v;
    int32_t *parent;
    size_t n, cap;
} Flat;

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

// Beside (a)(b)(c), a partial page is as tall as the blocks it holds, so
// what did lay out can be scrolled to.
static void layout_relation_sweep(const char *what, const char *html, int32_t width)
{
    os64_html_document_t *doc = parse(html);
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    flow_tree_t *whole = flow_layout(doc, page, width, &kEnv);
    Flat w = {0};
    flatten(&w, flow_root(whole), -1);
    size_t base_live = live;
    allocations = 0;
    flow_tree_t *probe = flow_layout(doc, page, width, &kEnv);
    size_t count = allocations;
    flow_free(probe);
    size_t partial = 0, nulls = 0, compared = 0;
    fail_single = true;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        flow_tree_t *t = flow_layout(doc, page, width, &kEnv);
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
                // Under an unfinished table or line, geometry is not compared:
                // a table's columns come from the cells it has.
                bool exempt = false;
                for (int32_t up = p.parent[i]; up >= 0 && !exempt; up = p.parent[up])
                    exempt = p.v[up]->unfinished && (p.v[up]->kind == FLOW_BOX_TABLE ||
                                                     p.v[up]->kind == FLOW_BOX_LINE);
                if (exempt)
                    continue;
                const os64_gui_rect_t *x = &p.v[i]->rect, *y = &w.v[i]->rect;
                if (x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h)
                    same = false;
                compared++;
            }
            expect(name, same, NULL);
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
            for (size_t i = 0; i < p.n; i++)
                if (p.v[i]->kind == FLOW_BOX_BLOCK && p.v[i]->rect.y + p.v[i]->rect.h > bottom)
                    bottom = p.v[i]->rect.y + p.v[i]->rect.h;
            snprintf(detail, sizeof(detail), "height %d, a block reaches %lld, root %d+%d%s",
                     flow_height(t), (long long)bottom, p.v[0]->rect.y, p.v[0]->rect.h,
                     p.v[0]->unfinished ? " unfinished" : "");
            expect(name, flow_height(t) >= bottom, detail);
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
    printf("libflow relation sweep, %s: %zu failures: %zu NULL, %zu partial trees held to (a)(b)(c), "
           "%zu finished boxes compared\n", what, count, nulls, partial, compared);
    free(w.v);
    free(w.parent);
    flow_free(whole);
    os64_page_free(page);
    os64_html_document_free(doc);
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

// The invariants of any laid-out tree, whatever the page did.
static void check_tree(const char *what, const flow_box_t *b, const flow_box_t *parent)
{
    bool ok = b->rect.w >= 0 && b->rect.h >= 0 && b->overflow.w >= 0 && b->overflow.h >= 0;
    if (parent != NULL) {
        const os64_gui_rect_t *o = &b->overflow, *po = &parent->overflow;
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
        check_tree(what, c, b);
}

static void intrinsic_once(const FBox *b, bool *ok)
{
    if (b->intrinsic_computed > 1)
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
    os64_page_t *page = os64_page_build(doc, kPage, NULL);
    static const int32_t widths[] = {800, 200, 0};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        flow_tree_t *t = flow_layout(doc, page, widths[w], &kEnv);
        fuzz_trees++;
        if (t == NULL || flow_incomplete(t)) {
            expect("fuzz: laid out whole", false, what);
            flow_free(t);
            continue;
        }
        if (flow_root(t) != NULL)
            check_tree(what, flow_root(t), NULL);
        flow_free(t);
    }
    FStyles *st = f_style_build(doc, page, &kEnv);
    FBoxes *bx = f_boxes_build(doc, page, st, &kEnv);
    FLayout *lay = f_layout(bx, doc, page, &kEnv, 300);
    bool once = true;
    if (bx != NULL && bx->root != NULL)
        intrinsic_once(bx->root, &once);
    expect("fuzz: every box's widths measured at most once", once, what);
    f_layout_free(lay);
    f_boxes_free(bx);
    f_style_free(st);
    os64_page_free(page);
    os64_html_document_free(doc);
    free(copy);
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
                switch (rng() % 4) {
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
