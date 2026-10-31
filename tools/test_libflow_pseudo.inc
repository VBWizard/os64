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

static void pseudo_cases(void)
{
    pseudo_boundary_cases();
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
