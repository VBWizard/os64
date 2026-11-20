/* innerText's walk (libdom/inner_text.c) on the host, under the sanitizers:
 * libhtml parses each case, and a provider of the test's own answers the
 * four facts from each element's tag and three attributes, inherited the
 * way CSS inherits them:
 *
 *   data-t   display: n none, b block, i inline, c contents, t table,
 *            g a row group, r row, d cell, k caption
 *   data-v   visibility: h hidden, v visible (inherited; visible at root)
 *   data-ws  white space: n collapse, p preserve, l pre-line (inherited)
 *
 * A tag with no data-t takes a small UA sheet's display (head, script,
 * style, title and template none; the usual blocks; the table parts;
 * slot contents), and `pre` preserves. The UA sheet yonder really uses is
 * proved in QEMU (WHEEL_FILELIST_INNERTEXT.md § 3.5). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inner_text.h"

/* What libhtml and the walk ask of the system. */
void *os64_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
void *os64_memmove(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }
void *os64_malloc(size_t n) { return malloc(n); }
void *os64_calloc(size_t n, size_t s) { return calloc(n, s); }
void os64_free(void *p) { free(p); }
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (int64_t)fwrite(buf, 1, len, handle == 2 ? stderr : stdout);
}
void os64_exit(int32_t code) { exit(code != 0 ? code : 3); }

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ── The provider ─────────────────────────────────────────────────────── */

typedef struct {
    const os64_html_document_t *doc;
    size_t calls;
    bool refuse;
    /* The element's own attributes only, no inheritance: the deep case's,
     * where climbing to the root at every call would make the TEST
     * quadratic. */
    bool shallow;
} Facts;

static const char *attr(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a != NULL ? a->value : NULL;
}

static int display_of(const os64_html_node_t *n)
{
    const char *t = attr(n, "data-t");
    if (t != NULL) {
        switch (t[0]) {
        case 'n': return -1;
        case 'b': return OS64_DOM_TEXT_BLOCK;
        case 'c': return OS64_DOM_TEXT_CONTENTS;
        case 't': return OS64_DOM_TEXT_TABLE;
        case 'g': return OS64_DOM_TEXT_TABLE_PART;
        case 'r': return OS64_DOM_TEXT_TABLE_ROW;
        case 'd': return OS64_DOM_TEXT_TABLE_CELL;
        case 'k': return OS64_DOM_TEXT_TABLE_CAPTION;
        default: return OS64_DOM_TEXT_INLINE;
        }
    }
    if (n->ns != OS64_HTML_NS_HTML || n->name == NULL) return OS64_DOM_TEXT_INLINE;
    static const char *const none[] = {"head", "script", "style", "title", "template", "meta", "link", NULL};
    static const char *const block[] = {"html", "body", "div", "p", "ul", "ol", "li", "h1", "h2", "pre",
                                        "section", "blockquote", NULL};
    for (int i = 0; none[i]; i++) if (strcmp(n->name, none[i]) == 0) return -1;
    for (int i = 0; block[i]; i++) if (strcmp(n->name, block[i]) == 0) return OS64_DOM_TEXT_BLOCK;
    if (strcmp(n->name, "table") == 0) return OS64_DOM_TEXT_TABLE;
    if (!strcmp(n->name, "tbody") || !strcmp(n->name, "thead") || !strcmp(n->name, "tfoot"))
        return OS64_DOM_TEXT_TABLE_PART;
    if (strcmp(n->name, "tr") == 0) return OS64_DOM_TEXT_TABLE_ROW;
    if (!strcmp(n->name, "td") || !strcmp(n->name, "th")) return OS64_DOM_TEXT_TABLE_CELL;
    if (strcmp(n->name, "caption") == 0) return OS64_DOM_TEXT_TABLE_CAPTION;
    if (strcmp(n->name, "slot") == 0) return OS64_DOM_TEXT_CONTENTS;
    return OS64_DOM_TEXT_INLINE;
}

static bool facts(void *opaque, const os64_html_node_t *element, os64_dom_text_style_t *out)
{
    Facts *f = opaque;
    f->calls++;
    if (f->refuse) return false;
    if (element == NULL) return true;       /* the read's first call: nothing to make current */
    memset(out, 0, sizeof(*out));
    out->rendered = true;
    out->visible = true;
    bool seen_v = false, seen_ws = false;
    /* A host's child no slot takes is in no flat tree, so layout gives it
     * no style: not rendered. */
    if (!f->shallow && element->parent != NULL && element->parent->kind == OS64_HTML_ELEMENT &&
        os64_html_flat_parent(f->doc, element) == NULL)
        out->rendered = false;
    for (const os64_html_node_t *n = element; n != NULL && n->kind == OS64_HTML_ELEMENT;
         n = f->shallow ? NULL : os64_html_flat_parent(f->doc, n)) {
        if (display_of(n) < 0) out->rendered = false;
        const char *v = attr(n, "data-v");
        if (!seen_v && v != NULL) { out->visible = v[0] != 'h'; seen_v = true; }
        const char *ws = attr(n, "data-ws");
        if (!seen_ws && (ws != NULL || (n->name && strcmp(n->name, "pre") == 0))) {
            out->white_space = ws == NULL || ws[0] == 'p' ? OS64_DOM_SPACE_PRESERVE
                             : ws[0] == 'l' ? OS64_DOM_SPACE_PRE_LINE : OS64_DOM_SPACE_COLLAPSE;
            seen_ws = true;
        }
    }
    int d = display_of(element);
    out->display = (uint8_t)(d < 0 ? OS64_DOM_TEXT_INLINE : d);
    return true;
}

/* ── Memory ───────────────────────────────────────────────────────────── */

typedef struct { int left; int live; } Budget;     /* left < 0: no limit */

static void *grow(void *ctx, void *old, size_t old_size, size_t size)
{
    Budget *b = ctx;
    if (b->left == 0) return NULL;
    if (b->left > 0) b->left--;
    void *block = malloc(size);
    if (block == NULL) return NULL;
    if (old != NULL) {
        memcpy(block, old, old_size);
        free(old);
        b->live--;
    }
    b->live++;
    return block;
}

static void release(void *ctx, void *block)
{
    Budget *b = ctx;
    if (block != NULL) b->live--;
    free(block);
}

/* ── The cases ────────────────────────────────────────────────────────── */

static os64_html_document_t *parse(const char *html, size_t depth)
{
    os64_html_options_t options = os64_html_options_default();
    if (depth != 0) options.max_depth = depth;
    options.max_bytes = strlen(html) + 1;
    options.max_work = UINT64_MAX / 2;
    options.charset = "utf-8";
    os64_html_parser_t *p = os64_html_parser_new(&options);
    if (p == NULL) return NULL;
    os64_html_parser_feed(p, html, strlen(html));
    return os64_html_parser_finish(p);
}

static const os64_html_node_t *by_id(const os64_html_node_t *n, const char *id)
{
    /* Tree order without recursion: the deep case is deep. */
    const os64_html_node_t *at = n;
    while (at != NULL) {
        const char *v = at->kind == OS64_HTML_ELEMENT ? attr(at, "id") : NULL;
        if (v != NULL && strcmp(v, id) == 0) return at;
        if (at->first_child) { at = at->first_child; continue; }
        while (at != NULL && at->next == NULL) at = at->parent;
        if (at != NULL) at = at->next;
    }
    return NULL;
}

static d_inner_text_status_t read_text(os64_html_document_t *doc, const os64_html_node_t *root, Facts *f,
                                       Budget *b, char **out, size_t *len)
{
    d_inner_text_memory_t memory = {grow, release, b};
    f->doc = doc;
    return d_inner_text(doc, root, facts, f, &memory, out, len);
}

static void show(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') printf("\\n");
        else if (*s == '\t') printf("\\t");
        else putchar(*s);
    }
}

static void expect(const char *name, const char *html, const char *want)
{
    os64_html_document_t *doc = parse(html, 0);
    check(doc != NULL, name);
    if (doc == NULL) return;
    const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
    check(root != NULL, name);
    Facts f = {0};
    Budget b = {-1, 0};
    char *text = NULL;
    size_t len = 0;
    d_inner_text_status_t status = read_text(doc, root, &f, &b, &text, &len);
    if (want == NULL) {
        check(status == D_INNER_TEXT_NOT_RENDERED && text == NULL, name);
    } else {
        int ok = status == D_INNER_TEXT_OK && text != NULL && strlen(text) == len && strcmp(text, want) == 0;
        if (!ok && text != NULL) {
            printf("  %s: got \"", name); show(text); printf("\" want \""); show(want); printf("\"\n");
        }
        check(ok, name);
    }
    if (text != NULL) release(&b, text);
    check(b.live == 0, "every block the walk took is given back");
    os64_html_document_free(doc);
}

static void shadow_case(void)
{
    os64_html_document_t *doc = parse("<div id=r>a<span id=h><i>light</i><b slot=x>named</b></span>z</div>", 0);
    os64_html_node_t *host = (os64_html_node_t *)by_id((const os64_html_node_t *)doc->document, "h");
    int64_t status = 0;
    os64_html_node_t *shadow = os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN, &status);
    check(shadow != NULL, "shadow: attached");
    os64_html_node_t *s1 = os64_html_create_text(doc, "[", 1, &status);
    os64_html_node_t *slot = os64_html_create_element(doc, OS64_HTML_NS_HTML, "slot", &status);
    os64_html_node_t *s2 = os64_html_create_text(doc, "]", 1, &status);
    os64_html_insert(doc, shadow, s1, NULL);
    os64_html_insert(doc, shadow, slot, NULL);
    os64_html_insert(doc, shadow, s2, NULL);
    const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
    Facts f = {0};
    Budget b = {-1, 0};
    char *text = NULL;
    size_t len = 0;
    read_text(doc, root, &f, &b, &text, &len);
    /* The walk is the host's children, as the standard's steps and WPT's
     * getter say ("Shadow DOM contents ignored"): the shadow root's own
     * text is none of it. The default slot takes the unnamed light child,
     * which is rendered; the named one, with no slot of its name, is in no
     * flat tree, has no style and renders nowhere. */
    int ok = text != NULL && strcmp(text, "alightz") == 0;
    if (!ok && text) { printf("  shadow: got \""); show(text); printf("\"\n"); }
    check(ok, "shadow: a host's children, not its shadow root's");
    release(&b, text);
    check(b.live == 0, "shadow: nothing kept");
    os64_html_document_free(doc);
}

/* A host's Text child that no slot takes is in no flat tree: the review's
 * case, where the walk once read it from the host's facts. */
static void shadow_text_case(void)
{
    os64_html_document_t *doc = parse("<div id=r>a<span id=h>unslotted</span>z</div>", 0);
    os64_html_node_t *host = (os64_html_node_t *)by_id((const os64_html_node_t *)doc->document, "h");
    int64_t status = 0;
    os64_html_node_t *shadow = os64_html_attach_shadow(doc, host, OS64_HTML_SHADOW_OPEN, &status);
    os64_html_insert(doc, shadow, os64_html_create_text(doc, "S", 1, &status), NULL);
    const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
    Facts f = {0};
    Budget b = {-1, 0};
    char *text = NULL;
    size_t len = 0;
    read_text(doc, root, &f, &b, &text, &len);
    int ok = text != NULL && strcmp(text, "az") == 0;
    if (!ok && text) { printf("  shadow text: got \""); show(text); printf("\"\n"); }
    check(ok, "shadow: an unslotted Text child gives nothing");
    release(&b, text);
    os64_html_document_free(doc);
}

static void deep_case(size_t depth)
{
    size_t cap = depth * 5 + 64;
    char *html = malloc(cap * 2);
    size_t n = 0;
    n += (size_t)sprintf(html + n, "<body><div id=r>");
    for (size_t i = 0; i < depth; i++) n += (size_t)sprintf(html + n, "<span>");
    n += (size_t)sprintf(html + n, "deep");
    html[n] = '\0';
    os64_html_document_t *doc = parse(html, depth + 16);
    free(html);
    check(doc != NULL, "deep: parsed");
    if (doc == NULL) return;
    const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
    size_t levels = 0;
    for (const os64_html_node_t *at = root; at && at->first_child; at = at->first_child) levels++;
    check(levels > depth / 2, "deep: the tree is as deep as asked");
    Facts f = {.shallow = true};
    Budget b = {-1, 0};
    char *text = NULL;
    size_t len = 0;
    d_inner_text_status_t status = read_text(doc, root, &f, &b, &text, &len);
    check(status == D_INNER_TEXT_OK && text != NULL && strcmp(text, "deep") == 0,
          "deep: a tree far deeper than any stack walks without one");
    /* Each element is asked about on the way in and the way out, and its
     * Text child asks again only when it is not the last one asked. */
    check(f.calls <= 3 * levels + 4, "deep: the provider is asked a bounded number of times");
    release(&b, text);
    os64_html_document_free(doc);
}

static void memory_case(void)
{
    const char *html = "<div id=r>" "<p>one two</p><table><tr><td>a</td><td>b</td></tr></table>"
                       "<div>three<br>four</div></div>";
    for (int fail_at = 0; fail_at < 64; fail_at++) {
        os64_html_document_t *doc = parse(html, 0);
        const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
        Facts f = {0};
        Budget b = {fail_at, 0};
        char *text = NULL;
        size_t len = 0;
        d_inner_text_status_t status = read_text(doc, root, &f, &b, &text, &len);
        check(status == D_INNER_TEXT_OK || (status == D_INNER_TEXT_NO_MEMORY && text == NULL),
              "memory: a failed grow is NO_MEMORY with nothing handed back");
        if (text) release(&b, text);
        check(b.live == 0, "memory: nothing leaks on any failure");
        os64_html_document_free(doc);
        if (status == D_INNER_TEXT_OK) break;
    }
}

static void refusal_case(void)
{
    os64_html_document_t *doc = parse("<div id=r>a<b>b</b></div>", 0);
    const os64_html_node_t *root = by_id((const os64_html_node_t *)doc->document, "r");
    Facts f = {.refuse = true};
    Budget b = {-1, 0};
    char *text = NULL;
    size_t len = 0;
    check(read_text(doc, root, &f, &b, &text, &len) == D_INNER_TEXT_NOT_RENDERED && text == NULL,
          "refusal: a provider with no answer is textContent's case");
    check(b.live == 0, "refusal: nothing kept");
    os64_html_document_free(doc);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t depth = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 50000;
    expect("inline spaces collapse across elements", "<div id=r>a<b> b</b>  c</div>", "a b c");
    expect("a tab and line feeds are spaces", "<div id=r>a\t\n\nb</div>", "a b");
    expect("a paragraph's two", "<div id=r>  lead  <p>para</p>tail </div>", "lead\n\npara\n\ntail");
    expect("br", "<div id=r>x<br>y</div>", "x\ny");
    expect("spaces beside a br go", "<div id=r>x <br> y</div>", "x\ny");
    expect("display none", "<div id=r>a<span data-t=n>gone</span>b</div>", "ab");
    expect("visibility hidden, a visible child", "<div id=r>a<span data-v=h>x<span data-v=v>y</span></span>b</div>",
           "ayb");
    expect("a hidden block writes no breaks", "<div id=r>a<div data-v=h>x</div>b</div>", "ab");
    expect("a hidden br", "<div id=r>a<br data-v=h>b</div>", "ab");
    expect("a table", "<table id=r>\n <tr>\n  <td>1</td> <td>2</td>\n </tr>\n <tr><td>3</td><td>4</td></tr>\n</table>",
           "1\t2\n3\t4");
    expect("a table inside", "<div id=r>x<table><tr><td>1</td><td>2</td></tr></table>y</div>", "x\n1\t2\ny");
    /* The inner table is block-level: its edges are required line break
     * counts, inside the cell as anywhere (the standard's step 9). */
    expect("a table in a cell", "<table id=r><tr><td>a</td><td><table><tr><td>b</td><td>c</td></tr>"
           "<tr><td>d</td></tr></table></td></tr></table>", "a\t\nb\tc\nd");
    expect("a row read alone", "<table><tr id=r><td>1</td><td>2</td></tr></table>", "1\t2");
    expect("a caption", "<table id=r><caption>cap</caption><tr><td>1</td></tr></table>", "cap\n1");
    expect("a cell's own spaces", "<table id=r><tr><td> a </td><td> b </td></tr></table>", "a\tb");
    expect("pre keeps everything", "<div id=r><pre>  a\n  b </pre></div>", "  a\n  b ");
    expect("pre-line keeps line feeds", "<div id=r data-ws=l>a  \n  b\r\nc</div>", "a\nb\nc");
    expect("the root not rendered", "<div id=r data-t=n>text</div>", NULL);
    expect("script and style give nothing", "<div id=r>a<script>x()</script><style>p{}</style>b</div>", "ab");
    expect("read ON a script: not rendered, so its textContent", "<div><script id=r>source()</script></div>", NULL);
    expect("counts in a run take the largest", "<div id=r><p>a</p><div>b</div></div>", "a\n\nb");
    expect("two blocks", "<div id=r><div>a</div><div>b</div></div>", "a\nb");
    expect("leading and trailing counts go", "<div id=r><div><div>a</div></div></div>", "a");
    expect("display contents", "<div id=r>a<span data-t=c>b</span>c</div>", "abc");
    expect("replaced elements' children are no text",
           "<div id=r>a<video>fallback</video>b<canvas>c</canvas><textarea>t</textarea>d</div>", "abd");
    expect("empty", "<div id=r></div>", "");
    expect("white space alone", "<div id=r>   \n  </div>", "");
    expect("a space at a block's end goes", "<div id=r><span>a </span></div>", "a");
    expect("an inline-block's spaces", "<div id=r>a <span data-t=i>b</span> c</div>", "a b c");
    expect("a br at a block's end, then a block", "<div id=r><div>a<br></div><div>b</div></div>", "a\n\nb");
    expect("a list", "<ul id=r><li>one</li> <li>two</li></ul>", "one\ntwo");
    expect("svg's text is no HTML text", "<div id=r>a<svg><text>s</text></svg>b</div>", "ab");
    expect("utf-8 passes through", "<div id=r>caf\xc3\xa9 <b>\xe2\x82\xac</b></div>", "caf\xc3\xa9 \xe2\x82\xac");
    /* The cold review's cases (each WPT's or the standard's answer). */
    expect("table white space goes under pre too", "<div id=r><table data-ws=p>  <tr><td>abc</td>  </tr>  </table></div>",
           "abc");
    expect("a pre around a table", "<pre id=r><table><tr><td>a</td> <td>b</td></tr></table></pre>", "a\tb");
    expect("a caption after the rows", "<div id=r><table><tr><td>abc<caption>def</caption></table></div>", "abc\ndef");
    expect("a hidden row's visible cell is still its last", "<table id=r><tr data-v=h><td data-v=v>abc</td></tr></table>",
           "abc");
    expect("a hidden table's visible rows: the last is last",
           "<div id=r><table data-v=h><tr data-v=v><td>a</td></tr><tr data-v=v><td>b</td></tr></table></div>", "a\nb");
    expect("a hidden last cell is still last, so the one before keeps its tab",
           "<table id=r><tr><td>a</td><td data-v=h>b</td></tr></table>", "a\t");
    expect("a p's two whatever its display", "<div id=r>a<p data-t=i>b</p>c</div>", "a\n\nb\n\nc");
    expect("a row group read alone, the table's last", "<table><tbody id=r><tr><td>a</td></tr><tr><td>b</td></tr></tbody></table>",
           "a\nb");
    expect("a row group read alone, an empty group after it",
           "<table><tbody id=r><tr><td>a</td></tr></tbody><tbody></tbody></table>", "a");
    expect("a row group read alone, rows after it", "<table><tbody id=r><tr><td>a</td></tr></tbody><tbody><tr><td>b</td></tr></tbody></table>",
           "a\n");
    expect("a preserved line feed ends a line (before)", "<div id=r>a <span data-ws=p>\nb</span></div>", "a\nb");
    expect("a preserved line feed ends a line (after)", "<div id=r><span data-ws=p>a\n</span> b</div>", "a\nb");
    shadow_case();
    shadow_text_case();
    deep_case(depth);
    memory_case();
    refusal_case();
    printf("inner text host: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
