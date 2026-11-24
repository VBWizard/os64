/* THE ORACLE (BRIEF_CASCADE_TRACKING § Proof): after an update the cascade
 * must equal one built fresh from the same document, element by element and
 * property by property. A seeded random walk makes the changes a page makes
 * (elements added, removed and moved; class, id and other attributes, the
 * ones libgarb's state pseudo-classes read among them: checked, open,
 * disabled, lang, dir; style attributes; text) in batches, on a generated
 * tree, under generated sheets that favour :has() in the forms the update
 * follows (subject and anchored, filtered and bare; descendant, child and
 * sibling arguments; inside :not(), :is() and :where(); a state inside the
 * argument; an anchor's own compound holding a pseudo-class that reads its
 * place, its children, a fieldset or an ancestor's lang or dir). A difference
 * fails, naming the seed, the step, the element, the property and the batch
 * that led there. The walk keeps a padding of elements no rule reaches, so
 * a batch reaches less than half the elements and the update is not merely
 * refused as broad; the counts at the end say how many updates took. */

typedef struct {
    uint64_t state;
} OracleRng;
static uint64_t oracle_next(OracleRng *r)
{
    r->state ^= r->state << 13; r->state ^= r->state >> 7; r->state ^= r->state << 17;
    return r->state;
}
static int oracle_pick(OracleRng *r, int n) { return (int)(oracle_next(r) % (uint64_t)n); }

static const char *const kOracleTags[]={"div","p","span","section","i","input","fieldset","details","legend"};
static const char *const kOracleClasses[]={"a","b","x","k"};
// A pseudo-class an anchor's own compound may hold, one that reads more
// than the element's attributes (its place among its siblings, its
// children, an ancestor fieldset's `disabled`, an ancestor's `lang` or
// `dir`); or nothing, half the time. No filter: an anchor holding only
// this is bare.
static const char *oracle_pseudo(OracleRng *r)
{
    static const char *const kPseudo[]={":first-child",":last-child",":nth-child(2)",":empty",":disabled",
                                        ":lang(fr)",":dir(rtl)",":only-child",":last-of-type",":only-of-type",
                                        ":nth-last-child(2)",":nth-last-child(-n+3)",":nth-last-of-type(2)",
                                        ":nth-child(3n)",":nth-last-child(2n+1)",":nth-of-type(2n)",
                                        ":nth-child(n+2)",":first-of-type",":not(:nth-child(2n))"};
    return oracle_pick(r, 2) ? kPseudo[oracle_pick(r, 19)] : "";
}
// An element's own compound: filtered (a type, a class or an attribute),
// sometimes with a pseudo-class beside. Few names, so rules match often
// and a turn shows.
static void oracle_compound(OracleRng *r, char *out, size_t cap)
{
    const char *pseudo = oracle_pick(r, 2) ? oracle_pseudo(r) : "";
    static const char *const kIds[]={"m","n","o"};
    switch (oracle_pick(r, 8)) {
    case 0: snprintf(out, cap, "%s%s", kOracleTags[oracle_pick(r, 4)], pseudo); break;
    case 1: case 2: snprintf(out, cap, ".%s%s", kOracleClasses[oracle_pick(r, 4)], pseudo); break;
    case 3: snprintf(out, cap, "[data-x]%s", pseudo); break;
    // The attribute readers by key: an id, a title, a lang's prefix, two
    // classes at once.
    case 4: snprintf(out, cap, "#%s%s", kIds[oracle_pick(r, 3)], pseudo); break;
    case 5: snprintf(out, cap, "[title]%s", pseudo); break;
    case 6: snprintf(out, cap, ".%s.%s[lang|=fr]", kOracleClasses[oracle_pick(r, 4)], kOracleClasses[oracle_pick(r, 4)]); break;
    default: snprintf(out, cap, "%s.%s%s", kOracleTags[oracle_pick(r, 4)], kOracleClasses[oracle_pick(r, 4)], pseudo); break;
    }
}
// What a :has() argument looks for, state pseudo-classes among it, and
// the forms a change of children is asked of in their own ways: a position
// or no filter (always asked), two compounds (every element brought).
static void oracle_argument(OracleRng *r, char *out, size_t cap)
{
    static const char *const kStates[]={"input:checked","fieldset:disabled","input:disabled","p:empty",
                                        "details[open]",":focus-visible","[data-x]",".k:last-child","*",
                                        ".x .k","span:not(.b)",".a + .k"};
    char c[64];
    if (oracle_pick(r, 3) == 0) { snprintf(out, cap, "%s", kStates[oracle_pick(r, 12)]); return; }
    oracle_compound(r, c, sizeof(c));
    snprintf(out, cap, "%s", c);
}
// Rule `i`'s declarations: a property of its own as far as the list goes,
// its value the rule's number, so which rules match an element shows in its
// winners and an inherited one in its descendants'; sometimes a custom
// property (one declared, one made from another, one inherited) and a
// longhand, a shorthand or a pseudo-element's value read through var().
static void oracle_declarations(OracleRng *r, int i, char *out, size_t cap)
{
    static const char *const kProps[]={"margin-left","margin-top","padding-left","padding-top","width","height",
                                       "min-width","max-width","top","left","z-index","order","flex-grow",
                                       "letter-spacing","word-spacing","text-indent","line-height","border-left-width"};
    const char *prop = kProps[i % 18];
    bool unitless = !strcmp(prop, "z-index") || !strcmp(prop, "order") || !strcmp(prop, "flex-grow") ||
                    !strcmp(prop, "line-height");
    int n = snprintf(out, cap, "%s:%d%s", prop, i + 1, unitless ? "" : "px");
    static const char *const kVars[]={";--v:%d", ";--w:calc(%dpx)", ";--w:var(--v)", ";--v:inherit",
                                      ";outline-width:var(--v,0px)", ";margin:var(--w,%dpx)",
                                      ";border-top-width:var(--w,2px)"};
    if (oracle_pick(r, 3) == 0) snprintf(out + n, cap - (size_t)n, kVars[oracle_pick(r, 7)], i + 1);
}
// One sheet in four is large, past one word of anchor bits a slot.
static void oracle_sheet(OracleRng *r, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    int rules = oracle_pick(r, 4) == 0 ? 110 + oracle_pick(r, 40) : 8 + oracle_pick(r, 10);
    for (int i = 0; i < rules && len + 512 < cap; i++) {
        char f[64], d[64], a[64], decl[128];
        oracle_compound(r, f, sizeof(f));
        oracle_compound(r, d, sizeof(d));
        oracle_argument(r, a, sizeof(a));
        oracle_declarations(r, i, decl, sizeof(decl));
        const char *p = oracle_pseudo(r);
        char g[64];
        oracle_compound(r, g, sizeof(g));
        switch (oracle_pick(r, 34)) {
        case 0: len += (size_t)snprintf(out + len, cap - len, "%s:has(%s) %s{%s}", f, a, d, decl); break;
        case 1: len += (size_t)snprintf(out + len, cap - len, "%s:has(> %s) > %s{%s}", f, a, d, decl); break;
        case 2: len += (size_t)snprintf(out + len, cap - len, "%s:has(+ %s) ~ %s{%s}", f, a, d, decl); break;
        case 3: len += (size_t)snprintf(out + len, cap - len, "%s:has(~ %s) + %s{%s}", f, a, d, decl); break;
        case 4: len += (size_t)snprintf(out + len, cap - len, "%s:not(:has(%s)) %s{%s}", f, a, d, decl); break;
        case 5: len += (size_t)snprintf(out + len, cap - len, "%s:is(.k, :has(%s)) > %s{%s}", f, a, d, decl); break;
        case 6: len += (size_t)snprintf(out + len, cap - len, ":where(%s:has(%s)) ~ %s{%s}", f, a, d, decl); break;
        case 7: len += (size_t)snprintf(out + len, cap - len, "%s:has(%s){%s}", f, a, decl); break;
        case 8: len += (size_t)snprintf(out + len, cap - len, "%s:has(+ %s){%s}", f, a, decl); break;
        case 9: len += (size_t)snprintf(out + len, cap - len, "%s:not(:hover, :has(%s)) %s{%s}", f, a, d, decl); break;
        case 10:
            // A pseudo-element whose value reads a custom property, now and
            // then: what a map's change reaches through its element.
            if (oracle_pick(r, 4) == 0)
                len += (size_t)snprintf(out + len, cap - len, "%s::before{content:'p';outline-width:var(--v,1px)}", d);
            else len += (size_t)snprintf(out + len, cap - len, "%s{%s}", d, decl);
            break;
        case 11: len += (size_t)snprintf(out + len, cap - len, "%s > %s{%s}", f, d, decl); break;
        case 12: len += (size_t)snprintf(out + len, cap - len, "%s + %s{%s}", f, d, decl); break;
        case 13: len += (size_t)snprintf(out + len, cap - len, "%s:first-child{%s}", d, decl); break;
        case 14: len += (size_t)snprintf(out + len, cap - len, "%s %s{%s}", f, d, decl); break;
        // The anchor left of a combinator inside the :is(): `.k` names
        // another element, so it is no filter of the anchor's.
        case 15: len += (size_t)snprintf(out + len, cap - len, ".k:is(%s:has(%s) > %s) %s{%s}", f, a, d, d, decl); break;
        // Bare anchors: nothing filters them (a pseudo-class beside is no
        // filter), and the cascade still tracks.
        case 16: len += (size_t)snprintf(out + len, cap - len, "%s:has(%s) %s{%s}", p, a, d, decl); break;
        case 17: len += (size_t)snprintf(out + len, cap - len, "%s:has(> %s) > %s{%s}", p, a, d, decl); break;
        case 18: len += (size_t)snprintf(out + len, cap - len, "%s:has(+ %s) ~ %s{%s}", p, a, d, decl); break;
        case 19: len += (size_t)snprintf(out + len, cap - len, "%s:not(:has(%s)) %s{%s}", p, a, d, decl); break;
        case 20: len += (size_t)snprintf(out + len, cap - len, "%s:has(~ %s) + %s{%s}", p, a, d, decl); break;
        case 21: len += (size_t)snprintf(out + len, cap - len, "%s:is(.k, :has(%s)) > %s{%s}", p, a, d, decl); break;
        case 22: len += (size_t)snprintf(out + len, cap - len, ":where(%s:has(%s)) ~ %s{%s}", p, a, d, decl); break;
        // The readers (select.h § A READER): `~`, a `+` chain, a `+` with a
        // descent after it, :is() alternatives, an :empty beside, and a
        // position form left of a combinator.
        case 24: len += (size_t)snprintf(out + len, cap - len, "%s ~ %s{%s}", f, d, decl); break;
        case 25: len += (size_t)snprintf(out + len, cap - len, "%s + %s + %s{%s}", f, g, d, decl); break;
        case 26: len += (size_t)snprintf(out + len, cap - len, "%s + %s %s{%s}", f, g, d, decl); break;
        case 27: len += (size_t)snprintf(out + len, cap - len, ":is(%s, %s) + %s{%s}", f, g, d, decl); break;
        case 28: len += (size_t)snprintf(out + len, cap - len, "%s:empty ~ %s{%s}", f, d, decl); break;
        case 29: len += (size_t)snprintf(out + len, cap - len, "%s%s > %s{%s}", f, p, d, decl); break;
        case 30: len += (size_t)snprintf(out + len, cap - len, "%s%s + %s{%s}", f, p, d, decl); break;
        case 31: len += (size_t)snprintf(out + len, cap - len, "%s + %s ~ %s{%s}", f, g, d, decl); break;
        case 32: len += (size_t)snprintf(out + len, cap - len, "%s:not(%s + %s){%s}", d, f, g, decl); break;
        default: len += (size_t)snprintf(out + len, cap - len, "input:checked ~ %s{%s}", d, decl); break;
        }
    }
}

// Every element under `root`, in tree order.
static int oracle_elements(os64_html_node_t *root, os64_html_node_t **out, int cap)
{
    int n = 0;
    for (os64_html_node_t *x = root; x != NULL;) {
        if (x->kind == OS64_HTML_ELEMENT && n < cap) out[n++] = x;
        if (x->first_child) { x = x->first_child; continue; }
        while (x != root && x->next == NULL) x = x->parent;
        if (x == root) break;
        x = x->next;
    }
    return n;
}
static bool oracle_inside(const os64_html_node_t *x, const os64_html_node_t *ancestor)
{
    for (; x != NULL; x = x->parent) if (x == ancestor) return true;
    return false;
}
static os64_html_node_t *oracle_make(OracleRng *r, os64_html_document_t *doc)
{
    os64_html_node_t *e = os64_html_create_element(doc, OS64_HTML_NS_HTML, kOracleTags[oracle_pick(r, 9)], NULL);
    if (e == NULL) return NULL;
    if (oracle_pick(r, 2)) os64_html_set_attr(doc, e, "class", kOracleClasses[oracle_pick(r, 4)], 1);
    if (!strcmp(e->name, "input")) os64_html_set_attr(doc, e, "type", "checkbox", 8);
    if (oracle_pick(r, 5) == 0) os64_html_set_attr(doc, e, "checked", "", 0);
    return e;
}
static void oracle_path(const os64_html_node_t *x, char *out, size_t cap)
{
    const os64_html_node_t *chain[64];
    int n = 0;
    for (; x != NULL && x->kind == OS64_HTML_ELEMENT && n < 64; x = x->parent) chain[n++] = x;
    size_t len = 0;
    out[0] = '\0';
    while (n-- > 0 && len + 32 < cap) {
        int index = 0;
        for (const os64_html_node_t *s = chain[n]->prev; s; s = s->prev) index += s->kind == OS64_HTML_ELEMENT;
        len += (size_t)snprintf(out + len, cap - len, "%s%s[%d]", len ? ">" : "", chain[n]->name, index);
    }
}
// One change, described into `log`. `growing`: mostly additions, of
// several elements at once. `*recent`: what the change before this one
// added or moved, if it did. Two times in three this change is made inside it (a
// removal, a class, an attribute): an element's anchor bits are first asked
// when it is styled, and the change that next visits it is the first to
// compare them, so a bit stored wrong at birth shows only then.
static void oracle_change(OracleRng *r, os64_html_document_t *doc, os64_html_node_t *root, bool growing,
                          os64_html_node_t **recent, char *log, size_t cap)
{
    os64_html_node_t *els[512];
    int n = oracle_elements(root, els, 512);
    os64_html_node_t *e = els[oracle_pick(r, n)];
    int op = oracle_pick(r, 10);
    if (*recent != NULL && oracle_pick(r, 3) != 0) {
        os64_html_node_t *in[64];
        int m = oracle_elements(*recent, in, 64);
        static const int kInside[] = {1, 3, 5};
        if (m > 1) { e = in[1 + oracle_pick(r, m - 1)]; op = kInside[oracle_pick(r, 3)]; }
    }
    *recent = NULL;
    char path[256];
    oracle_path(e, path, sizeof(path));
    if (growing && oracle_pick(r, 2)) op = 0;
    if (op == 1 && (n < 12 || growing)) op = 0;
    // Room to grow past the slot table's first size, which moves every
    // slot's row of bits.
    if (op == 0 && n > 400) op = 1;
    switch (op) {
    case 0: {
        os64_html_node_t *k = oracle_make(r, doc);
        for (int more = growing ? 1 + oracle_pick(r, 6) : oracle_pick(r, 2) ? 1 + oracle_pick(r, 2) : 0; k && more > 0;
             more--) {
            os64_html_node_t *g = oracle_make(r, doc);
            if (g) os64_html_insert(doc, k, g, NULL);
        }
        os64_html_node_t *before = e->first_child;
        for (int skip = oracle_pick(r, 4); before && skip > 0; skip--) before = before->next;
        if (k) os64_html_insert(doc, e, k, before);
        *recent = k;
        snprintf(log, cap, "add %s under %s", k ? k->name : "?", path);
        break;
    }
    case 1:
        if (e == root) { snprintf(log, cap, "nothing"); break; }
        os64_html_remove(doc, e);
        snprintf(log, cap, "remove %s", path);
        break;
    case 2: {
        os64_html_node_t *to = els[oracle_pick(r, n)];
        if (e == root || oracle_inside(to, e)) { snprintf(log, cap, "nothing"); break; }
        os64_html_node_t *before = oracle_pick(r, 2) ? to->first_child : NULL;
        char where[256];
        oracle_path(to, where, sizeof(where));
        os64_html_insert(doc, to, e, before == e ? NULL : before);
        *recent = e;
        snprintf(log, cap, "move %s under %s", path, where);
        break;
    }
    case 3: case 4: {
        const char *c = kOracleClasses[oracle_pick(r, 4)];
        if (oracle_pick(r, 4) == 0) os64_html_remove_attr(doc, e, "class");
        else os64_html_set_attr(doc, e, "class", c, strlen(c));
        snprintf(log, cap, "class of %s", path);
        break;
    }
    case 5: {
        static const char *const kAttrs[]={"checked","open","disabled","data-x","title","hidden","lang","dir"};
        static const char *const kValues[]={"","","","","","","fr","rtl"};
        int which = oracle_pick(r, 8);
        const char *a = kAttrs[which];
        if (os64_html_attr(e, a)) os64_html_remove_attr(doc, e, a);
        else os64_html_set_attr(doc, e, a, kValues[which], strlen(kValues[which]));
        snprintf(log, cap, "toggle %s on %s", a, path);
        break;
    }
    case 6: {
        static const char *const kIds[]={"m","n","o"};
        if (oracle_pick(r, 3) == 0) os64_html_remove_attr(doc, e, "id");
        else os64_html_set_attr(doc, e, "id", kIds[oracle_pick(r, 3)], 1);
        snprintf(log, cap, "id of %s", path);
        break;
    }
    case 7: {
        // A container's custom property written, rewritten with the same
        // value, chained, inherited; and longhands read through one.
        static const char *const kStyles[]={"color:purple","--v:3","margin-left:1px","--v:4","--w:2px",
                                            "--v:3;--w:var(--v)","--v:inherit","outline-width:var(--w,1px)",
                                            "transform:translateX(3px)"};
        if (oracle_pick(r, 5) == 0) os64_html_remove_attr(doc, e, "style");
        else { const char *s = kStyles[oracle_pick(r, 9)]; os64_html_set_attr(doc, e, "style", s, strlen(s)); }
        snprintf(log, cap, "style of %s", path);
        break;
    }
    default: {
        os64_html_node_t *t = e->first_child;
        while (t && t->kind != OS64_HTML_TEXT) t = t->next;
        bool empty = oracle_pick(r, 2);
        if (t && oracle_pick(r, 2)) os64_html_set_text(doc, t, empty ? "" : "w", empty ? 0 : 1);
        else { os64_html_node_t *k = os64_html_create_text(doc, "t", 1, NULL); if (k) os64_html_insert(doc, e, k, e->first_child); }
        snprintf(log, cap, "text in %s", path);
        break;
    }
    }
}
// Every element's winners and pseudo-elements' against a fresh cascade's
// (garb_cascade_compare, the check yonder's --cascade-check makes): false,
// with the first difference printed, when they differ.
static bool oracle_compare(garb_cascade_t *c, garb_sheet_in_t *in, os64_html_document_t *doc,
                           uint64_t seed, int step, const char *batch, const char *sheet)
{
    garb_cascade_t *fresh = garb_cascade(in, 1, doc, garb_cascade_env(c));
    if (fresh == NULL || garb_cascade_incomplete(fresh)) {
        fprintf(stderr, "FAIL oracle seed %llu step %d: no fresh cascade\n", (unsigned long long)seed, step);
        garb_cascade_free(fresh);
        return false;
    }
    char why[2048];
    bool same = garb_cascade_compare(c, fresh, doc, why, sizeof(why));
    if (!same)
        fprintf(stderr, "FAIL oracle seed %llu step %d: %s (updated against fresh)\n  batch: %s\n  sheet: %s\n",
                (unsigned long long)seed, step, why, batch, sheet);
    garb_cascade_free(fresh);
    return same;
}

// One walk: `steps` batches on one seed, a new sheet every 50.
static bool oracle_walk(uint64_t seed, int steps, unsigned *taken, unsigned *refused)
{
    OracleRng r = {seed * 0x9E3779B97F4A7C15ull + 1};
    char html[16384] = "<!doctype html><body><aside>";
    for (int i = 0; i < 300; ++i) strcat(html, "<u></u>");
    strcat(html, "</aside><div id=root>");
    for (int i = 0; i < 40; ++i) {
        const char *tag = kOracleTags[oracle_pick(&r, 5)];
        char one[96];
        snprintf(one, sizeof(one), "<%s class=%s>%s</%s>", tag, kOracleClasses[oracle_pick(&r, 4)],
                 oracle_pick(&r, 2) ? "t" : "", tag);
        strcat(html, one);
    }
    strcat(html, "</div>");
    os64_html_document_t *doc = update_doc(html);
    os64_html_node_t *root = (os64_html_node_t *)by_id(doc->document, "root");
    os64_html_node_t *recent = NULL;
    bool ok = true;
    char sheet_text[8192];
    garb_parsed_t sheet = {0};
    garb_sheet_in_t in = {.sheet = &sheet};
    garb_cascade_t *c = NULL;
    for (int step = 0; step < steps && ok; step++) {
        if (step % 50 == 0) {
            garb_cascade_free(c);
            garb_free(&sheet);
            oracle_sheet(&r, sheet_text, sizeof(sheet_text));
            garb_parse_sheet_text(sheet_text, strlen(sheet_text), &sheet);
            c = garb_cascade(&in, 1, doc, (garb_env_t){.width = 800, .height = 600});
        }
        char batch[2048] = "", one[600];
        for (int k = 0, changes = 1 + oracle_pick(&r, 3); k < changes; k++) {
            // Each second sheet's life grows the tree, the first half of it.
            oracle_change(&r, doc, root, step % 100 >= 50 && step % 100 < 75, &recent, one, sizeof(one));
            if (strlen(batch) + strlen(one) + 3 < sizeof(batch)) { if (k) strcat(batch, "; "); strcat(batch, one); }
        }
        if (garb_cascade_update(c)) ++*taken;
        else {
            refused[garb_cascade_update_refusal(c)]++;
            garb_cascade_t *fresh = garb_cascade(&in, 1, doc, garb_cascade_env(c));
            garb_cascade_free(c);
            c = fresh;
            continue;
        }
        ok = oracle_compare(c, &in, doc, seed, step, batch, sheet_text);
    }
    garb_cascade_free(c);
    garb_free(&sheet);
    os64_html_document_free(doc);
    return ok;
}

// The oracle cannot see the matcher's own mistakes (a fresh cascade makes
// them too), so the :has() walks has_matches narrows are held to answers
// worked out by hand: a leading `>`, `+` or `~` with the compounds after it
// going down, and with a sibling combinator among them, which it must not
// narrow. (tools/test_garb_select.py holds the same forms to cssselect2.)
static bool oracle_has_walks(void)
{
    static const char kPage[] =
        "<!doctype html><body>"
        "<div id=a><p id=a1>x</p><span id=a2><i>y</i></span></div>"
        "<div id=b><span id=b1></span><p id=b2></p></div>"
        "<div id=c><div id=c1><a id=ca></a></div></div>"
        "<div id=d><div id=d1></div><div id=d2><a id=da></a></div></div>"
        "<div id=e><p id=e1><span id=e2></span></p></div>"
        "<h1 id=h></h1><p id=hp><span id=hs></span></p><div id=hd></div>"
        "<h2 id=k></h2><div id=kd><a id=ka></a></div><p id=kp><a id=kpa></a></p>"
        "<section id=t>text first<a id=ta></a></section>";
    static const struct { const char *sel, *want; } kCases[] = {
        {"div:has(> p ~ span)", "a"}, {"div:has(> p + span)", "a"}, {"div:has(> p span)", "e"},
        {"div:has(> div a)", "c d"}, {"div:has(> div ~ div a)", "d"}, {":has(> a)", "c1 d2 kd kp t"},
        {"h1:has(+ p span)", "h"}, {"h1:has(+ p ~ div)", "h"}, {"h1:has(+ div)", ""},
        {"h2:has(+ div)", "k"}, {"h2:has(~ p > a)", "k"}, {"h2:has(~ div a)", "k"},
    };
    os64_html_document_t *doc = update_doc(kPage);
    bool ok = true;
    for (size_t i = 0; i < sizeof(kCases) / sizeof(*kCases); i++) {
        garb_parsed_t r;
        garb_parse_values(kCases[i].sel, strlen(kCases[i].sel), &r);
        garb_selectors_t *sel = garb_selectors_parse(&r, r.values, r.nvalues, OS64_HTML_NO_QUIRKS, false);
        char got[256] = "";
        os64_html_node_t *els[64];
        int n = oracle_elements(doc->document, els, 64);
        for (int k = 0; sel != NULL && k < n; k++) {
            const os64_html_attr_t *id = os64_html_attr(els[k], "id");
            if (id == NULL || !garb_selector_matches(sel, 0, els[k])) continue;
            if (got[0]) strcat(got, " ");
            strcat(got, id->value);
        }
        if (sel == NULL || strcmp(got, kCases[i].want) != 0) {
            fprintf(stderr, "FAIL oracle: %s matches '%s', not '%s'\n", kCases[i].sel, got, kCases[i].want);
            ok = false;
        }
        garb_free(&r);
    }
    os64_html_document_free(doc);
    return ok;
}

// The compare itself says where two cascades differ, and nothing where
// they do not.
static bool oracle_compare_says(void)
{
    os64_html_document_t *doc = update_doc("<!doctype html><div class=a><p id=q class=b>t</p></div>");
    const char *one = ".a .b{color:red}", *two = ".a .b{color:blue}";
    garb_parsed_t s1, s2;
    garb_parse_sheet_text(one, strlen(one), &s1);
    garb_parse_sheet_text(two, strlen(two), &s2);
    garb_sheet_in_t i1 = {.sheet = &s1}, i2 = {.sheet = &s2};
    garb_env_t env = {.width = 800, .height = 600};
    garb_cascade_t *a = garb_cascade(&i1, 1, doc, env), *a2 = garb_cascade(&i1, 1, doc, env),
                   *b = garb_cascade(&i2, 1, doc, env);
    char why[512];
    bool ok = garb_cascade_compare(a, a2, doc, why, sizeof(why)) && why[0] == '\0' &&
              !garb_cascade_compare(a, b, doc, why, sizeof(why)) &&
              strstr(why, "p[0]#q color: ") != NULL && strstr(why, "rules setting it there: ") != NULL;
    if (!ok) fprintf(stderr, "FAIL oracle: the compare says '%s'\n", why);
    garb_cascade_free(a); garb_cascade_free(a2); garb_cascade_free(b);
    garb_free(&s1); garb_free(&s2);
    os64_html_document_free(doc);
    return ok;
}

static int cascade_oracle(int argc, char **argv)
{
    size_t baseline = live;
    if (!oracle_compare_says() || !oracle_has_walks()) return 1;
    uint64_t first = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    int seeds = argc > 2 ? 1 : 8;
    int steps = argc > 3 ? atoi(argv[3]) : 400;
    unsigned taken = 0, refused[GARB_UPDATE_REASONS] = {0};
    bool ok = true;
    for (int s = 0; s < seeds && ok; s++) ok = oracle_walk(first + (uint64_t)s, steps, &taken, refused);
    printf("libgarb oracle: %d seed(s) from %llu, %d steps each: %u updates equal a fresh cascade",
           seeds, (unsigned long long)first, steps, taken);
    for (int i = 1; i < GARB_UPDATE_REASONS; i++)
        if (refused[i]) printf("; %u refused %s", refused[i], garb_update_refusal_name((garb_update_refusal_t)i));
    printf("\n");
    if (ok && taken < (unsigned)(seeds * steps) / 2) {
        fprintf(stderr, "FAIL oracle: fewer than half the updates took, so the walk proves little\n");
        ok = false;
    }
    if (live != baseline) { fprintf(stderr, "FAIL oracle: allocations outlive the walk\n"); ok = false; }
    return ok ? 0 : 1;
}
