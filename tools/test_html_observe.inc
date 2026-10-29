// test_html_observe.inc — the listener and custom state (html.h, THE
// LISTENER and CUSTOM STATE; CUSTOM_ELEMENTS.md), included by
// test_html_dom_host.c.
//
// The listener copies every record as it is told: its runs walked into
// arrays while they lie where the record says, its old bytes copied while
// they are still borrowed. Two oracles read the copies. The shapes the
// standard names are asserted record by record. And a MIRROR, a second tree
// rebuilt from nothing but the CHILDREN records, must come out as the real
// tree after the verbs and after the parser on both of its link paths: a
// link nobody was told about leaves the mirror behind.

#define CHILDREN OS64_HTML_CHANGE_CHILDREN
#define RUN_MAX 64

typedef struct {
    os64_html_change_t c;
    const HNode *added[RUN_MAX], *removed[RUN_MAX];
    char old[128];
    size_t old_len;
    bool old_null;
    char name[64], local[64], ns[128]; // ATTR: borrowed for the call, so copied
    bool ns_null;
} Told;
static Told told[512];
static size_t ntold, told_dropped;
static os64_html_document_t *told_doc;
// The listener holds every node it is told was removed, as libdom's
// records will, so a removal it heard of cannot be reclaimed under it.
static bool told_hold;
static const HNode *held_by_listener[64];
static size_t nheld_by_listener;

static void listen(void *opaque, const os64_html_change_t *c)
{
    check(opaque == told_doc, "listener: told with its own opaque");
    check(c->version == os64_html_version(told_doc), "listener: the record's version is the document's");
    check(c->kind == CHILDREN || c->kind == OS64_HTML_CHANGE_ATTR || c->kind == OS64_HTML_CHANGE_TEXT,
          "listener: never told of a state change");
    if (ntold == H_ARRAY(told)) {
        told_dropped++;
        return;
    }
    Told *t = &told[ntold++];
    memset(t, 0, sizeof(*t));
    t->c = *c;
    if (c->kind == CHILDREN) {
        check(c->nadded <= RUN_MAX && c->nremoved <= RUN_MAX && (c->nadded || c->nremoved) &&
                  !c->added == !c->nadded && !c->removed == !c->nremoved,
              "listener: a children record has a run, and each run is its count");
        const HNode *n = c->added;
        for (size_t i = 0; i < c->nadded && i < RUN_MAX; i++, n = n ? n->next : NULL) {
            check(n && n->parent == c->target, "listener: added node %zu lies in the target", i);
            t->added[i] = n;
        }
        n = c->removed;
        for (size_t i = 0; i < c->nremoved && i < RUN_MAX; i++, n = n ? n->next : NULL) {
            check(n && n->kind <= OS64_HTML_PI, "listener: removed node %zu is alive", i);
            t->removed[i] = n;
            if (told_hold && n && nheld_by_listener < H_ARRAY(held_by_listener)) {
                os64_html_hold(told_doc, n);
                held_by_listener[nheld_by_listener++] = n;
            }
        }
        return;
    }
    const char *old = c->kind == OS64_HTML_CHANGE_ATTR ? c->attr_old : c->text_old;
    size_t len = c->kind == OS64_HTML_CHANGE_ATTR ? c->attr_old_len : c->text_old_len;
    t->old_null = old == NULL;
    t->old_len = len;
    if (old)
        memcpy(t->old, old, len < sizeof(t->old) ? len : sizeof(t->old) - 1);
    if (c->kind != OS64_HTML_CHANGE_ATTR)
        return;
    check(c->attr_name && c->attr_local, "listener: an attribute record names the attribute");
    snprintf(t->name, sizeof(t->name), "%s", c->attr_name ? c->attr_name : "");
    snprintf(t->local, sizeof(t->local), "%s", c->attr_local ? c->attr_local : "");
    snprintf(t->ns, sizeof(t->ns), "%s", c->attr_ns ? c->attr_ns : "");
    t->ns_null = c->attr_ns == NULL;
}
static void listen_to(os64_html_document_t *doc)
{
    told_doc = doc;
    ntold = told_dropped = 0;
    os64_html_observe(doc, listen, doc);
}
static void release_listener_holds(void)
{
    while (nheld_by_listener)
        os64_html_release(told_doc, held_by_listener[--nheld_by_listener]);
}
static bool told_old(const Told *t, const char *want)
{
    if (!want)
        return t->old_null;
    return !t->old_null && t->old_len == strlen(want) && memcmp(t->old, want, t->old_len) == 0;
}
static bool told_children(const Told *t, const HNode *target, const HNode *added, size_t nadded,
                          const HNode *removed, size_t nremoved, const HNode *prev, const HNode *next)
{
    return t->c.kind == CHILDREN && t->c.target == target && t->c.added == added &&
           t->c.nadded == nadded && t->c.removed == removed && t->c.nremoved == nremoved &&
           t->c.previous_sibling == prev && t->c.next_sibling == next;
}

// ── The mirror ──────────────────────────────────────────────────────────

typedef struct {
    const HNode *node;
    const HNode *kids[256];
    size_t n;
} Mirror;
static Mirror *mirror;
static size_t nmirror;
static Mirror *m_of(const HNode *n)
{
    for (size_t i = 0; i < nmirror; i++)
        if (mirror[i].node == n)
            return &mirror[i];
    if (nmirror == 2048) {
        check(false, "mirror: more parents than the test keeps");
        return &mirror[0];
    }
    Mirror *m = &mirror[nmirror++];
    m->node = n;
    m->n = 0;
    return m;
}
// The mirror starts as the tree stands, or empty for a parse.
static void m_import(const HNode *n)
{
    Mirror *m = m_of(n);
    for (const HNode *c = n->first_child; c && m->n < H_ARRAY(m->kids); c = c->next) {
        m->kids[m->n++] = c;
        m_import(c);
    }
    if (n->template_contents)
        m_import(n->template_contents);
}
static void m_start(const HNode *root)
{
    if (!mirror)
        mirror = calloc(2048, sizeof(*mirror));
    nmirror = 0;
    if (root)
        m_import(root);
}
// A record is applied as the standard defines it: the removed nodes leave
// the target, the added ones go in before the next sibling it names. The
// previous sibling is not used, since the standard names it before a move.
static void m_apply(const Told *t)
{
    if (t->c.kind != CHILDREN)
        return;
    Mirror *m = m_of(t->c.target);
    for (size_t r = 0; r < t->c.nremoved; r++) {
        size_t at = 0;
        while (at < m->n && m->kids[at] != t->removed[r])
            at++;
        check(at < m->n, "mirror: a removed node was the target's child");
        if (at < m->n) {
            memmove(&m->kids[at], &m->kids[at + 1], (m->n - at - 1) * sizeof(m->kids[0]));
            m->n--;
        }
    }
    size_t at = m->n;
    if (t->c.next_sibling) {
        at = 0;
        while (at < m->n && m->kids[at] != t->c.next_sibling)
            at++;
        check(at < m->n, "mirror: the next sibling is the target's child");
    }
    if (m->n + t->c.nadded > H_ARRAY(m->kids)) {
        check(false, "mirror: more children than the test keeps");
        return;
    }
    memmove(&m->kids[at + t->c.nadded], &m->kids[at], (m->n - at) * sizeof(m->kids[0]));
    for (size_t a = 0; a < t->c.nadded; a++)
        m->kids[at + a] = t->added[a];
    m->n += t->c.nadded;
}
static void m_apply_told(void)
{
    for (size_t i = 0; i < ntold; i++)
        m_apply(&told[i]);
}
static bool m_matches(const HNode *n)
{
    Mirror *m = m_of(n);
    size_t i = 0;
    for (const HNode *c = n->first_child; c; c = c->next, i++)
        if (i >= m->n || m->kids[i] != c || !m_matches(c))
            return false;
    return i == m->n && (!n->template_contents || m_matches(n->template_contents));
}

// ── The verbs ───────────────────────────────────────────────────────────

static void t_observe_children(void)
{
    os64_html_document_t *doc =
        parse("<body><div id=a><i id=i1></i><i id=i2></i></div><div id=b><u id=u></u></div>");
    HNode *a = id_in(doc, "a"), *b = id_in(doc, "b"), *i1 = id_in(doc, "i1"), *i2 = id_in(doc, "i2"),
          *u = id_in(doc, "u");
    m_start(doc->document);
    listen_to(doc);

    // A move is two records: out of the old parent, then in.
    uint64_t version = os64_html_version(doc);
    check(os64_html_insert(doc, a, u, i2) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], b, NULL, 0, u, 1, NULL, NULL) &&
              told_children(&told[1], a, u, 1, NULL, 0, i1, i2) && os64_html_version(doc) > version,
          "a move: its removal from the old parent, then its insertion");
    m_apply_told();

    // A fragment drained is two records: the fragment emptied, then the
    // parent filled, both naming the same run.
    HNode *frag = os64_html_create_fragment(doc, NULL), *t1 = os64_html_create_text(doc, "x", 1, NULL),
          *em = os64_html_create_element(doc, OS64_HTML_NS_HTML, "em", NULL);
    ntold = 0;
    check(os64_html_insert(doc, frag, t1, NULL) == OS64_HTML_OK &&
              os64_html_insert(doc, frag, em, NULL) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], frag, t1, 1, NULL, 0, NULL, NULL) &&
              told_children(&told[1], frag, em, 1, NULL, 0, t1, NULL),
          "a detached fragment's own insertions are told");
    m_apply_told();
    ntold = 0;
    check(os64_html_insert(doc, b, frag, NULL) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], frag, NULL, 0, t1, 2, NULL, NULL) &&
              told_children(&told[1], b, t1, 2, NULL, 0, NULL, NULL) && told[0].removed[1] == em &&
              told[1].added[1] == em,
          "a fragment drained: the fragment's record, then the parent's, one run of two");
    m_apply_told();

    // A replace is one record on the parent; a node that had a parent of
    // its own adds its removal first, naming its siblings once `old` is gone.
    ntold = 0;
    check(os64_html_replace(doc, a, em, i1) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], b, NULL, 0, em, 1, t1, NULL) &&
              told_children(&told[1], a, em, 1, i1, 1, NULL, u),
          "a replace by a node from elsewhere: its removal, then one record with both lists");
    m_apply_told();
    // The node `old` stood beside: it leaves from between `old`'s
    // neighbours, since `old` has gone first.
    HNode *left = os64_html_create_element(doc, OS64_HTML_NS_HTML, "q", NULL),
          *old = os64_html_create_element(doc, OS64_HTML_NS_HTML, "q", NULL),
          *beside = os64_html_create_element(doc, OS64_HTML_NS_HTML, "q", NULL),
          *right = os64_html_create_element(doc, OS64_HTML_NS_HTML, "q", NULL);
    HNode *row[] = {left, old, beside, right};
    ntold = 0;
    for (size_t i = 0; i < H_ARRAY(row); i++)
        os64_html_insert(doc, b, row[i], NULL);
    m_apply_told();
    ntold = 0;
    check(os64_html_replace(doc, b, beside, old) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], b, NULL, 0, beside, 1, left, right) &&
              told_children(&told[1], b, beside, 1, old, 1, left, right),
          "a replace by the node beside `old`: it leaves from between `old`'s neighbours");
    m_apply_told();
    HNode *fresh = os64_html_create_element(doc, OS64_HTML_NS_HTML, "s", NULL);
    ntold = 0;
    check(os64_html_replace(doc, a, fresh, u) == OS64_HTML_OK && ntold == 1 &&
              told_children(&told[0], a, fresh, 1, u, 1, em, i2),
          "a replace by a detached node: one record");
    m_apply_told();

    // A replace by itself moves nothing and is still one record.
    ntold = 0;
    version = os64_html_version(doc);
    check(os64_html_replace(doc, a, fresh, fresh) == OS64_HTML_OK && ntold == 1 &&
              told_children(&told[0], a, fresh, 1, fresh, 1, em, i2) && os64_html_version(doc) == version,
          "a node replaced by itself: one record, and the version stays");
    m_apply_told();

    // An insert where the node already sits: out and back in, and the
    // version stays. The standard names the previous sibling before the
    // node leaves, which is the node itself when it is the last child.
    ntold = 0;
    check(os64_html_insert(doc, a, i2, NULL) == OS64_HTML_OK && ntold == 2 &&
              told_children(&told[0], a, NULL, 0, i2, 1, fresh, NULL) &&
              told_children(&told[1], a, i2, 1, NULL, 0, i2, NULL) && os64_html_version(doc) == version,
          "a node inserted where it sits: its removal and its insertion, the version still");
    m_apply_told();

    // What is not a change is not told.
    HNode *empty = os64_html_create_fragment(doc, NULL), *loose = os64_html_create_comment(doc, "c", 1, NULL);
    ntold = 0;
    check(os64_html_insert(doc, b, empty, NULL) == OS64_HTML_OK && os64_html_remove(doc, loose) == OS64_HTML_OK &&
              os64_html_insert(doc, a, a, NULL) == OS64_HTML_HIERARCHY && ntold == 0,
          "an empty fragment, removing a detached node, and a refusal tell nothing");

    // A remove is one record, and a listener that holds what it was told
    // was removed keeps it from being reclaimed.
    told_hold = true;
    size_t nodes = doc->node_count;
    check(os64_html_remove(doc, i2) == OS64_HTML_OK && ntold == 1 &&
              told_children(&told[0], a, NULL, 0, i2, 1, fresh, NULL) && doc->node_count == nodes &&
              i2->kind == OS64_HTML_ELEMENT && strcmp(i2->name, "i") == 0,
          "a remove: one record, and the node the listener holds is still there");
    told_hold = false;
    m_apply_told();
    release_listener_holds();
    check(doc->node_count == nodes - 1, "the listener's hold let go: the removed node is reclaimed");

    check(m_matches(doc->document), "the mirror rebuilt from the records is the tree");
    check(told_dropped == 0, "the listener kept every record");

    // No listener, nothing told.
    os64_html_observe(doc, NULL, doc);
    ntold = 0;
    check(os64_html_remove(doc, t1) == OS64_HTML_OK && ntold == 0, "a listener taken away hears nothing");
    os64_html_document_free(doc);
}

static void t_observe_attributes(void)
{
    os64_html_document_t *doc = parse("<body><p id=p class=x><svg><a id=s xlink:href=h></a></svg>");
    HNode *p = id_in(doc, "p"), *s = id_in(doc, "s"), *text = NULL;
    const HAttr *href = os64_html_attr(s, "xlink:href");
    char xlink[64] = "";
    check(href && href->ns, "the parser gives xlink:href its namespace");
    if (href && href->ns)
        snprintf(xlink, sizeof(xlink), "%s", href->ns);
    listen_to(doc);

    uint64_t version = os64_html_version(doc);
    check(os64_html_set_attr(doc, p, "class", "x", 1) == OS64_HTML_OK && ntold == 1 &&
              told[0].c.kind == OS64_HTML_CHANGE_ATTR && told[0].c.target == p && told_old(&told[0], "x") &&
              told[0].ns_null && strcmp(told[0].local, "class") == 0 &&
              os64_html_version(doc) == version,
          "an attribute set to its own value is told, and the version stays");
    ntold = 0;
    check(os64_html_set_attr(doc, p, "class", "y", 1) == OS64_HTML_OK && ntold == 1 &&
              told_old(&told[0], "x") && os64_html_version(doc) > version,
          "a changed value: its old value");
    ntold = 0;
    check(os64_html_set_attr(doc, p, "title", "t", 1) == OS64_HTML_OK && ntold == 1 &&
              told_old(&told[0], NULL) && strcmp(told[0].name, "title") == 0,
          "a new attribute: no old value");
    ntold = 0;
    check(os64_html_remove_attr(doc, p, "title") == OS64_HTML_OK && ntold == 1 && told_old(&told[0], "t") &&
              os64_html_remove_attr(doc, p, "title") == OS64_HTML_OK && ntold == 1,
          "a removal: its old value; removing an absent attribute is not told");
    ntold = 0;
    check(os64_html_set_attr(doc, s, "xlink:href", "k", 1) == OS64_HTML_OK && ntold == 1 &&
              told_old(&told[0], "h") && !told[0].ns_null && strcmp(told[0].ns, xlink) == 0 &&
              strcmp(told[0].local, "href") == 0 && strcmp(told[0].name, "xlink:href") == 0,
          "a namespaced attribute: its namespace and its local name");
    ntold = 0;
    check(os64_html_set_attr(doc, p, "a:b", "v", 1) == OS64_HTML_OK && ntold == 1 && told[0].ns_null &&
              strcmp(told[0].local, "a:b") == 0,
          "a colon in no namespace is part of the local name");
    ntold = 0;
    check(os64_html_set_attr(doc, p, "class", "\xff", 1) == OS64_HTML_BAD_TEXT && ntold == 0,
          "a refused set is not told");

    // A batch is one record per change as applied in order.
    const os64_html_attr_change_t batch[] = {
        {"class", "z", 1, false}, {"class", NULL, 0, true}, {"class", "w", 1, false}, {"nope", NULL, 0, true}};
    ntold = 0;
    check(os64_html_set_attrs(doc, p, batch, 4) == OS64_HTML_OK && ntold == 3 && told_old(&told[0], "y") &&
              told_old(&told[1], "z") && told_old(&told[2], NULL),
          "a batch: each change's old value as the changes before it left it");
    const os64_html_attr_change_t same[] = {{"class", "w", 1, false}},
                                  there_and_back[] = {{"class", "q", 1, false}, {"class", "w", 1, false}};
    ntold = 0;
    version = os64_html_version(doc);
    check(os64_html_set_attrs(doc, p, same, 1) == OS64_HTML_OK && ntold == 1 && told_old(&told[0], "w") &&
              os64_html_set_attrs(doc, p, there_and_back, 2) == OS64_HTML_OK && ntold == 3 &&
              told_old(&told[1], "w") && told_old(&told[2], "q") && os64_html_version(doc) == version,
          "a batch that changes nothing in the end is told change by change, and the version stays");
    const os64_html_attr_change_t renew[] = {{"xlink:href", NULL, 0, true}, {"xlink:href", "m", 1, false}};
    ntold = 0;
    href = NULL;
    check(os64_html_set_attrs(doc, s, renew, 2) == OS64_HTML_OK && ntold == 2 && told_old(&told[0], "k") &&
              !told[0].ns_null && strcmp(told[0].ns, xlink) == 0 && told_old(&told[1], NULL) &&
              told[1].ns_null &&
              (href = os64_html_attr(s, "xlink:href")) && !href->ns,
          "a name removed and set again comes back in no namespace, as the tree has it");

    // Text, replaced or set to what it was.
    for (HNode *n = p->first_child; n; n = n->next)
        if (n->kind == OS64_HTML_TEXT)
            text = n;
    if (!text) {
        text = os64_html_create_text(doc, "one", 3, NULL);
        os64_html_insert(doc, p, text, NULL);
    }
    os64_html_set_text(doc, text, "one", 3);
    ntold = 0;
    version = os64_html_version(doc);
    check(os64_html_set_text(doc, text, "one", 3) == OS64_HTML_OK && ntold == 1 &&
              told[0].c.kind == OS64_HTML_CHANGE_TEXT && told_old(&told[0], "one") &&
              os64_html_version(doc) == version,
          "a text set to itself is told, and the version stays");
    ntold = 0;
    check(os64_html_set_text(doc, text, "two!", 4) == OS64_HTML_OK && ntold == 1 && told_old(&told[0], "one") &&
              os64_html_version(doc) > version,
          "a text replaced: the old text");
    // With a pin, the old bytes are retired rather than freed, and are the
    // same bytes either way.
    os64_html_pin_t pin = os64_html_pin(doc);
    ntold = 0;
    check(os64_html_set_text(doc, text, "three", 5) == OS64_HTML_OK && ntold == 1 && told_old(&told[0], "two!"),
          "a text replaced under a pin: the old text");
    os64_html_unpin(doc, pin);
    check(told_dropped == 0, "the listener kept every record");
    os64_html_document_free(doc);
}

// ── The parser ──────────────────────────────────────────────────────────

// Every TEXT record the parser made is an append: what it names as the
// old text is a shorter prefix of the text the node ends with.
static bool texts_are_appends(size_t *appends)
{
    *appends = 0;
    for (size_t i = 0; i < ntold; i++) {
        const Told *t = &told[i];
        if (t->c.kind != OS64_HTML_CHANGE_TEXT)
            continue;
        ++*appends;
        if (t->old_null || t->old_len >= t->c.target->text_len ||
            memcmp(t->old, t->c.target->text, t->old_len < sizeof(t->old) ? t->old_len : sizeof(t->old) - 1))
            return false;
    }
    return true;
}
static size_t removals_told(void)
{
    size_t n = 0;
    for (size_t i = 0; i < ntold; i++)
        n += told[i].c.kind == CHILDREN && told[i].c.nremoved;
    return n;
}
static os64_html_parser_t *parser_listened(void)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    os64_html_document_t *doc = os64_html_parser_document(p);
    m_start(doc->document);
    listen_to(doc);
    return p;
}
static void feed(os64_html_parser_t *p, const char *s)
{
    os64_html_parser_feed(p, s, strlen(s));
}
// The parser reads its first 1024 bytes for an encoding before it parses
// any, so a fixture that streams fills that window with a comment first.
static void feed_past_window(os64_html_parser_t *p)
{
    char pad[1100];
    memset(pad, 'x', sizeof(pad));
    memcpy(pad, "<!--", 4);
    memcpy(pad + sizeof(pad) - 4, "-->", 4);
    feed(p, pad);
}

static void t_observe_parser(void)
{
    // The tree as the parser built it, undisturbed: its formatting
    // elements are rebuilt by the adoption agency, which moves nodes.
    // Fed in pieces, so that some characters land on a text node that is
    // already there.
    static const char *const pieces[] = {"<!doctype html><title>t</title><body><di", "v id=d>on",
                                         "e<b>x<p>y</b>z</p><template><i>t</i></template><tab",
                                         "le><tr><td>c</table>tail"};
    os64_html_parser_t *p = parser_listened();
    feed_past_window(p);
    for (size_t i = 0; i < H_ARRAY(pieces); i++)
        feed(p, pieces[i]);
    os64_html_document_t *doc = os64_html_parser_finish(p);
    size_t appends = 0;
    m_apply_told();
    check(!((HDoc *)doc)->disturbed && m_matches(doc->document),
          "the parser, undisturbed: the mirror rebuilt from its records is its tree");
    check(texts_are_appends(&appends) && appends >= 2 && removals_told() >= 1,
          "the parser, undisturbed: its appends are text records, and its moves are told");
    check(told_dropped == 0, "the listener kept every record");
    os64_html_document_free(doc);

    // The same once a verb has moved a node between two calls: the parser
    // then links through the verbs' rules (d_place), and is told the same.
    p = parser_listened();
    feed_past_window(p);
    feed(p, "<body><div id=d>one");
    doc = os64_html_parser_document(p);
    HNode *moved = os64_html_create_element(doc, OS64_HTML_NS_HTML, "aside", NULL);
    check(os64_html_insert(doc, doc->body, moved, NULL) == OS64_HTML_OK && ((HDoc *)doc)->disturbed,
          "a verb between two calls disturbs the parse");
    feed(p, "two<b>x<p>y</b>z</p><span>s</span>");
    doc = os64_html_parser_finish(p);
    m_apply_told();
    check(m_matches(doc->document), "the parser, disturbed: the mirror rebuilt from its records is its tree");
    check(texts_are_appends(&appends) && appends >= 2 && removals_told() >= 1,
          "the parser, disturbed: its appends and its moves are told");
    os64_html_document_free(doc);

    // A frameset after a body the parser made itself takes the body out: the
    // parser's one removal that is not a move. (A `<body>` tag of the page's
    // own would have ruled the frameset out.)
    p = parser_listened();
    feed_past_window(p);
    feed(p, "<div><frameset><frame></frameset>");
    doc = os64_html_parser_finish(p);
    m_apply_told();
    check(m_matches(doc->document) && removals_told() >= 1,
          "the parser takes the body out for a frameset: told, and the mirror is its tree");
    os64_html_document_free(doc);

    // A second body tag gives the body the attributes it lacks, one record each.
    p = parser_listened();
    feed(p, "<body a=1><body a=2 b=3>");
    doc = os64_html_parser_finish(p);
    size_t attrs = 0;
    for (size_t i = 0; i < ntold; i++)
        if (told[i].c.kind == OS64_HTML_CHANGE_ATTR) {
            attrs++;
            check(told[i].c.target == doc->body && strcmp(told[i].name, "b") == 0 &&
                      told_old(&told[i], NULL),
                  "a second body tag: the attribute it adds, which was absent");
        }
    check(attrs == 1, "a second body tag: one attribute record");
    os64_html_document_free(doc);

    // An abandoned parse is given its html element, and that is told.
    p = parser_listened();
    feed(p, "<!--before-->");
    doc = os64_html_parser_abandon(p);
    check(ntold >= 1 && told_children(&told[ntold - 1], doc->document, doc->html, 1, NULL, 0,
                                      doc->html->prev, NULL),
          "the html element a parse is given at its end is told");
    m_apply_told();
    check(m_matches(doc->document), "an abandoned parse: the mirror is its tree");
    os64_html_document_free(doc);

    // A fragment parse builds in its own tree and tells nothing; nor does a
    // clone, which nothing can be listening to while it is built.
    doc = parse("<body><div id=d><p>x</p></div>");
    listen_to(doc);
    int64_t st = 99;
    uint64_t version = os64_html_version(doc);
    HNode *frag = os64_html_parse_fragment(doc, id_in(doc, "d"), "<b>1</b>2<i>3</i>", 17, true, &st);
    HNode *copy = os64_html_clone(doc, id_in(doc, "d"), true, NULL);
    check(frag && st == OS64_HTML_OK && copy && ntold == 0 && os64_html_version(doc) == version,
          "a fragment parse and a deep clone tell nothing");
    // What the fragment holds reaches the document by a verb, which tells.
    check(os64_html_insert(doc, doc->body, frag, NULL) == OS64_HTML_OK && ntold == 2 &&
              told[1].c.target == doc->body && told[1].c.nadded == 3,
          "a fragment's children inserted are told, as one run");
    os64_html_document_free(doc);
}

// ── Custom state, and the name it is read with ──────────────────────────

static void t_custom_state(void)
{
    os64_html_document_t *doc = parse("<body><x-a id=x>t</x-a>"), *other = parse("<body><p id=p>");
    HNode *x = id_in(doc, "x");
    listen_to(doc);
    check(x->ce == OS64_HTML_CE_PLAIN, "an element is born PLAIN");
    uint64_t version = os64_html_version(doc);
    check(os64_html_set_ce(doc, x, OS64_HTML_CE_CUSTOM) == OS64_HTML_OK && x->ce == OS64_HTML_CE_CUSTOM &&
              os64_html_version(doc) > version && ntold == 0,
          "set_ce: the state, the version moved, and no listener told");
    version = os64_html_version(doc);
    check(os64_html_set_ce(doc, x, OS64_HTML_CE_CUSTOM) == OS64_HTML_OK && os64_html_version(doc) == version,
          "set_ce to the state it has: the version stays");
    HNode *copy = os64_html_clone(doc, x, true, NULL);
    check(copy && copy->ce == OS64_HTML_CE_PLAIN, "a clone of a CUSTOM element is PLAIN");
    check(os64_html_set_ce(doc, x, OS64_HTML_CE_FAILED) == OS64_HTML_OK && x->ce == OS64_HTML_CE_FAILED &&
              (copy = os64_html_clone(doc, x, false, NULL)) && copy->ce == OS64_HTML_CE_PLAIN,
          "a clone of a FAILED element is PLAIN, to be tried again");
    version = os64_html_version(doc);
    check(os64_html_set_ce(doc, x->first_child, OS64_HTML_CE_CUSTOM) == OS64_HTML_BAD_ARGUMENT &&
              os64_html_set_ce(doc, id_in(other, "p"), OS64_HTML_CE_CUSTOM) == OS64_HTML_BAD_ARGUMENT &&
              os64_html_set_ce(doc, x, (os64_html_ce_t)3) == OS64_HTML_BAD_ARGUMENT &&
              os64_html_set_ce(doc, x, (os64_html_ce_t)-1) == OS64_HTML_BAD_ARGUMENT &&
              os64_html_set_ce(NULL, x, OS64_HTML_CE_PLAIN) == OS64_HTML_BAD_ARGUMENT &&
              os64_html_set_ce(doc, NULL, OS64_HTML_CE_PLAIN) == OS64_HTML_BAD_ARGUMENT &&
              x->ce == OS64_HTML_CE_FAILED && os64_html_version(doc) == version,
          "set_ce refuses a text, another document's element, and a state the enum does not name");
    os64_html_document_free(other);
    os64_html_document_free(doc);

    static const struct { const char *name; bool valid; } names[] = {
        {"x-foo", true}, {"a-", true}, {"a-b.c_d9", true}, {"a-\xc3\xa9", true}, {"\xc3\xa9-a", false},
        {"foo", false}, {"X-foo", false}, {"1-a", false}, {"-a", false}, {"_a-b", false}, {"{a-b", false},
        {"a-B", false}, {"a-b c", false},
        {"a_b", false}, {"annotation-xml", false}, {"color-profile", false}, {"font-face", false},
        {"font-face-src", false}, {"font-face-uri", false}, {"font-face-format", false},
        {"font-face-name", false}, {"missing-glyph", false}, {"font-faces", true}, {"", false}};
    for (size_t i = 0; i < H_ARRAY(names); i++)
        check(os64_html_custom_name(names[i].name) == names[i].valid, "custom name \"%s\": %s", names[i].name,
              names[i].valid ? "valid" : "not valid");
    check(!os64_html_custom_name(NULL), "custom name: NULL is not one");
}
