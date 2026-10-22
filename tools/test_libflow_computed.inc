// Computed values (flow_computed_*, CSSOM § getComputedStyle), worked by
// hand from the page below at 800 wide: the body has no margin, so a
// percentage of the containing block is of 800 px, and each expected text
// is what a browser's getComputedStyle answers for that property there.

static const char kComputedPage[] =
    "<!doctype html><style>"
    "body{margin:0}"
    "#box{width:200px;height:50px;padding:10px 5%;margin:4px auto;border:2px solid rgb(10,20,30);"
    "color:rgba(255,0,0,0.5);background-color:#00ff00;font-size:20px}"
    "#hidden{display:none;color:blue;font-size:20px}"
    "#inner{display:block;width:30%;margin-left:2em;line-height:1.5}"
    "#pos{position:relative;top:10px;left:5px;z-index:3;opacity:0.25;"
    "transform:translate(10px,20px) scale(2);transform-origin:0 0}"
    "#grid{display:grid;grid-template-columns:100px 1fr minmax(10px,auto);grid-template-areas:\"a b .\"}"
    "#face{font-family:\"Times New Roman\",serif;font-weight:bold;font-style:italic}"
    "#flt{float:left;margin:auto;width:10px;height:10px;box-sizing:border-box;padding:1px}"
    "#shadow{box-shadow:2px 3px rgb(1,2,3);text-shadow:1px 1px 2px currentcolor;color:rgb(9,9,9)}"
    "#anim{animation:spin 2s linear infinite;will-change:transform}"
    "</style>"
    "<div id=box>box</div><div id=hidden><p id=inner>x<span id=deep>y</span></p></div>"
    "<div id=pos>p</div><div id=grid></div><span id=face>s</span><div id=flt></div>"
    "<div id=shadow>t</div><div id=anim>a</div>";

static const os64_html_node_t *computed_find(const os64_html_node_t *n, const char *id)
{
    for (; n != NULL; n = n->next) {
        const os64_html_attr_t *a = n->kind == OS64_HTML_ELEMENT ? os64_html_attr(n, "id") : NULL;
        if (a != NULL && strcmp(a->value, id) == 0)
            return n;
        const os64_html_node_t *in = computed_find(n->first_child, id);
        if (in != NULL)
            return in;
    }
    return NULL;
}

// One property's text, held to `want`, and to the same text cut at every
// shorter buffer.
static void computed_is(const flow_computed_t *c, const char *what, int32_t prop, const char *want)
{
    char got[512];
    size_t n = flow_computed_text(c, prop, got, sizeof(got));
    bool ok = n == strlen(got) && strcmp(got, want) == 0;
    for (size_t cap = 0; ok && cap <= n; cap++) {
        char cut[512];
        memset(cut, 'x', sizeof(cut));
        ok = flow_computed_text(c, prop, cut, cap) == n &&
             (cap == 0 ? cut[0] == 'x' : strlen(cut) == cap - 1 && strncmp(cut, got, cap - 1) == 0);
    }
    char detail[700];
    snprintf(detail, sizeof(detail), "got \"%s\", want \"%s\"", got, want);
    expect(what, ok, detail);
}

static bool computed_url(void *ctx, const char *written, uint32_t len, int32_t sheet, char *out,
                         size_t cap)
{
    (void)ctx;
    (void)sheet;
    snprintf(out, cap, "http://host/dir/%.*s", (int)len, written);
    return true;
}

static void computed_cases(void)
{
    Sheeted p;
    sheeted_open(&p, kComputedPage, 800, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    expect("computed: the page lays out", tree != NULL, NULL);
    if (tree == NULL) {
        sheeted_close(&p);
        return;
    }
    const os64_html_node_t *root = p.doc->document;
    flow_computed_t *c = flow_computed_new(tree, &p.env, computed_find(root, "box"), NULL);
    expect("computed: a laid-out element has a style", flow_computed_styled(c), NULL);
    computed_is(c, "computed: display", GARB_DISPLAY, "block");
    computed_is(c, "computed: width is the used content width", GARB_WIDTH, "200px");
    computed_is(c, "computed: height", GARB_HEIGHT, "50px");
    computed_is(c, "computed: a percentage padding is the containing block's", GARB_PADDING_LEFT, "40px");
    computed_is(c, "computed: padding-top", GARB_PADDING_TOP, "10px");
    computed_is(c, "computed: an auto margin is what the block left", GARB_MARGIN_LEFT, "258px");
    computed_is(c, "computed: and the other side's", GARB_MARGIN_RIGHT, "258px");
    computed_is(c, "computed: margin-top", GARB_MARGIN_TOP, "4px");
    computed_is(c, "computed: border width", GARB_BORDER_TOP_WIDTH, "2px");
    computed_is(c, "computed: border style", GARB_BORDER_LEFT_STYLE, "solid");
    computed_is(c, "computed: border colour", GARB_BORDER_BOTTOM_COLOR, "rgb(10, 20, 30)");
    computed_is(c, "computed: a translucent colour is rgba with the fewest decimals", GARB_COLOR,
                "rgba(255, 0, 0, 0.5)");
    computed_is(c, "computed: background-color", GARB_BACKGROUND_COLOR, "rgb(0, 255, 0)");
    computed_is(c, "computed: font-size", GARB_FONT_SIZE, "20px");
    computed_is(c, "computed: position", GARB_POSITION, "static");
    computed_is(c, "computed: a static box's inset is as computed", GARB_TOP, "auto");
    computed_is(c, "computed: line-height normal", GARB_LINE_HEIGHT, "normal");
    computed_is(c, "computed: z-index auto", GARB_Z_INDEX, "auto");
    computed_is(c, "computed: transform none", GARB_TRANSFORM, "none");
    computed_is(c, "computed: background-image none", GARB_BACKGROUND_IMAGE, "none");
    computed_is(c, "computed: max-width none", GARB_MAX_WIDTH, "none");
    computed_is(c, "computed: min-width auto", GARB_MIN_WIDTH, "auto");
    computed_is(c, "computed: font-variant, which libflow does not model, is its initial value",
                GARB_FONT_VARIANT, "normal");
    computed_is(c, "computed: a property past the table is nothing", GARB_NPROPS, "");
    computed_is(c, "computed: nor before it", -1, "");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "hidden"), NULL);
    computed_is(c, "computed: display none", GARB_DISPLAY, "none");
    computed_is(c, "computed: its colour", GARB_COLOR, "rgb(0, 0, 255)");
    computed_is(c, "computed: an element with no box answers its computed width", GARB_WIDTH, "auto");
    flow_computed_free(c);

    size_t before = live;
    c = flow_computed_new(tree, &p.env, computed_find(root, "inner"), NULL);
    expect("computed: an element inside display:none is styled for the asking",
           flow_computed_styled(c), NULL);
    computed_is(c, "computed: its own display", GARB_DISPLAY, "block");
    computed_is(c, "computed: it inherits from the undisplayed parent", GARB_COLOR, "rgb(0, 0, 255)");
    computed_is(c, "computed: an em is its own font's", GARB_MARGIN_LEFT, "40px");
    computed_is(c, "computed: with no box a percentage width stays one", GARB_WIDTH, "30%");
    computed_is(c, "computed: a number line-height is in pixels", GARB_LINE_HEIGHT, "30px");
    flow_computed_free(c);
    c = flow_computed_new(tree, &p.env, computed_find(root, "deep"), NULL);
    computed_is(c, "computed: two deep inside display:none", GARB_COLOR, "rgb(0, 0, 255)");
    computed_is(c, "computed: its display", GARB_DISPLAY, "inline");
    flow_computed_free(c);
    expect("computed: styling for the asking keeps nothing", live == before, NULL);

    c = flow_computed_new(tree, &p.env, computed_find(root, "pos"), NULL);
    computed_is(c, "computed: a relative box's inset", GARB_TOP, "10px");
    computed_is(c, "computed: left", GARB_LEFT, "5px");
    computed_is(c, "computed: a relative box's unset inset is the other's negation", GARB_RIGHT, "-5px");
    computed_is(c, "computed: and both unset are 0", GARB_BOTTOM, "-10px");
    computed_is(c, "computed: z-index", GARB_Z_INDEX, "3");
    computed_is(c, "computed: opacity", GARB_OPACITY, "0.25");
    computed_is(c, "computed: transform is its matrix", GARB_TRANSFORM, "matrix(2, 0, 0, 2, 10, 20)");
    computed_is(c, "computed: transform-origin in pixels", GARB_TRANSFORM_ORIGIN, "0px 0px");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "grid"), NULL);
    computed_is(c, "computed: grid display", GARB_DISPLAY, "grid");
    computed_is(c, "computed: grid tracks", GARB_GRID_TEMPLATE_COLUMNS, "100px 1fr minmax(10px, auto)");
    computed_is(c, "computed: grid areas", GARB_GRID_TEMPLATE_AREAS, "\"a b .\"");
    computed_is(c, "computed: no rows", GARB_GRID_TEMPLATE_ROWS, "none");
    computed_is(c, "computed: auto rows", GARB_GRID_AUTO_ROWS, "auto");
    computed_is(c, "computed: auto flow", GARB_GRID_AUTO_FLOW, "row");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "face"), NULL);
    computed_is(c, "computed: a family with a space is quoted, its generic after it", GARB_FONT_FAMILY,
                "\"Times New Roman\", serif");
    computed_is(c, "computed: bold is 700", GARB_FONT_WEIGHT, "700");
    computed_is(c, "computed: italic", GARB_FONT_STYLE, "italic");
    computed_is(c, "computed: an inline box's width does not apply", GARB_WIDTH, "auto");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "flt"), NULL);
    computed_is(c, "computed: a float's auto margin is 0", GARB_MARGIN_LEFT, "0px");
    computed_is(c, "computed: border-box width is the border box's", GARB_WIDTH, "10px");
    computed_is(c, "computed: float", GARB_FLOAT, "left");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "shadow"), NULL);
    computed_is(c, "computed: box-shadow, colour first, its spread", GARB_BOX_SHADOW,
                "rgb(1, 2, 3) 2px 3px 0px 0px");
    computed_is(c, "computed: text-shadow in currentcolor", GARB_TEXT_SHADOW, "rgb(9, 9, 9) 1px 1px 2px");
    flow_computed_free(c);

    c = flow_computed_new(tree, &p.env, computed_find(root, "anim"), NULL);
    computed_is(c, "computed: animation-name", GARB_ANIMATION_NAME, "spin");
    computed_is(c, "computed: animation-duration in seconds", GARB_ANIMATION_DURATION, "2s");
    computed_is(c, "computed: a keyword easing", GARB_ANIMATION_TIMING_FUNCTION, "linear");
    computed_is(c, "computed: infinite", GARB_ANIMATION_ITERATION_COUNT, "infinite");
    computed_is(c, "computed: will-change", GARB_WILL_CHANGE, "transform");
    flow_computed_free(c);

    // A node that is no element, and an element no layout of this document
    // reached: no style.
    c = flow_computed_new(tree, &p.env, computed_find(root, "box")->first_child, NULL);
    expect("computed: a text node has no style", c != NULL && !flow_computed_styled(c), NULL);
    computed_is(c, "computed: and reads nothing", GARB_DISPLAY, "");
    flow_computed_free(c);
    int64_t made = 0;
    os64_html_node_t *loose = os64_html_create_element(p.doc, OS64_HTML_NS_HTML, "div", &made);
    c = flow_computed_new(tree, &p.env, loose, NULL);
    expect("computed: a detached element has no style", c != NULL && !flow_computed_styled(c), NULL);
    flow_computed_free(c);
    expect("computed: NULL arguments answer NULL",
           flow_computed_new(NULL, &p.env, loose, NULL) == NULL &&
               flow_computed_new(tree, NULL, loose, NULL) == NULL &&
               flow_computed_new(tree, &p.env, NULL, NULL) == NULL, NULL);
    flow_computed_free(NULL);

    // Every allocation flow_computed_new makes for an element inside
    // display:none fails in turn: NULL, or the whole answer, and nothing
    // kept either way.
    const os64_html_node_t *deep = computed_find(root, "deep");
    allocations = 0;
    c = flow_computed_new(tree, &p.env, deep, NULL);
    size_t count = allocations;
    flow_computed_free(c);
    bool whole = true, kept = false;
    for (size_t at = 1; at <= count; at++) {
        size_t base = live;
        allocations = 0;
        fail_at = at;
        fail_single = true;
        c = flow_computed_new(tree, &p.env, deep, NULL);
        fail_at = 0;
        fail_single = false;
        if (c != NULL) {
            char got[64];
            flow_computed_text(c, GARB_COLOR, got, sizeof(got));
            whole = whole && flow_computed_styled(c) && strcmp(got, "rgb(0, 0, 255)") == 0;
        }
        flow_computed_free(c);
        kept = kept || live != base;
    }
    expect("computed: each allocation failed answers NULL or the whole style", whole && count > 0, NULL);
    expect("computed: and keeps nothing", !kept, NULL);
    printf("libflow computed sweep: each of %zu allocations failed in turn\n", count);
    flow_free(tree);
    sheeted_close(&p);

    // Positioned boxes with auto margins and insets (WPT's computed-style-005
    // and the insets' tests): what the layout placed.
    sheeted_open(&p, "<!doctype html><style>body{margin:0}x{display:block;position:relative;width:60px;"
                     "height:10px}y{display:block;width:40px;height:10px;margin:auto}"
                     "#a{position:absolute;left:0;right:0}#r{position:relative}"
                     "#t{position:absolute;top:7px;width:5px;height:5px;margin:0}</style>"
                     "<x><y id=a></y></x><x><y id=r></y></x><x><y id=t></y></x>", 800, 600);
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    root = p.doc->document;
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "a"), NULL) : NULL;
    computed_is(c, "computed: an absolute box's auto margin is what it left", GARB_MARGIN_LEFT, "10px");
    computed_is(c, "computed: and on the other side", GARB_MARGIN_RIGHT, "10px");
    computed_is(c, "computed: an absolute box's inset as written", GARB_LEFT, "0px");
    computed_is(c, "computed: an absolute box's auto inset is where it was placed", GARB_TOP, "0px");
    flow_computed_free(c);
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "r"), NULL) : NULL;
    computed_is(c, "computed: a relative box's auto margin", GARB_MARGIN_LEFT, "10px");
    computed_is(c, "computed: a relative box with no insets is at 0", GARB_LEFT, "0px");
    computed_is(c, "computed: on every side", GARB_RIGHT, "0px");
    flow_computed_free(c);
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(root, "t"), NULL) : NULL;
    computed_is(c, "computed: an absolute box's top", GARB_TOP, "7px");
    computed_is(c, "computed: its bottom, from its containing block", GARB_BOTTOM, "-2px");
    computed_is(c, "computed: its auto left, its static place", GARB_LEFT, "0px");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);

    // At a zoom the tree is in device pixels; a script reads CSS pixels.
    sheeted_open(&p, "<!doctype html><style>body{margin:0}"
                     "#z{width:200px;height:10px;padding-left:10%;background-image:url(a.png)}</style>"
                     "<div id=z></div>", 400, 300);
    p.env.zoom = 2000;
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    flow_computed_options_t resolve = {computed_url, NULL};
    c = tree != NULL ? flow_computed_new(tree, &p.env, computed_find(p.doc->document, "z"), &resolve)
                     : NULL;
    computed_is(c, "computed: at a zoom, CSS pixels", GARB_WIDTH, "200px");
    computed_is(c, "computed: a percentage of a zoomed containing block", GARB_PADDING_LEFT, "40px");
    computed_is(c, "computed: a sheet's url() resolved as the face fetches it", GARB_BACKGROUND_IMAGE,
                "url(\"http://host/dir/a.png\")");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
}

// Codex's round on slice 1b: a hidden descendant's rem reads the root's
// font size; the CSS-wide keywords on a property libflow does not model
// mean what the cascade gives them; and a declared value of any length is
// written whole.
static void computed_round1_cases(void)
{
    static char page[2048];
    char longname[701];
    memset(longname, 'a', 700);
    longname[700] = '\0';
    snprintf(page, sizeof(page),
             "<!doctype html><style>html{font-size:20px}body{margin:0}#h{display:none}"
             "#r{margin-left:1rem;text-indent:2rem}#p{font-variant:small-caps}#ci{font-variant:initial}"
             "#cu{font-variant:unset}#cn{font-variant:inherit}#cr{font-variant:revert}"
             "#ul{list-style-image:url(b.png)}#ii{list-style-image:initial}#long{list-style-image:url(%s.png)}"
             "</style><div id=h><p id=r>x</p></div><div id=p><span id=ci>a</span><span id=cu>b</span>"
             "<span id=cn>c</span><span id=cr>d</span><span id=plain>e</span></div>"
             "<ul id=ul><li id=ii>a</li><li id=il>b</li></ul><ul id=long><li>c</li></ul>",
             longname);
    Sheeted p;
    sheeted_open(&p, page, 800, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    expect("computed round 1: the page lays out", tree != NULL, NULL);
    if (tree == NULL) {
        sheeted_close(&p);
        return;
    }
    const os64_html_node_t *root = p.doc->document;
    flow_computed_t *c = flow_computed_new(tree, &p.env, computed_find(root, "r"), NULL);
    computed_is(c, "computed: a hidden descendant's rem is the root's font size", GARB_MARGIN_LEFT, "20px");
    computed_is(c, "computed: and so is every rem it has", GARB_TEXT_INDENT, "40px");
    flow_computed_free(c);
    static const struct { const char *id, *want, *what; } kVariant[] = {
        {"ci", "normal", "computed: `initial` is the initial value, not an ancestor's"},
        {"cu", "small-caps", "computed: `unset` on an inherited property inherits"},
        {"cn", "small-caps", "computed: `inherit` inherits"},
        {"cr", "small-caps", "computed: `revert` goes back to the user agent's, which is `unset`"},
        {"plain", "small-caps", "computed: no winner on an inherited property inherits"},
    };
    for (size_t i = 0; i < sizeof(kVariant) / sizeof(kVariant[0]); i++) {
        c = flow_computed_new(tree, &p.env, computed_find(root, kVariant[i].id), NULL);
        computed_is(c, kVariant[i].what, GARB_FONT_VARIANT, kVariant[i].want);
        flow_computed_free(c);
    }
    c = flow_computed_new(tree, &p.env, computed_find(root, "ii"), NULL);
    computed_is(c, "computed: `initial` list-style-image is none", GARB_LIST_STYLE_IMAGE, "none");
    flow_computed_free(c);
    // A declared value past any fixed buffer: whole, at the length it
    // needs, and the same text read again with room.
    const os64_html_node_t *ul = computed_find(root, "long");
    c = ul != NULL ? flow_computed_new(tree, &p.env, ul->first_child, NULL) : NULL;
    size_t need = c != NULL ? flow_computed_text(c, GARB_LIST_STYLE_IMAGE, NULL, 0) : 0;
    char *whole = malloc(need + 1);
    size_t again = whole != NULL ? flow_computed_text(c, GARB_LIST_STYLE_IMAGE, whole, need + 1) : 0;
    char want_tail[16];
    snprintf(want_tail, sizeof(want_tail), "a.png)");
    expect("computed: a long declared value is written whole",
           whole != NULL && need > 700 && again == need && strlen(whole) == need &&
               strstr(whole, longname) != NULL && strcmp(whole + need - strlen(want_tail), want_tail) == 0,
           whole);
    if (c != NULL && need > 0) {
        char cut[64];
        size_t short_read = flow_computed_text(c, GARB_LIST_STYLE_IMAGE, cut, sizeof(cut));
        expect("computed: and a short buffer is told the whole length", short_read == need &&
               strlen(cut) == sizeof(cut) - 1 && strncmp(cut, whole, sizeof(cut) - 1) == 0, cut);
    }
    free(whole);
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
}
