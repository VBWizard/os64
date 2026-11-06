// SHADOW_DOM.md § 6: the cascade over shadow trees — scopes, :host,
// ::slotted(), the context step, inheritance through the flat tree, and
// updates that cross into shadow trees. The trees are made with libhtml's
// verbs (no markup makes a shadow root).

static unsigned shadow_checks, shadow_failures;
static void shadow_check(bool ok, const char *what)
{
    ++shadow_checks;
    if (!ok) { ++shadow_failures; fprintf(stderr, "FAIL cascade shadow: %s\n", what); }
}

// An element by id anywhere in the document's shadow-including tree.
static os64_html_node_t *sh_find(const os64_html_document_t *doc, const char *id)
{
    for (const os64_html_node_t *x = doc->document; x; x = os64_html_shadow_including_next(doc->document, x)) {
        const os64_html_attr_t *a = x->kind == OS64_HTML_ELEMENT ? os64_html_attr(x, "id") : NULL;
        if (a && a->value && !strcmp(a->value, id)) return (os64_html_node_t *)x;
    }
    return NULL;
}

// Attaches a shadow root to `id` and fills it with `html`.
static os64_html_node_t *sh_attach(os64_html_document_t *doc, const char *id, const char *html)
{
    os64_html_node_t *host = sh_find(doc, id);
    os64_html_node_t *root = host ? os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
    os64_html_node_t *frag = root ? os64_html_parse_fragment(doc, host, html, strlen(html), false, NULL) : NULL;
    if (!frag) return NULL;
    while (frag->first_child) os64_html_insert(doc, root, frag->first_child, NULL);
    return root;
}

// The winner of `prop` for `el` as the dump writes it ("" for none).
static const char *sh_win(const garb_cascade_t *c, const os64_html_node_t *el, const char *prop)
{
    static char text[256];
    garb_style_t st = garb_style_for(c, el);
    text[0] = '\0';
    for (int32_t i = 0; i < st.n; i++) {
        if (strcmp(garb_prop_name(st.sets[i].prop), prop)) continue;
        garb_val_dump(&st.sets[i].value, text, sizeof(text));
    }
    return text;
}

static void sh_expect(const garb_cascade_t *c, const os64_html_document_t *doc, const char *id,
                      const char *prop, const char *want, const char *what)
{
    const os64_html_node_t *el = sh_find(doc, id);
    const char *got = el ? sh_win(c, el, prop) : "(no element)";
    bool ok = !strcmp(got, want);
    if (!ok) fprintf(stderr, "  %s: #%s %s is \"%s\", not \"%s\"\n", what, id, prop, got, want);
    shadow_check(ok, what);
}

// The fixture: a host in the page with two light children, one slotted,
// and a shadow tree with a paragraph and a default slot. Sheet 0 is the
// page's, sheet 1 the shadow tree's. The `aside` keeps each change's dirty
// elements under half the page, past which an update asks for a fresh
// cascade instead.
#define SHADOW_PAD "<i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i>"
static const char kShadowPage[] =
    "<!doctype html><body><aside>" SHADOW_PAD SHADOW_PAD SHADOW_PAD "</aside>"
    "<div id=h class=page><span id=lt class=x></span>"
    "<b id=un slot=nowhere></b></div><p id=out class=x></p>";
static const char kShadowTree[] = "<p id=in class=x></p><slot id=sl></slot>";
static const char kPageSheet[] =
    ".x{z-index:1;opacity:.75 !important}.page .x{order:7}"
    "#h{z-index:5;flex-shrink:2 !important;--k:11px}span{width:var(--k)}";
static const char kTreeSheet[] =
    ".x{z-index:2}:host{order:3;z-index:6;flex-shrink:3 !important}"
    ":host(.page){flex-grow:9}:host(.nope){flex-basis:1px}"
    ":host(.page) .x{flex-grow:4}.page .x{flex-basis:2px}"
    "::slotted(span){z-index:9;opacity:.25 !important}"
    "::slotted(.x){order:8}::slotted(span){order:6}"
    "slot{--k:22px}";

typedef struct {
    os64_html_document_t *doc;
    os64_html_node_t *root;
    garb_parsed_t sheets[2];
    garb_sheet_in_t in[2];
    int32_t page_list[1], tree_list[1];
    garb_scope_in_t scopes[2];
} ShadowFixture;

static bool sh_open(ShadowFixture *f)
{
    memset(f, 0, sizeof(*f));
    f->doc = update_doc(kShadowPage);
    f->root = f->doc ? sh_attach(f->doc, "h", kShadowTree) : NULL;
    if (!f->root) return false;
    garb_parse_sheet_text(kPageSheet, strlen(kPageSheet), &f->sheets[0]);
    garb_parse_sheet_text(kTreeSheet, strlen(kTreeSheet), &f->sheets[1]);
    f->in[0] = (garb_sheet_in_t){.sheet = &f->sheets[0], .parent = -1};
    f->in[1] = (garb_sheet_in_t){.sheet = &f->sheets[1], .parent = -1};
    f->page_list[0] = 0;
    f->tree_list[0] = 1;
    f->scopes[0] = (garb_scope_in_t){f->doc->document, f->page_list, 1};
    f->scopes[1] = (garb_scope_in_t){f->root, f->tree_list, 1};
    return true;
}

static void sh_close(ShadowFixture *f)
{
    garb_free(&f->sheets[0]);
    garb_free(&f->sheets[1]);
    os64_html_document_free(f->doc);
}

static garb_cascade_t *sh_cascade(ShadowFixture *f)
{
    return garb_cascade_scoped(f->in, 2, f->scopes, 2, f->doc, (garb_env_t){.width = 800, .height = 600},
                               NULL);
}

// The winners an update left equal the winners a fresh cascade finds.
static void sh_same_as_fresh(garb_cascade_t *c, ShadowFixture *f, const char *what)
{
    garb_cascade_t *fresh = sh_cascade(f);
    static char a[65536], b[65536];
    size_t na = garb_cascade_dump(c, f->doc, a, sizeof(a)), nb = garb_cascade_dump(fresh, f->doc, b, sizeof(b));
    bool ok = fresh && !garb_cascade_incomplete(fresh) && na < sizeof(a) && na == nb && !strcmp(a, b);
    if (!ok) fprintf(stderr, "  %s:\n  updated: %s\n  fresh:   %s\n", what, a, b);
    shadow_check(ok, what);
    garb_cascade_free(fresh);
}

static void shadow_scoping(void)
{
    ShadowFixture f;
    if (!sh_open(&f)) { shadow_check(false, "fixture"); return; }
    garb_cascade_t *c = sh_cascade(&f);
    shadow_check(c && !garb_cascade_incomplete(c), "a scoped cascade builds whole");
    // Each tree's sheets, and only those.
    sh_expect(c, f.doc, "out", "z-index", "1", "the page's sheet styles the page");
    sh_expect(c, f.doc, "in", "z-index", "2", "the tree's sheet styles the tree, the page's does not");
    // A combinator never leaves the tree, but reaches the host, featureless.
    sh_expect(c, f.doc, "in", "order", "", "the page's descendant rule stops at the shadow root");
    sh_expect(c, f.doc, "in", "flex-basis", "", "a tree's `.page .x` cannot see the host's class");
    sh_expect(c, f.doc, "in", "flex-grow", "4", "`:host(.page) .x` climbs to the host");
    // :host and :host(), and the context step on a host.
    sh_expect(c, f.doc, "h", "order", "3", ":host styles the host");
    sh_expect(c, f.doc, "h", "flex-grow", "9", ":host(.page) tests the host's class");
    sh_expect(c, f.doc, "h", "flex-basis", "", ":host(.nope) does not match");
    sh_expect(c, f.doc, "h", "z-index", "5", "the page's normal rule beats :host's");
    sh_expect(c, f.doc, "h", "flex-shrink", "3", ":host's important rule beats the page's");
    // ::slotted(), the context step on a slotted element, and specificity.
    sh_expect(c, f.doc, "lt", "z-index", "1", "the page's normal rule beats ::slotted()'s");
    sh_expect(c, f.doc, "lt", "opacity", "0.25", "::slotted()'s important rule beats the page's");
    sh_expect(c, f.doc, "lt", "order", "7", "the page's rule beats every ::slotted(), whatever its weight");
    sh_expect(c, f.doc, "un", "z-index", "", "::slotted() reaches only an assigned element");
    // Inheritance of a custom property through the flat tree: the slot's.
    sh_expect(c, f.doc, "lt", "width", "22px", "a slotted element takes custom properties from its slot");
    garb_cascade_free(c);

    // Without scopes the page's sheets style the page alone.
    garb_cascade_t *plain = garb_cascade(f.in, 1, f.doc, (garb_env_t){.width = 800, .height = 600});
    sh_expect(plain, f.doc, "out", "z-index", "1", "no scopes: the page is styled");
    sh_expect(plain, f.doc, "in", "z-index", "", "no scopes: no shadow tree is");
    shadow_check(!garb_cascade_matches_scoped(plain, f.in, 1, f.scopes, 1,
                                              (garb_env_t){.width = 800, .height = 600}),
                 "a cascade without scopes does not match scoped input");
    garb_cascade_free(plain);

    // ::slotted()'s weight ranks rules within one tree.
    c = sh_cascade(&f);
    garb_parsed_t weights;
    const char *w = "::slotted(.x){order:8}::slotted(span){order:6}";
    garb_parse_sheet_text(w, strlen(w), &weights);
    garb_sheet_in_t only = {.sheet = &weights, .parent = -1};
    int32_t zero = 0;
    garb_scope_in_t tree = {f.root, &zero, 1};
    garb_cascade_t *weighed = garb_cascade_scoped(&only, 1, &tree, 1, f.doc,
                                                  (garb_env_t){.width = 800, .height = 600}, NULL);
    sh_expect(weighed, f.doc, "lt", "order", "8", "::slotted(.x) outweighs a later ::slotted(span)");
    garb_cascade_free(weighed);
    garb_free(&weights);

    // matches_scoped: the same scopes, another list, another root.
    garb_env_t env = garb_cascade_env(c);
    shadow_check(garb_cascade_matches_scoped(c, f.in, 2, f.scopes, 2, env), "the same scopes match");
    int32_t other[1] = {0};
    garb_scope_in_t changed[2] = {f.scopes[0], {f.root, other, 1}};
    shadow_check(!garb_cascade_matches_scoped(c, f.in, 2, changed, 2, env), "another list does not match");
    shadow_check(!garb_cascade_matches(c, f.in, 2, env), "no scopes do not match a scoped cascade");
    garb_cascade_free(c);
    sh_close(&f);
}

static void shadow_imports(void)
{
    // An imported sheet applies where its importer does, unlisted.
    ShadowFixture f;
    if (!sh_open(&f)) { shadow_check(false, "fixture"); return; }
    garb_parsed_t imported;
    const char *text = "p{z-index:42;max-width:5px}";
    garb_parse_sheet_text(text, strlen(text), &imported);
    garb_import_t via = {.url = "i.css", .len = 5};
    garb_sheet_in_t in[2] = {{.sheet = &imported, .via = &via, .parent = 1}, {.sheet = &f.sheets[1], .parent = -1}};
    int32_t list[1] = {1};
    garb_scope_in_t tree = {f.root, list, 1};
    garb_env_t env = {.width = 800, .height = 600};
    garb_cascade_t *c = garb_cascade_scoped(in, 2, &tree, 1, f.doc, env, NULL);
    sh_expect(c, f.doc, "in", "max-width", "5px", "an unlisted import applies where its importer does");
    sh_expect(c, f.doc, "in", "z-index", "2", "the importer's own rule follows its import's");
    sh_expect(c, f.doc, "out", "z-index", "", "neither applies in the page");
    garb_cascade_free(c);
    // Listed alone in the page, the import applies there and not in the tree.
    int32_t page_list[1] = {0}, none[1] = {7};
    garb_scope_in_t two[2] = {{f.doc->document, page_list, 1}, {f.root, none, 1}};
    c = garb_cascade_scoped(in, 2, two, 2, f.doc, env, NULL);
    sh_expect(c, f.doc, "out", "z-index", "42", "a listed import applies where it is listed");
    sh_expect(c, f.doc, "in", "z-index", "", "an index naming no sheet names nothing");
    garb_cascade_free(c);
    garb_free(&imported);
    sh_close(&f);
}

static void shadow_updates(void)
{
    ShadowFixture f;
    if (!sh_open(&f)) { shadow_check(false, "fixture"); return; }
    garb_cascade_t *c = sh_cascade(&f);
    os64_html_node_t *h = sh_find(f.doc, "h"), *sl = sh_find(f.doc, "sl"), *in = sh_find(f.doc, "in");
    os64_html_node_t *lt = sh_find(f.doc, "lt");

    // The host's class reaches :host() and the tree's `:host(.page) .x`.
    os64_html_set_attr(f.doc, h, "class", "plain", 5);
    shadow_check(garb_cascade_update(c), "a host's class updates incrementally");
    sh_expect(c, f.doc, "in", "flex-grow", "", "the tree's element follows its host's class");
    sh_expect(c, f.doc, "h", "flex-grow", "", ":host(.page) follows the host's class");
    sh_same_as_fresh(c, &f, "after the host's class");

    // A custom property on the slot reaches the element assigned to it.
    os64_html_set_attr(f.doc, sl, "style", "--k:33px", 8);
    shadow_check(garb_cascade_update(c), "a slot's style updates incrementally");
    sh_expect(c, f.doc, "lt", "width", "33px", "the slotted element inherits the slot's new value");
    sh_same_as_fresh(c, &f, "after the slot's style");

    // Renaming the slot unassigns the span: no ::slotted(), no slot to inherit from.
    os64_html_set_attr(f.doc, sl, "name", "other", 5);
    shadow_check(garb_cascade_update(c), "a slot's name updates incrementally");
    sh_expect(c, f.doc, "lt", "opacity", "0.75", "an unassigned element loses ::slotted()");
    sh_same_as_fresh(c, &f, "after the slot's name");
    // The span's own slot attribute assigns it again.
    os64_html_set_attr(f.doc, lt, "slot", "other", 5);
    shadow_check(garb_cascade_update(c), "a child's slot attribute updates incrementally");
    sh_expect(c, f.doc, "lt", "opacity", "0.25", "the reassigned element has ::slotted() again");
    sh_same_as_fresh(c, &f, "after the child's slot attribute");

    // An element added inside the shadow tree is styled by the tree's sheet.
    os64_html_node_t *em = os64_html_create_element(f.doc, OS64_HTML_NS_HTML, "em", NULL);
    os64_html_set_attr(f.doc, em, "id", "added", 5);
    os64_html_set_attr(f.doc, em, "class", "x", 1);
    os64_html_insert(f.doc, f.root, em, NULL);
    shadow_check(garb_cascade_update(c), "an element added to a shadow tree updates incrementally");
    sh_expect(c, f.doc, "added", "z-index", "2", "the added element takes the tree's sheet");
    sh_same_as_fresh(c, &f, "after an addition inside the tree");

    // A slot removed from the tree: its child is no longer slotted. A slot
    // leaving asks for a fresh cascade (the tree it left may be gone by the
    // time the log is read).
    os64_html_remove(f.doc, sl);
    shadow_check(!garb_cascade_update(c), "a slot leaving asks for a fresh cascade");
    garb_cascade_free(c);
    c = sh_cascade(&f);
    sh_expect(c, f.doc, "lt", "opacity", "0.75", "with its slot gone the element is not slotted");
    // A slot arriving assigns the element again, incrementally.
    os64_html_node_t *again = os64_html_create_element(f.doc, OS64_HTML_NS_HTML, "slot", NULL);
    os64_html_set_attr(f.doc, again, "name", "other", 5);
    os64_html_insert(f.doc, f.root, again, NULL);
    shadow_check(garb_cascade_update(c), "a slot arriving updates incrementally");
    sh_expect(c, f.doc, "lt", "opacity", "0.25", "a slot arriving takes its element");
    sh_same_as_fresh(c, &f, "after a slot's arrival");

    // A second host attached after the build: an update or a fresh cascade,
    // either way the same winners.
    os64_html_node_t *out = sh_find(f.doc, "out");
    os64_html_node_t *root2 = os64_html_attach_shadow(f.doc, out, OS64_HTML_SHADOW_OPEN, NULL);
    shadow_check(root2 != NULL, "a second root attaches");
    if (!garb_cascade_update(c)) {
        garb_cascade_free(c);
        c = sh_cascade(&f);
    }
    sh_same_as_fresh(c, &f, "after a second root");
    (void)in;
    garb_cascade_free(c);
    sh_close(&f);
}

// A cascade built before any shadow root asks for a fresh one once the
// document has one: every walk now crosses into shadow trees.
static void shadow_first_root(void)
{
    os64_html_document_t *doc = update_doc(kShadowPage);
    garb_parsed_t sheet;
    garb_parse_sheet_text(kPageSheet, strlen(kPageSheet), &sheet);
    garb_sheet_in_t in = {.sheet = &sheet, .parent = -1};
    garb_cascade_t *c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
    shadow_check(c && !os64_html_has_shadow_roots(doc), "a page with no shadow root cascades");
    shadow_check(sh_attach(doc, "h", kShadowTree) != NULL, "a root attaches after the cascade");
    shadow_check(!garb_cascade_update(c), "the first shadow root asks for a fresh cascade");
    garb_cascade_free(c);
    garb_free(&sheet);
    os64_html_document_free(doc);
}

// The context step follows the trees' roots in shadow-including order, not
// their depth: a slotted host's slot tree and its own shadow tree stand at
// one depth, and the slot tree comes first.
static void shadow_contexts(void)
{
    garb_env_t env = {.width = 800, .height = 600};
    // A slotted host: ::slotted() from its parent host's tree, :host from its own.
    os64_html_document_t *doc = update_doc("<!doctype html><body><div id=outer><span id=inner></span></div>");
    os64_html_node_t *outer = sh_attach(doc, "outer", "<slot></slot>");
    os64_html_node_t *inner = sh_attach(doc, "inner", "<p></p>");
    garb_parsed_t sheets[2];
    const char *slotted = "::slotted(*){z-index:1;order:1 !important}";
    const char *host = ":host{z-index:2;order:2 !important}";
    garb_parse_sheet_text(slotted, strlen(slotted), &sheets[0]);
    garb_parse_sheet_text(host, strlen(host), &sheets[1]);
    garb_sheet_in_t in[2] = {{.sheet = &sheets[0], .parent = -1}, {.sheet = &sheets[1], .parent = -1}};
    int32_t zero = 0, one = 1;
    garb_scope_in_t scopes[2] = {{outer, &zero, 1}, {inner, &one, 1}};
    garb_cascade_t *c = outer && inner ? garb_cascade_scoped(in, 2, scopes, 2, doc, env, NULL) : NULL;
    sh_expect(c, doc, "inner", "z-index", "1", "a slot's tree's normal rule beats the host's own tree's");
    sh_expect(c, doc, "inner", "order", "2", "the host's own tree's important rule beats a slot's tree's");
    garb_cascade_free(c);
    garb_free(&sheets[0]);
    garb_free(&sheets[1]);
    os64_html_document_free(doc);

    // A forwarding chain, one sheet applying in both trees on it: counted in
    // each, so its important rule from the inner tree wins.
    doc = update_doc("<!doctype html><body><div id=outer><b id=x></b></div>");
    outer = sh_attach(doc, "outer", "<div id=mid><slot></slot></div>");
    os64_html_node_t *mid = outer ? sh_attach(doc, "mid", "<slot></slot>") : NULL;
    const char *both = "::slotted(*){z-index:1 !important;order:5}";
    const char *first = "::slotted(*){z-index:2 !important;order:6}";
    garb_parse_sheet_text(both, strlen(both), &sheets[0]);
    garb_parse_sheet_text(first, strlen(first), &sheets[1]);
    in[0] = (garb_sheet_in_t){.sheet = &sheets[0], .parent = -1};
    in[1] = (garb_sheet_in_t){.sheet = &sheets[1], .parent = -1};
    int32_t outer_list[2] = {0, 1};
    scopes[0] = (garb_scope_in_t){outer, outer_list, 2};
    scopes[1] = (garb_scope_in_t){mid, &zero, 1};
    c = mid ? garb_cascade_scoped(in, 2, scopes, 2, doc, env, NULL) : NULL;
    sh_expect(c, doc, "x", "z-index", "1", "a shared sheet's important rule counts in the inner tree too");
    sh_expect(c, doc, "x", "order", "6", "along a chain the outer slot's tree's normal rules win");
    garb_cascade_free(c);
    garb_free(&sheets[0]);
    garb_free(&sheets[1]);
    os64_html_document_free(doc);
}

// One batch removes a slot from its wrapper and then the wrapper: by the
// time the log is read the tree the slot left is past finding.
static void shadow_batch_removal(void)
{
    ShadowFixture f;
    if (!sh_open(&f)) { shadow_check(false, "fixture"); return; }
    os64_html_node_t *w = os64_html_create_element(f.doc, OS64_HTML_NS_HTML, "div", NULL);
    os64_html_insert(f.doc, f.root, w, NULL);
    os64_html_node_t *sl = sh_find(f.doc, "sl");
    os64_html_insert(f.doc, w, sl, NULL);
    garb_cascade_t *c = sh_cascade(&f);
    sh_expect(c, f.doc, "lt", "opacity", "0.25", "the wrapped slot takes the element");
    os64_html_remove(f.doc, sl);
    os64_html_remove(f.doc, w);
    bool updated = garb_cascade_update(c);
    shadow_check(!updated, "a slot that left a removed wrapper asks for a fresh cascade");
    if (!updated) {
        garb_cascade_free(c);
        c = sh_cascade(&f);
    }
    sh_same_as_fresh(c, &f, "after a slot and its wrapper leave in one batch");
    garb_cascade_free(c);
    sh_close(&f);
}

static int cascade_shadow(void)
{
    shadow_scoping();
    shadow_imports();
    shadow_updates();
    shadow_first_root();
    shadow_contexts();
    shadow_batch_removal();
    printf("libgarb shadow: %u checks, %u failed\n", shadow_checks, shadow_failures);
    return shadow_failures != 0;
}
