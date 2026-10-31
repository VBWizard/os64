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
}

static void pseudo_cases(void)
{
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
    pseudo_case("pseudo: an author's q::before replaces q's open mark, its ::after left drawn",
        "<!doctype html><style>q::before { content: none }</style><p><q>a</q>",
        "block html\n"
        "  block body\n"
        "    block p\n"
        "      open q\n"
        "      text \"a\"\n"
        "      text \"”\" generated-by q\n"
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
