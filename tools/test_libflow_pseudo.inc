// ::before and ::after (GENERATED_CONTENT.md, slice 8a): pass 1 styles a
// pseudo-element from its own rules and resolves its content; pass 2 walks
// it as the element's first or last child, so it takes whatever box its
// display asks; and a public box it makes names its originating element.

static char *pseudo_boxes_of(const char *html)
{
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    FStyles *styles = f_style_build(p.doc, p.page, &p.env);
    FBoxes *boxes = styles != NULL ? f_boxes_build(p.doc, p.page, styles, &p.env) : NULL;
    char *text = NULL;
    if (boxes != NULL) {
        int64_t need = f_boxes_dump(boxes, NULL, 0);
        text = malloc((size_t)need + 1);
        f_boxes_dump(boxes, text, (size_t)need + 1);
    }
    f_boxes_free(boxes);
    f_style_free(styles);
    sheeted_close(&p);
    return text;
}

static void pseudo_case(const char *name, const char *html, const char *expected)
{
    char *got = pseudo_boxes_of(html);
    bool ok = got != NULL && strcmp(got, expected) == 0;
    expect(name, ok, NULL);
    if (!ok)
        fprintf(stderr, "---- expected\n%s---- got\n%s----\n", expected, got ? got : "(null)\n");
    free(got);
}

// Every public box under `b`: how many name a node whose name no document
// gives (a synthetic node leaked), and how many a pseudo-element of `el`
// made, which must name `el`.
static void pseudo_walk(const flow_box_t *b, const os64_html_node_t *el, int *leaked, int *made)
{
    for (; b != NULL; b = b->next) {
        if (b->node != NULL && b->node->name != NULL && b->node->name[0] == ':')
            (*leaked)++;
        if (b->pseudo != GARB_PSEUDO_NONE && b->node == el)
            (*made)++;
        pseudo_walk(b->first, el, leaked, made);
    }
}

// The boundary rule (GENERATED_CONTENT.md § 4): no public box names a
// synthetic node; a pseudo-element's boxes name its element and say which;
// flow_box_for answers the element's own box.
static void pseudo_boundary_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>a::before { content: \"> \" } a::after { content: \"\"; display: block }"
                     "</style><p><a href=x id=l>link</a>", 800, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    const os64_html_node_t *a = NULL;
    for (const os64_html_node_t *n = p.doc->body; n != NULL && a == NULL; n = n->first_child)
        for (const os64_html_node_t *k = n; k != NULL && a == NULL; k = k->next)
            if (k->kind == OS64_HTML_ELEMENT && k->name != NULL && strcmp(k->name, "a") == 0)
                a = k;
    int leaked = 0, made = 0;
    if (tree != NULL)
        pseudo_walk(flow_root(tree), a, &leaked, &made);
    const flow_box_t *own = tree != NULL && a != NULL ? flow_box_for(tree, a) : NULL;
    expect("pseudo: no public box names a synthetic node", tree != NULL && leaked == 0, NULL);
    expect("pseudo: its boxes name the link and say they are a pseudo-element's", made >= 2, NULL);
    expect("pseudo: the link's own box is the link's", own != NULL && own->pseudo == GARB_PSEUDO_NONE, NULL);
    flow_free(tree);
    sheeted_close(&p);
}

// A replaced element has no pseudo-element at all: pass 1 makes none, as
// well as pass 2 drawing none.
static void pseudo_replaced_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>img::before, img::after, span::before { content: \"x\" }</style>"
                     "<p><img src=x.png><span>s</span>", 800, 600);
    FStyles *styles = f_style_build(p.doc, p.page, &p.env);
    const os64_html_node_t *img = NULL, *span = NULL;
    for (const os64_html_node_t *n = p.doc->body; n != NULL; n = n->first_child)
        for (const os64_html_node_t *k = n; k != NULL; k = k->next)
            if (k->kind == OS64_HTML_ELEMENT && k->name != NULL) {
                if (strcmp(k->name, "img") == 0)
                    img = k;
                if (strcmp(k->name, "span") == 0)
                    span = k;
            }
    const FStyled *si = styles != NULL && img != NULL ? f_style_of(styles, img) : NULL;
    const FStyled *ss = styles != NULL && span != NULL ? f_style_of(styles, span) : NULL;
    expect("pseudo: a replaced element is given none", si != NULL && si->pseudo[0] == NULL && si->pseudo[1] == NULL,
           NULL);
    expect("pseudo: beside it, an element that is not replaced is", ss != NULL && ss->pseudo[0] != NULL, NULL);
    f_style_free(styles);
    sheeted_close(&p);
}

// How many of `el`'s pseudo-elements' boxes are painted, and how many not.
static void pseudo_paint_walk(const flow_box_t *b, const os64_html_node_t *el, int *painted, int *hidden)
{
    for (; b != NULL; b = b->next) {
        if (b->pseudo != GARB_PSEUDO_NONE && b->node == el)
            (*(b->unpainted ? hidden : painted))++;
        pseudo_paint_walk(b->first, el, painted, hidden);
    }
}

static void pseudo_count_text(void *ctx, const flow_box_t *text)
{
    (void)text;
    (*(int *)ctx)++;
}

static const os64_html_node_t *pseudo_find(const os64_html_node_t *n, const char *id)
{
    for (; n != NULL; n = n->next) {
        const os64_html_attr_t *a = n->kind == OS64_HTML_ELEMENT ? os64_html_attr(n, "id") : NULL;
        if (a != NULL && a->value != NULL && strcmp(a->value, id) == 0)
            return n;
        const os64_html_node_t *in = pseudo_find(n->first_child, id);
        if (in != NULL)
            return in;
    }
    return NULL;
}

// The FStyled of `id`'s ::before (`which` 0) or ::after (1), or NULL.
static const FStyled *pseudo_styled(const FStyles *styles, const os64_html_document_t *doc, const char *id,
                                    int which)
{
    const FStyled *s = styles != NULL ? f_style_of(styles, pseudo_find(doc->document, id)) : NULL;
    return s != NULL && s->pseudo[which] != NULL ? f_style_of(styles, s->pseudo[which]) : NULL;
}

// What an element's child is given of it, a pseudo-element is given too:
// an atomic inline's edge to the decorations, a border style's medium
// width, and the parent's animation moving by layouts when it takes from it.
static void pseudo_style_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>a { text-decoration: underline }"
                     " a::before { content: \"I\"; display: inline-block } a::after { content: \"!\" }"
                     " #b::before { content: \"\"; border-style: solid; outline-style: solid }</style>"
                     "<p><a href=x id=a>link</a><div id=b></div>", 800, 600);
    FStyles *styles = f_style_build(p.doc, p.page, &p.env);
    const FStyled *icon = pseudo_styled(styles, p.doc, "a", 0), *mark = pseudo_styled(styles, p.doc, "a", 1);
    const FStyled *box = pseudo_styled(styles, p.doc, "b", 0);
    expect("pseudo: an inline-block pseudo-element stops its link's underline", icon != NULL && icon->decoration == 0,
           NULL);
    expect("pseudo: an inline one takes it", mark != NULL && mark->decoration != 0, NULL);
    expect("pseudo: a border style with no width is medium",
           box != NULL && box->style.border_width[0] == 3 * FLOW_UNITS_PER_PX, NULL);
    expect("pseudo: an outline style with no width is medium",
           box != NULL && box->style.outline_width == 3 * FLOW_UNITS_PER_PX, NULL);
    f_style_free(styles);
    sheeted_close(&p);

    static const char *const kTakes[] = {
        "<!doctype html><style>@keyframes k { to { background-color: red } } #m { animation: k 1s }"
        " #m::before { content: \"x\" }</style><div id=m>m</div>",
        "<!doctype html><style>@keyframes k { to { background-color: red } } #m { animation: k 1s }"
        " #m::before { content: \"x\"; background-color: inherit }</style><div id=m>m</div>",
    };
    bool ticks[2] = {false, false};
    for (int i = 0; i < 2; i++) {
        sheeted_open(&p, kTakes[i], 800, 600);
        flow_timeline_t *tl = flow_timeline_new(p.doc);
        p.env.timeline = tl;
        p.env.now_ms = 100;
        p.env.ticks = true;
        FStyles *fs = f_style_build(p.doc, p.page, &p.env);
        const FStyled *m = fs != NULL ? f_style_of(fs, pseudo_find(p.doc->document, "m")) : NULL;
        ticks[i] = m != NULL && m->motion != NULL && m->motion->ticks;
        f_style_free(fs);
        flow_timeline_free(tl);
        sheeted_close(&p);
    }
    expect("pseudo: an element's animation ticks beside a pseudo-element that takes nothing of it", ticks[0], NULL);
    expect("pseudo: and moves by layouts when its pseudo-element inherits what it animates", !ticks[1], NULL);
}

// A pseudo-element's content is charged to the lists' budget, as a list
// one rule copies into every element it reaches is, and only when it
// exists: four bytes more in the rule cost four bytes for the one
// pseudo-element drawn, none for the one whose display is none.
static void pseudo_budget_cases(void)
{
    static const char *const kPages[] = {
        "<!doctype html><style>div::before { content: \"abcd\" } #n::before { display: none }</style>"
        "<div>1</div><div id=n>2</div>",
        "<!doctype html><style>div::before { content: \"abcdefgh\" } #n::before { display: none }</style>"
        "<div>1</div><div id=n>2</div>",
    };
    size_t left[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        Sheeted p;
        sheeted_open(&p, kPages[i], 800, 600);
        FStyles *styles = f_style_build(p.doc, p.page, &p.env);
        left[i] = styles != NULL ? styles->lists_left : 0;
        f_style_free(styles);
        sheeted_close(&p);
    }
    expect("pseudo: content is charged to the lists' budget, once per pseudo-element drawn",
           left[0] != 0 && left[0] - left[1] == 4, NULL);
}

// The root element's ::before is laid out, as its ::after is; and a
// pseudo-element's text clip holds its own text, not its element's.
static void pseudo_layout_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>html::before { content: \"X\"; display: block }"
                     " #s::before { content: \"B\" }</style><p><span id=s>host<b>child</b></span>", 800, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    int leaked = 0, made = 0;
    if (tree != NULL)
        pseudo_walk(flow_root(tree), p.doc->html, &leaked, &made);
    expect("pseudo: the root element's ::before is laid out", tree != NULL && made >= 1, NULL);
    const os64_html_node_t *span = pseudo_find(p.doc->document, "s");
    const flow_box_t *own = tree != NULL ? flow_box_for(tree, span) : NULL;
    const flow_box_t *gen = NULL;
    if (own != NULL && own->parent != NULL)
        for (const flow_box_t *k = own->parent->first; k != NULL && gen == NULL; k = k->next)
            if (k->kind == FLOW_BOX_SPAN && k->node == span && k->pseudo == GARB_PSEUDO_BEFORE)
                gen = k;
    int seen[2] = {0, 0};
    flow_visit_text_clip(gen, pseudo_count_text, &seen[0]);
    flow_visit_text_clip(own, pseudo_count_text, &seen[1]);
    expect("pseudo: a pseudo-element's text clip is its own text", gen != NULL && seen[0] == 1, NULL);
    expect("pseudo: its element's holds its own and the pseudo-element's", own != NULL && seen[1] == 3, NULL);
    flow_free(tree);
    sheeted_close(&p);

    // Its opacity is its own: an invisible ::before of a visible element
    // is not painted, and the element still is.
    sheeted_open(&p, "<!doctype html><style>#h::before { content: \"HIDDEN\"; opacity: 0 }</style>"
                     "<p><span id=h>shown</span>", 800, 600);
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    span = pseudo_find(p.doc->document, "h");
    own = tree != NULL ? flow_box_for(tree, span) : NULL;
    int painted = 0, hidden = 0;
    if (tree != NULL)
        pseudo_paint_walk(flow_root(tree), span, &painted, &hidden);
    expect("pseudo: a transparent pseudo-element is not painted", hidden >= 2 && painted == 0, NULL);
    expect("pseudo: its element still is", own != NULL && !own->unpainted, NULL);
    flow_free(tree);
    sheeted_close(&p);
}

// ── Counters and quotes (GENERATED_CONTENT.md § 8) ────────────────────

static void texts_put(char *out, size_t cap, const char *text, size_t len)
{
    size_t at = strlen(out);
    snprintf(out + at, cap - at, "%.*s|", (int)len, text);
}

// What pass 1 generated, in tree order: each ::before's text (a `q`'s open
// mark too), the children, each ::after's.
static void texts_walk(const FStyles *st, const os64_html_node_t *n, char *out, size_t cap)
{
    for (; n != NULL; n = n->next) {
        const FStyled *s = n->kind == OS64_HTML_ELEMENT ? f_style_of(st, n) : NULL;
        if (s == NULL)
            continue;
        if (s->pseudo[0] != NULL && s->pseudo[0]->first_child != NULL)
            texts_put(out, cap, s->pseudo[0]->first_child->text, s->pseudo[0]->first_child->text_len);
        texts_walk(st, n->first_child, out, cap);
        if (s->pseudo[1] != NULL && s->pseudo[1]->first_child != NULL)
            texts_put(out, cap, s->pseudo[1]->first_child->text, s->pseudo[1]->first_child->text_len);
    }
}

static void texts_case_in(const char *name, const char *html, bool scripting, const char *expected)
{
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    p.env.scripting = scripting;
    FStyles *styles = f_style_build(p.doc, p.page, &p.env);
    char got[512] = "";
    if (styles != NULL)
        texts_walk(styles, p.doc->html, got, sizeof(got));
    bool ok = styles != NULL && strcmp(got, expected) == 0;
    char detail[600];
    snprintf(detail, sizeof(detail), "got \"%s\"", got);
    expect(name, ok, ok ? NULL : detail);
    f_style_free(styles);
    sheeted_close(&p);
}

static void texts_case(const char *name, const char *html, const char *expected)
{
    texts_case_in(name, html, false, expected);
}

// counters() whose spelling would not fit in the lists' budget is refused
// before it is made: a 1 MiB joiner between 64 counters is never spelled
// out, the pseudo-element is lost, and the page's next one is drawn.
static void counter_budget_cases(void)
{
    size_t joiner = (size_t)1 << 20;
    char *html = malloc(joiner + 4096);
    int at = sprintf(html, "<!doctype html><style>div { counter-reset: n } i::before { content: \"ok\" }"
                           " b::before { content: counters(n, \"");
    memset(html + at, 'x', joiner);
    at += (int)joiner;
    at += sprintf(html + at, "\") }</style>");
    for (int i = 0; i < 64; i++)
        at += sprintf(html + at, "<div>");
    sprintf(html + at, "<b></b><i></i>");
    largest = 0;
    texts_case("counters: one too long for the budget is lost, the next drawn", html, "ok|");
    char detail[64];
    snprintf(detail, sizeof(detail), "largest allocation %zu", largest);
    expect("counters: and its spelling is never made", largest < ((size_t)16 << 20), detail);
    free(html);
}

static void counter_cases(void)
{
    counter_budget_cases();
    texts_case("counters: inherit through an ancestor that inherits too",
        "<!doctype html><style>body { counter-reset: k 7 } div, p { counter-reset: inherit }"
        " p::before { content: counters(k, \"-\") }</style><div><p></p></div>",
        "7-7-7|");
    texts_case("quotes: revert keeps the inherited pairs",
        "<!doctype html><style>body { quotes: \"<\" \">\" } q { quotes: revert }</style><p><q>x</q>",
        "<|>|");
    texts_case("quotes: q's own marks take their pseudo-element's quotes",
        "<!doctype html><style>q::before, q::after { quotes: \"[\" \"]\" }</style><p><q>x</q>",
        "[|]|");
    texts_case("counters: a q's own ::before counts, its content the chapter's",
        "<!doctype html><style>body { counter-reset: n } q::before { counter-increment: n }"
        " p::before { content: counter(n) }</style><div><q>x</q></div><p>t</p>",
        "\xe2\x80\x9c|\xe2\x80\x9d|1|");
    cascade_layout_case("quotes: a q's own marks are its text, transformed as its text is",
        "<!doctype html><style>body { margin: 0 } p { margin: 0 }"
        " q { quotes: \"a\" \"b\"; text-transform: uppercase }</style><p><q>x</q>",
        200,
        "page 200 20\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 20\n"
        "    block p 0 0 200 20\n"
        "      line 0 0 200 20 base 14\n"
        "        span q 0 2 8 16\n"
        "        span q 16 2 8 16\n"
        "        span q 0 2 24 16\n"
        "        text \"A\" 0 2 8 16 serif 16\n"
        "        text \"X\" 8 2 8 16 serif 16\n"
        "        text \"B\" 16 2 8 16 serif 16\n");
    texts_case("quotes: initial is auto",
        "<!doctype html><style>body { quotes: \"<\" \">\" } q { quotes: initial }</style><p><q>x</q>",
        "\xe2\x80\x9c|\xe2\x80\x9d|");
    texts_case("counters: an audio, and its fallback, count and quote for nothing",
        "<!doctype html><style>body { counter-reset: n } span, audio { counter-increment: n }"
        " span::before { content: open-quote } p::before { content: counter(n) }</style>"
        "<audio><span>f</span></audio><p><q>x</q></p>",
        "0|\xe2\x80\x9c|\xe2\x80\x9d|");
    texts_case("counters: a button laid out through display: contents generates inside",
        "<!doctype html><style>button { display: contents } span::before { content: \"B\" }</style>"
        "<p><button><span>x</span><q>y</q></button>",
        "B|\xe2\x80\x9c|\xe2\x80\x9d|");
    texts_case_in("counters: a canvas's fallback counts and quotes for nothing",
        "<!doctype html><style>body { counter-reset: n } span { counter-increment: n }"
        " span::before { content: open-quote } p::before { content: counter(n) }</style>"
        "<canvas><span>f</span></canvas><p><q>x</q></p>",
        true, "0|\xe2\x80\x9c|\xe2\x80\x9d|");

    texts_case("counters: nested, joined from the outermost in",
        "<!doctype html><style>.l { counter-reset: item } .i { counter-increment: item }"
        " .i::before { content: counters(item, \".\") \" \" }</style>"
        "<div class=l><div class=i>a</div><div class=i>b<div class=l><div class=i>c</div></div></div>"
        "<div class=i>d</div></div>",
        "1 |2 |2.1 |3 |");
    texts_case("counters: a sibling's reset replaces the counter, never nests in it",
        "<!doctype html><style>p { counter-reset: x 5 } p::before { content: counter(x) }"
        " span::before { content: counters(x, \".\") }</style><div><p></p><p></p><span></span></div>",
        "5|5|5|");
    texts_case("counters: an increment with no reset makes the counter, shared by its siblings",
        "<!doctype html><style>i { counter-increment: n } i::before { content: counter(n) }</style>"
        "<p><i></i><i></i><i></i>",
        "1|2|3|");
    texts_case("counters: reset, then increment, then set",
        "<!doctype html><style>p { counter-reset: a 10; counter-increment: a 5; counter-set: a 2 }"
        " p::before { content: counter(a) } div { counter-reset: b 10; counter-increment: b 5 }"
        " div::before { content: counter(b) }</style><p></p><div></div>",
        "2|15|");
    texts_case("counters: an ::after sees its element's children's increments",
        "<!doctype html><style>div { counter-reset: c } span { counter-increment: c }"
        " div::before, div::after { content: counter(c) }</style><div><span>x</span><span>y</span></div>",
        "0|2|");
    texts_case("counters: a pseudo-element's own increment, before its content",
        "<!doctype html><style>body { counter-reset: h } h2::before { counter-increment: h;"
        " content: counter(h) \". \" }</style><h2>a</h2><h2>b</h2>",
        "1. |2. |");
    texts_case("counters: display none counts for nothing",
        "<!doctype html><style>i { counter-increment: n } i::before { content: counter(n) } .h { display: none }"
        "</style><p><i></i><i class=h></i><i></i>",
        "1|2|");
    texts_case("counters: and neither does a pseudo-element that does not exist",
        "<!doctype html><style>i { counter-increment: n } i::before { content: counter(n) }"
        " i::after { counter-increment: n 10 }</style><p><i></i><i></i>",
        "1|2|");
    texts_case("counters: each style spelled, an unknown one decimal",
        "<!doctype html><style>p { counter-reset: a 4 } p::before { content: counter(a, lower-roman)"
        " counter(a, upper-alpha) counter(a, disc) \"/\" counter(a, none) \"/\" counter(a, lower-greek) }</style><p></p>",
        "ivD\xe2\x80\xa2//4|");
    texts_case("counters: a reset by inherit takes the parent's",
        "<!doctype html><style>div { counter-reset: k 7 } p { counter-reset: inherit }"
        " p::before { content: counters(k, \"-\") }</style><div><p></p></div>",
        "7-7|");

    texts_case("quotes: by pair, the last repeated past them",
        "<!doctype html><style>body { quotes: \"<\" \">\" \"(\" \")\" }</style><p><q>a<q>b<q>c</q></q></q>",
        "<|(|(|)|)|>|");
    texts_case("quotes: auto, English's double then single",
        "<!doctype html><p><q>a<q>b<q>c</q></q></q>",
        "\xe2\x80\x9c|\xe2\x80\x98|\xe2\x80\x98|\xe2\x80\x99|\xe2\x80\x99|\xe2\x80\x9d|");
    texts_case("quotes: none draws nothing and still deepens",
        "<!doctype html><style>.n { quotes: none } .a { quotes: auto }</style><p><q class=n>a<q class=a>b</q></q>",
        "\xe2\x80\x98|\xe2\x80\x99|");
    texts_case("quotes: a close at depth 0 is nothing, the rest drawn",
        "<!doctype html><style>p::before { content: close-quote \"x\" }</style><p></p>",
        "x|");
    texts_case("quotes: no-open-quote deepens, drawing nothing",
        "<!doctype html><style>p::before { content: no-open-quote } p::after { content: close-quote }</style><p></p>",
        "\xe2\x80\x9d|");
    texts_case("quotes: a q after an author's open-quote takes the next pair",
        "<!doctype html><style>p::before { content: open-quote }</style><p><q>a</q>",
        "\xe2\x80\x9c|\xe2\x80\x98|\xe2\x80\x99|");

    static const char *const kOrdered[] = {
        "<!doctype html><p>plain",
        "<!doctype html><style>p::before { content: counter(n) }</style><p>x",
        "<!doctype html><p><q>x</q>",
    };
    bool ordered[3];
    for (int i = 0; i < 3; i++) {
        Sheeted p;
        sheeted_open(&p, kOrdered[i], 800, 600);
        FStyles *styles = f_style_build(p.doc, p.page, &p.env);
        ordered[i] = styles != NULL && styles->ordered;
        f_style_free(styles);
        sheeted_close(&p);
    }
    expect("counters: a pass that met none is not ordered", !ordered[0], NULL);
    expect("counters: one that met a counter is", ordered[1], NULL);
    expect("counters: and one that met a q", ordered[2], NULL);
}

// An author's content on an `rt`'s ::before or ::after takes the place of
// the parentheses yonder draws round it, as it does `q`'s marks.
static void pseudo_rt_cases(void)
{
    char *plain = pseudo_boxes_of("<!doctype html><p><ruby>a<rt>b</rt></ruby>");
    char *taken = pseudo_boxes_of("<!doctype html><style>rt::before, rt::after { content: \"\" }</style>"
                                  "<p><ruby>a<rt>b</rt></ruby>");
    expect("pseudo: an rt is drawn with its parentheses",
           plain != NULL && strstr(plain, "text \"(\" generated-by rt") != NULL, NULL);
    expect("pseudo: an author's ::before and ::after take their place",
           taken != NULL && strstr(taken, "text \"(\"") == NULL && strstr(taken, "text \")\"") == NULL, NULL);
    free(plain);
    free(taken);
}

static void pseudo_cases(void)
{
    pseudo_rt_cases();
    counter_cases();
    pseudo_style_cases();
    pseudo_budget_cases();
    pseudo_layout_cases();
    pseudo_boundary_cases();
    pseudo_replaced_cases();
    pseudo_case("pseudo: inline ::before and ::after, strings and attr()",
        "<!doctype html><style>p::before { content: \"[\" attr(TITLE) \"] \" } p::after { content: \"!\" }</style>"
        "<p title=T>x",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open ::before\n"
        "      text \"[T] \"\n"
        "      close ::before\n"
        "      text \"x\"\n"
        "      open ::after\n"
        "      text \"!\"\n"
        "      close ::after\n");
    pseudo_case("pseudo: a block ::after, empty, is a box (the clearfix)",
        "<!doctype html><style>div::after { content: \"\"; display: block }</style><div>x</div>",
        "block html\n"
        "  block body\n"
        "    block div\n"
        "      block anon\n"
        "        text \"x\"\n"
        "      block ::after\n");
    pseudo_case("pseudo: none, normal, display none and no content make nothing",
        "<!doctype html><style>.a::before { content: none } .b::before { content: normal }"
        ".c::before { content: \"x\"; display: none } .d::before { color: red }</style>"
        "<p class=a>a<p class=b>b<p class=c>c<p class=d>d",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      text \"a\"\n"
        "    block p\n"
        "      text \"b\"\n"
        "    block p\n"
        "      text \"c\"\n"
        "    block p\n"
        "      text \"d\"\n");
    pseudo_case("pseudo: a replaced element has none; display: contents keeps its own",
        "<!doctype html><style>img::before { content: \"no\" } span::before { content: \"yes\" }"
        "span { display: contents }</style><p><img src=x.png><span>s</span>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      atomic img\n"
        "      open ::before\n"
        "      text \"yes\"\n"
        "      close ::before\n"
        "      text \"s\"\n");
    pseudo_case("pseudo: an absolute ::before leaves the flow",
        "<!doctype html><style>p { position: relative } p::before { content: \"\"; position: absolute }</style>"
        "<p>x",
        "block html\n"
        "  block body\n"
        "    block p positioned\n"
        "      placeholder ::before\n"
        "      text \"x\"\n"
        "      block ::before out-of-flow\n");
    pseudo_case("pseudo: an author's q::before replaces q's open mark, its own close left drawn",
        "<!doctype html><style>q::before { content: \"<\" open-quote }</style><p><q>a</q>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open q\n"
        "      open ::before\n"
        "      text \"<“\"\n"
        "      close ::before\n"
        "      text \"a\"\n"
        "      open ::after\n"
        "      text \"”\"\n"
        "      close ::after\n"
        "      close q\n");
    // Content 3 § 4.2: with no open mark before it, q's close is at depth 0,
    // and draws nothing; its ::after is there, empty.
    pseudo_case("pseudo: q::before { content: none } leaves its close at depth 0, drawn as nothing",
        "<!doctype html><style>q::before { content: none }</style><p><q>a</q>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open q\n"
        "      text \"a\"\n"
        "      open ::after\n"
        "      close ::after\n"
        "      close q\n");
    pseudo_case("pseudo: a table cell's and a caption's are their content's",
        "<!doctype html><style>td::before { content: attr(data-label) \": \" } caption::after { content: \"*\" }</style>"
        "<table><caption>c</caption><tr><td data-label=Name>v</td></tr></table>",
        "block html\n"
        "  block body\n"
        "    table table\n"
        "      caption caption\n"
        "        text \"c\"\n"
        "        open ::after\n"
        "        text \"*\"\n"
        "        close ::after\n"
        "      row-group tbody\n"
        "        row tr\n"
        "          cell td\n"
        "            open ::before\n"
        "            text \"Name: \"\n"
        "            close ::before\n"
        "            text \"v\"\n");
    pseudo_case("pseudo: quote words outside q, and a flex container's ::before is an item",
        "<!doctype html><style>p::before { content: open-quote } p::after { content: close-quote no-close-quote }"
        "div { display: flex } div::before { content: \"i\" }</style><p>x<div>y</div>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open ::before\n"
        "      text \"“\"\n"
        "      close ::before\n"
        "      text \"x\"\n"
        "      open ::after\n"
        "      text \"”\"\n"
        "      close ::after\n"
        "    block div flex\n"
        "      block ::before\n"
        "        text \"i\"\n"
        "      block anon\n"
        "        text \"y\"\n");
}
