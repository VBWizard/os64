// test_wend_host.c — wend's renderer, on the host.
//
// The renderer is pure computation (userland/apps/wend/render.h), so it can
// be driven here under the sanitizers instead of through a boot: a tree and a
// width in, lines out. Two jobs:
//
//   --checks            the fold's table, the wrapper's edges, the invariant
//                       every painted byte depends on
//   --render FILE ...   one corpus page through libhtml and the renderer,
//                       dumped in a form a person can read in a diff
//
// The dump is the point of the second one. A rendering change arrives as a
// changed LINE on a named page, which is reviewable; "it looks different on
// my screen" is not.

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/wend/render.h"
#include "html/html.h"
#include "page/page.h"
#include "os64/str.h"
#include "os64/url.h"

// libos64's formatter is linked for the URL resolver's sake and reaches for
// the write syscall on the printing paths nothing here calls. Answering "all
// of it went out" keeps the link honest without inventing an output.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// ── The allocator the library and the renderer share ────────────────────

static size_t live, allocations, fail_at;

void *os64_malloc(size_t n)
{
    allocations++;
    if (fail_at && allocations >= fail_at)
        return NULL;
    void *p = malloc(n ? n : 1);
    if (p)
        live++;
    return p;
}

void *os64_calloc(size_t n, size_t s)
{
    if (s && n > SIZE_MAX / s)
        return NULL;
    void *p = os64_malloc(n * s);
    if (p)
        memset(p, 0, n * s);
    return p;
}

void *os64_realloc(void *old, size_t n)
{
    allocations++;
    if (fail_at && allocations >= fail_at)
        return NULL;
    void *p = realloc(old, n ? n : 1);
    if (p && !old)
        live++;
    return p;
}

void os64_free(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

// ── Checks ──────────────────────────────────────────────────────────────

static int checks, failures;

#define CHECK(c) do { \
    checks++; \
    if (!(c)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    } \
} while (0)

// The geometry borrows source nodes and the model borrows the tree. Keep each
// helper's document and model alive until its rendered page is released,
// including multi-width test pairs.
typedef struct TestDocument {
    wend_page_t *page;
    os64_html_document_t *doc;
    os64_page_t *model;
    struct TestDocument *next;
} TestDocument;
static TestDocument *test_documents;
static void test_page_free(wend_page_t *page)
{
    TestDocument **at = &test_documents;
    while (*at != NULL && (*at)->page != page) at = &(*at)->next;
    if (*at != NULL) {
        TestDocument *entry = *at;
        *at = entry->next;
        os64_page_free(entry->model);
        os64_html_document_free(entry->doc);
        free(entry);
    }
    wend_page_free(page);
}
#define wend_page_free test_page_free

// The model a helper-rendered page was drawn from.
static os64_page_t *model_of(const wend_page_t *page)
{
    for (TestDocument *at = test_documents; at != NULL; at = at->next)
        if (at->page == page)
            return at->model;
    return NULL;
}

// The same document and model drawn again at another width, the way a
// resize draws it. The page returned borrows them from `page`'s entry.
static wend_page_t *redraw(const wend_page_t *page, int32_t cols)
{
    for (TestDocument *at = test_documents; at != NULL; at = at->next)
        if (at->page == page)
            return wend_render_html(at->doc, at->model, cols, NULL, 0);
    return NULL;
}

// Where spot `i`, a link, goes — the model's answer. "" when it has none.
static const char *link_url(const wend_page_t *page, int32_t i)
{
    const os64_page_link_t *link = os64_page_link(model_of(page), page->spots[i].link);
    return link != NULL && link->href.url != NULL ? link->href.url : "";
}

static const os64_page_link_t *link_of(const wend_page_t *page, int32_t i)
{
    return os64_page_link(model_of(page), page->spots[i].link);
}

static const os64_page_control_t *control_at(const wend_page_t *page, int32_t i)
{
    return os64_page_control(model_of(page), page->spots[i].control);
}

// Fixture lookup only: production navigation resolves names in libpage.
static int32_t fixture_node_line(const wend_page_t *page, const char *name)
{
    if (page == NULL) return -1;
    for (int32_t i = 0; i < page->nnode_rows; i++) {
        const os64_html_node_t *node = page->node_rows[i].node;
        const os64_html_attr_t *id = os64_html_attr(node, "id");
        const os64_html_attr_t *legacy = os64_html_attr(node, "name");
        if ((id && strcmp(id->value, name) == 0) ||
            (node->tag == OS64_HTML_TAG_A && legacy && strcmp(legacy->value, name) == 0))
            return wend_node_line(page, node);
    }
    return -1;
}

// Parse, build the model the browser would, and draw it. `page_url` is the
// address the document came from, which is what the model resolves against
// unless the page carries a `<base href>` of its own.
static wend_page_t *render_html_text(const char *html, int32_t cols, const char *page_url)
{
    os64_html_parser_t *p = os64_html_parser_new(NULL);
    if (!p)
        return NULL;
    if (os64_html_parser_feed(p, html, strlen(html)) < 0) {
        os64_html_parser_destroy(p);
        return NULL;
    }
    os64_html_document_t *doc = os64_html_parser_finish(p);
    if (!doc)
        return NULL;
    os64_page_t *model = os64_page_build(doc, page_url ? page_url : "http://h/d/p.html", NULL);
    wend_page_t *page = wend_render_html(doc, model, cols, NULL, 0);
    TestDocument *entry = page != NULL ? malloc(sizeof(*entry)) : NULL;
    if (entry != NULL) {
        *entry = (TestDocument){page, doc, model, test_documents};
        test_documents = entry;
    } else {
        if (page != NULL)
            abort();
        os64_page_free(model);
        os64_html_document_free(doc);
    }
    return page;
}

// Every byte a line carries is one printable cell. The painter counts bytes
// to know where a colour starts and stops, and the terminal obeys anything
// below 0x20, so this is the invariant that keeps a stranger's page from
// steering the glass.
static void check_printable(const wend_page_t *page)
{
    for (int32_t i = 0; i < page->nlines; i++) {
        const char *t = page->lines[i].text;
        CHECK((int32_t)strlen(t) == page->lines[i].len);
        for (int32_t j = 0; j < page->lines[i].len; j++) {
            unsigned char b = (unsigned char)t[j];
            CHECK((b >= 0x20 && b < 0x7F) || b >= 0xA0);
        }
        int32_t at = 0;
        for (int32_t j = 0; j < page->lines[i].nruns; j++) {
            CHECK((int32_t)page->lines[i].runs[j].start == at);
            at += (int32_t)page->lines[i].runs[j].len;
        }
        CHECK(at == page->lines[i].len);
    }
}

static const char *line_of(const wend_page_t *page, int32_t i)
{
    return i < page->nlines ? page->lines[i].text : "<past the end>";
}

static void expect_lines(const char *what, const char *html, int32_t cols,
                         const char *const *want, int32_t n)
{
    wend_page_t *page = render_html_text(html, cols, "http://h/d/p.html");
    if (!page) {
        failures++;
        fprintf(stderr, "FAIL %s: no page\n", what);
        return;
    }
    check_printable(page);
    bool same = page->nlines == n;
    for (int32_t i = 0; same && i < n; i++)
        same = strcmp(page->lines[i].text, want[i]) == 0;
    checks++;
    if (!same) {
        failures++;
        fprintf(stderr, "FAIL %s: got %d lines\n", what, page->nlines);
        for (int32_t i = 0; i < page->nlines || i < n; i++)
            fprintf(stderr, "   %2d got |%s|\n      want |%s|\n", i,
                    i < page->nlines ? line_of(page, i) : "",
                    i < n ? want[i] : "");
    }
    wend_page_free(page);
}

static void fold_checks(void)
{
    char out[WEND_FOLD_MAX];
    struct { uint32_t cp; const char *as; } cases[] = {
        { 'A', "A" }, { 0x7E, "~" }, { 0xE9, "\xE9" }, { 0xFF, "\xFF" },
        { 0x00A0, " " }, { 0x2018, "'" }, { 0x2019, "'" }, { 0x201C, "\"" },
        { 0x201D, "\"" }, { 0x2013, "-" }, { 0x2014, "-" }, { 0x2026, "..." },
        { 0x2022, "*" }, { 0x2190, "<" }, { 0x2192, ">" }, { 0x2191, "^" },
        { 0x2193, "v" }, { 0x2122, "(tm)" }, { 0x00A9, "\xA9" },
        { 0x00AD, "" }, { 0x200B, "" }, { 0xFEFF, "" },
        // The two control ranges and everything with no glyph: one mark
        { 0x00, "?" }, { 0x1B, "?" }, { 0x7F, "?" }, { 0x80, "?" },
        { 0x9B, "?" }, { 0x9F, "?" }, { 0x4E2D, "?" }, { 0x1F600, "?" },
        { 0x10FFFF, "?" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = wend_fold(cases[i].cp, out);
        checks++;
        if (n != strlen(cases[i].as) || memcmp(out, cases[i].as, n) != 0) {
            failures++;
            fprintf(stderr, "FAIL fold U+%04X -> %.*s\n", cases[i].cp, (int)n, out);
        }
    }
    // Every entry of the table folds to printable bytes, or to nothing.
    for (uint32_t cp = 0; cp < 0x11000; cp++) {
        size_t n = wend_fold(cp, out);
        CHECK(n <= WEND_FOLD_MAX);
        for (size_t j = 0; j < n; j++) {
            unsigned char b = (unsigned char)out[j];
            CHECK((b >= 0x20 && b < 0x7F) || b >= 0xA0);
        }
    }
}

static void render_checks(void)
{
    {
        const char *want[] = { "Hello there" };
        expect_lines("collapse", "<p>Hello\n\n   there</p>", 40, want, 1);
    }
    {
        // A paragraph either side of a heading: one blank between, none at
        // the top and none trailing.
        const char *want[] = { "One", "", "Head", "", "Two" };
        expect_lines("blanks", "<p>One<h2>Head</h2><p>Two", 40, want, 5);
    }
    {
        const char *want[] = { "aaa bbb", "ccc" };
        expect_lines("wrap", "<p>aaa bbb ccc", 7, want, 2);
    }
    {
        // A word wider than the screen is the only one this splits, and it
        // splits at the margin.
        const char *want[] = { "aaaaa", "bbbbb", "cc" };
        expect_lines("hard wrap", "<p>aaaaabbbbbcc", 5, want, 3);
    }
    {
        // A list is indented, so its items sit two columns in.
        const char *want[] = { "  * one", "  * two" };
        expect_lines("bullets", "<ul><li>one<li>two</ul>", 40, want, 2);
    }
    {
        const char *want[] = { "  1. one", "  2. two" };
        expect_lines("counter", "<ol><li>one<li>two</ol>", 40, want, 2);
    }
    {
        // A wrapped item lines up under its own text, not under its bullet.
        const char *want[] = { "  * aaaa", "    bbbb" };
        expect_lines("hanging", "<ul><li>aaaa bbbb</ul>", 10, want, 2);
    }
    {
        const char *want[] = { "[1]here" };
        expect_lines("link mark", "<a href=x>here</a>", 40, want, 1);
    }
    {
        const char *want[] = { "a  b" };
        expect_lines("cells", "<table><tr><td>a<td>b</table>", 40, want, 1);
    }
    {
        const char *want[] = { "line", "", "next" };
        expect_lines("double br", "line<br><br>next", 40, want, 3);
    }
    {
        const char *want[] = { " two   spaces ", "", "kept" };
        expect_lines("pre", "<pre> two   spaces \n\nkept</pre>", 40, want, 3);
    }
    {
        const char *want[] = { "a       b" };
        expect_lines("pre tab", "<pre>a\tb</pre>", 40, want, 1);
    }
    {
        // The author's trailing spaces survive to the last row of a `pre`,
        // and a flowed row keeps none of the renderer's own.
        const char *want[] = { "a", "b  " };
        expect_lines("pre tail", "<pre>a\nb  </pre>", 40, want, 2);
    }
    {
        const char *want[] = { "one", "two" };
        expect_lines("trim", "<div>one   <div>two   ", 40, want, 2);
    }
    {
        // A quote's own margin holds until its last word is down.
        const char *want[] = { "    aaaa", "    bbbb" };
        expect_lines("quote wrap", "<blockquote>aaaa bbbb</blockquote>", 10, want, 2);
    }
    {
        // Two images with no whitespace between them are one word, because
        // that is what the markup says.
        const char *want[] = { "[alt text][image]" };
        expect_lines("images", "<img alt='alt text'><img>", 40, want, 1);
    }
    {
        // An EMPTY alt is the page saying "this one is decoration": it draws
        // nothing at all, where an absent alt draws that a picture is there.
        const char *want[] = { "before after" };
        expect_lines("decorative", "before <img alt=''> after", 40, want, 1);
    }
    {
        const char *want[] = { "shown" };
        expect_lines("noscript", "<noscript>shown</noscript><script>hidden</script>"
                     "<style>hidden</style>", 40, want, 1);
    }
    {
        const char *want[] = { "before after" };
        expect_lines("foreign", "before <svg><text>inside</text></svg> after", 40, want, 1);
    }
    {
        // Every control a person can land on wears the number they can type,
        // and a hidden field draws nothing at all.
        const char *want[] = { "[1][____][2][Go]" };
        expect_lines("widgets", "<form><input name=q size=4>"
                     "<input type=hidden name=h value=1>"
                     "<input type=submit value=Go></form>", 40, want, 1);
    }
    {
        // A `datalist` is suggestions for a box, never shown: the box is the
        // only place to land.
        const char *want[] = { "[1][____]" };
        expect_lines("datalist", "<input name=q size=4 list=l>"
                     "<datalist id=l><option value=a>A<input name=inner></datalist>",
                     40, want, 1);
    }
    {
        // HTML inside SVG is the page's own: drawn and landed on, while the
        // SVG's own text stays undrawn.
        const char *want[] = { "[1]inside[2][____]" };
        expect_lines("foreign html", "<svg><text>token</text><foreignObject><form>"
                     "<a href=/x>inside</a><input name=q size=4></form></foreignObject></svg>",
                     40, want, 1);
    }
    {
        // A `details` is its summary, and the summary is a toggle: closed,
        // nothing it folds is drawn or landed on; open, all of it is. A
        // closed `dialog` is nothing at all.
        const char *want[] = { "[1]> Title", "[2]v Details", "[3]open" };
        expect_lines("details", "<details><summary>Title</summary><a href=/s>secret</a>"
                     "<input name=x></details><details open><a href=/o>open</a></details>"
                     "<dialog><a href=/d>boxed</a></dialog>", 40, want, 3);
        // The READER's flips turn each the other way, and the page is not
        // touched to do it.
        wend_page_t *page = render_html_text("<details><summary>T</summary><a href=/s>in</a>"
                                             "</details><details open><a href=/o>o</a></details>",
                                             40, "http://host/p");
        CHECK(page != NULL && page->nspots == 3 && page->spots[0].kind == WEND_SPOT_TOGGLE);
        if (page && page->nspots == 3) {
            const os64_html_node_t *flips[2] = { page->spots[0].node, page->spots[1].node };
            CHECK(!wend_details_open(flips[0], NULL, 0) && wend_details_open(flips[1], NULL, 0));
            CHECK(wend_details_open(flips[0], flips, 2) && !wend_details_open(flips[1], flips, 2));
            TestDocument *entry = test_documents;
            while (entry != NULL && entry->page != page)
                entry = entry->next;
            wend_page_t *turned = entry ? wend_render_html(entry->doc, entry->model, 40, flips, 2)
                                        : NULL;
            CHECK(turned != NULL && turned->nlines == 3 &&
                  strcmp(turned->lines[0].text, "[1]v T") == 0 &&
                  strcmp(turned->lines[1].text, "[2]in") == 0 &&
                  strcmp(turned->lines[2].text, "[3]> Details") == 0);
            wend_page_free(turned);
        }
        wend_page_free(page);
    }
    {
        // A textarea is as wide as its `cols`, 20 without one.
        const char *want[] = { "[1][" "__________" "]", "[2][" "____________________" "]" };
        expect_lines("textarea cols", "<textarea cols=10></textarea><br>"
                     "<textarea size=3></textarea>", 80, want, 2);
    }
    {
        // A box shows what is in it, padded to its width.
        const char *want[] = { "[1][cat_____]" };
        expect_lines("box value", "<input name=q size=8 value=cat>", 40, want, 1);
    }
    {
        // Ticked and not, and a list showing the option it would send.
        const char *want[] = { "[1][x][2]( )[3][v Two]" };
        expect_lines("ticks", "<input type=checkbox checked><input type=radio>"
                     "<select><option>One<option selected>Two</select>",
                     40, want, 1);
    }

    // Where a link goes, and where it is on the page.
    wend_page_t *page = render_html_text(
        "<p>one<p><a href='/two'>two</a><p><a href='http://other/x'>three</a>",
        40, "http://host/dir/page.html");
    CHECK(page != NULL);
    if (page) {
        CHECK(page->nspots == 2);
        CHECK(page->nspots == 2 && strcmp(link_url(page, 0), "http://host/two") == 0);
        CHECK(page->nspots == 2 && strcmp(link_url(page, 1), "http://other/x") == 0);
        // one, blank, [1]two, blank, [2]three
        CHECK(page->nspots == 2 && page->spots[0].line == 2);
        CHECK(page->nspots == 2 && page->spots[1].line == 4);
        wend_page_free(page);
    }

    // A <base href> is the page's own word about where it lives, and it wins
    // over the address the reply came from — in the model, where every link
    // is resolved, so the renderer never needs to know a base exists.
    wend_page_t *page2 = render_html_text(
        "<head><base href='http://elsewhere/sub/'><title> A  long\n"
        "title </title></head><body><a href=rel>r</a>", 40, "http://host/dir/page.html");
    CHECK(page2 != NULL);
    if (page2) {
        CHECK(strcmp(page2->title, "A long title") == 0);
        CHECK(page2->nspots == 1 && strcmp(link_url(page2, 0), "http://elsewhere/sub/rel") == 0);
        wend_page_free(page2);
    }

    // A FRAMESET PAGE IS ITS FRAMES, each one a link the model resolved —
    // against the page's `<base>` like any other reference.
    wend_page_t *frames = render_html_text(
        "<head><base href='http://other/dir/'></head>"
        "<frameset><frame name=menu src=menu.html><frame src=body.html#top>"
        "<frame name=nothing></frameset>", 40, "http://host/p");
    CHECK(frames != NULL && frames->nspots == 2);
    if (frames && frames->nspots == 2) {
        CHECK(frames->nlines == 2 && strcmp(frames->lines[0].text, "[1]frame: menu") == 0 &&
              strcmp(frames->lines[1].text, "[2]frame: body.html#top") == 0);
        CHECK(strcmp(link_url(frames, 0), "http://other/dir/menu.html") == 0);
        CHECK(strcmp(link_url(frames, 1), "http://other/dir/body.html") == 0 &&
              strcmp(link_of(frames, 1)->href.fragment, "top") == 0);
    }
    wend_page_free(frames);

    // text/plain, both ways round: the same byte is a character in Latin-1
    // and half a character in UTF-8.
    const char latin[] = { 'c', 'a', 'f', (char)0xE9, '\n', 'x', '\0' };
    wend_page_t *t1 = wend_render_text(latin, 6, false, 40);
    CHECK(t1 && t1->nlines == 2);
    CHECK(t1 && strcmp(t1->lines[0].text, "caf\xE9") == 0);
    wend_page_free(t1);
    wend_page_t *t2 = wend_render_text("caf\xC3\xA9\n", 6, true, 40);
    CHECK(t2 && t2->nlines == 1 && strcmp(t2->lines[0].text, "caf\xE9") == 0);
    wend_page_free(t2);
    // A text file whose lines end in carriage returns still has lines, and a
    // CRLF pair is one ending rather than two.
    wend_page_t *cr = wend_render_text("a\rb\r\nc\n", 7, false, 40);
    CHECK(cr && cr->nlines == 3);
    if (cr && cr->nlines == 3) {
        CHECK(strcmp(cr->lines[0].text, "a") == 0);
        CHECK(strcmp(cr->lines[1].text, "b") == 0);
        CHECK(strcmp(cr->lines[2].text, "c") == 0);
    }
    wend_page_free(cr);

    // An invalid sequence is one mark per bad byte, never a dropped byte.
    wend_page_t *t3 = wend_render_text("a\xC3\x28" "b", 4, true, 40);
    CHECK(t3 && t3->nlines == 1 && strcmp(t3->lines[0].text, "a?(b") == 0);
    wend_page_free(t3);
}

// ── What a form would send ──────────────────────────────────────────────

// What a spot activates: a GET address, a POST body, no form, or a
// refusal because its target will not fit or resolve.
typedef enum {
    WANT_SENT = 0,
    WANT_NO_FORM,
    WANT_POST,
    WANT_TOO_LONG,
    WANT_BAD_ACTION,
} want_t;

// Ask the model what activating spot `spot` does, the way the browser asks:
// a button is PRESSED, and any other control is FINISHED — the standard's
// implicit submission, which libpage answers with the form's default button.
static os64_page_verdict_t send_spot(const wend_page_t *page, int32_t spot,
                                     os64_page_request_t *request)
{
    os64_page_what_t what = { OS64_PAGE_ACTIVATE_IMPLICIT, page->spots[spot].control, 0, 0 };
    if (page->spots[spot].kind == WEND_SPOT_SUBMIT)
        what.how = OS64_PAGE_ACTIVATE_CONTROL;
    return os64_page_activate(model_of(page), what, request);
}

// Render, then ask what activating one spot would ask for. The page URL is
// the one a form with no action of its own falls back to.
static void expect_url(const char *what, const char *html, int32_t spot,
                       want_t want, const char *want_url)
{
    const char *page_url = "http://host/dir/page.html?old=1";
    wend_page_t *page = render_html_text(html, 80, page_url);
    checks++;
    if (!page || spot >= page->nspots) {
        failures++;
        fprintf(stderr, "FAIL %s: no page, or no spot %d\n", what, spot);
        if (page)
            wend_page_free(page);
        return;
    }
    os64_page_request_t request;
    os64_page_verdict_t verdict = send_spot(page, spot, &request);
    bool ok;
    switch (want) {
        case WANT_SENT:
            ok = verdict == OS64_PAGE_NAVIGATE && request.method == OS64_PAGE_METHOD_GET &&
                 (want_url == NULL || strcmp(request.url, want_url) == 0);
            break;
        case WANT_POST:
            ok = verdict == OS64_PAGE_NAVIGATE && request.method == OS64_PAGE_METHOD_POST;
            break;
        case WANT_NO_FORM:
            ok = verdict == OS64_PAGE_NOTHING && request.reason == OS64_PAGE_REASON_NO_FORM;
            break;
        case WANT_TOO_LONG:
            ok = verdict == OS64_PAGE_REFUSED && request.reason == OS64_PAGE_REASON_TOO_LONG;
            break;
        default:
            ok = verdict == OS64_PAGE_REFUSED && request.reason == OS64_PAGE_REASON_BAD_ACTION;
            break;
    }
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s: verdict %d reason %s method %d url |%s|\n"
                        "            want %d |%s|\n",
                what, (int)verdict, os64_page_reason_name(request.reason), (int)request.method,
                request.url ? request.url : "", (int)want, want_url ? want_url : "");
    }
    os64_page_request_free(&request);
    wend_page_free(page);
}

// The same question, for the verdicts that send nothing at all.
static void expect_nothing(const char *what, const char *html, int32_t spot,
                           os64_page_reason_t want)
{
    wend_page_t *page = render_html_text(html, 80, "https://host/dir/page.html");
    checks++;
    if (!page || spot >= page->nspots) {
        failures++;
        fprintf(stderr, "FAIL %s: no page, or no spot %d\n", what, spot);
        if (page)
            wend_page_free(page);
        return;
    }
    os64_page_request_t request;
    os64_page_verdict_t verdict = send_spot(page, spot, &request);
    if (verdict == OS64_PAGE_NAVIGATE || request.reason != want) {
        failures++;
        fprintf(stderr, "FAIL %s: verdict %d reason %s url |%s|, want %s\n", what,
                (int)verdict, os64_page_reason_name(request.reason),
                request.url ? request.url : "", os64_page_reason_name(want));
    }
    os64_page_request_free(&request);
    wend_page_free(page);
}

// A `#name` is kept beside the address and answered by moving. These are the
// two halves: the fragment survives resolution, and the page knows which row
// each name fell on.
static void anchor_checks(void)
{
    wend_page_t *page = render_html_text(
        "<p><a href='#two'>to two</a><h2 id=one>One</h2><p>text"
        "<h2 id=two>Two</h2><p>more<p><a name=old></a>after"
        "<p><a href='other.html#frag'>elsewhere</a>",
        40, "http://host/dir/page.html");
    CHECK(page != NULL);
    if (!page)
        return;
    CHECK(page->nspots == 2);
    if (page->nspots == 2) {
        // The address drops a fragment, correctly; the link keeps it.
        CHECK(strcmp(link_url(page, 0), "http://host/dir/page.html") == 0);
        CHECK(strcmp(link_of(page, 0)->href.fragment, "two") == 0);
        CHECK(link_of(page, 0)->same_document);
        CHECK(strcmp(link_url(page, 1), "http://host/dir/other.html") == 0);
        CHECK(strcmp(link_of(page, 1)->href.fragment, "frag") == 0);
    }
    CHECK(fixture_node_line(page, "one") >= 0);
    CHECK(fixture_node_line(page, "two") > fixture_node_line(page, "one"));
    CHECK(fixture_node_line(page, "old") >= 0);      // the 1994 spelling
    CHECK(fixture_node_line(page, "nothing") == -1);
    // Following the table-of-contents link is a MOVE to the node the model
    // names, and that node has a row on this page.
    if (page->nspots == 2) {
        os64_page_request_t request;
        os64_page_what_t what = { OS64_PAGE_ACTIVATE_LINK, page->spots[0].link, 0, 0 };
        CHECK(os64_page_activate(model_of(page), what, &request) == OS64_PAGE_FRAGMENT);
        CHECK(wend_node_line(page, request.anchor) == fixture_node_line(page, "two"));
        os64_page_request_free(&request);
    }
    // The row an anchor names is the row its element begins on, so a reader
    // sent there finds the heading at the top rather than above the screen.
    int32_t at = fixture_node_line(page, "two");
    CHECK(at >= 0 && at < page->nlines && strcmp(page->lines[at].text, "Two") == 0);
    wend_page_free(page);
}

static void form_checks(void)
{
    // The plain search box: the action resolved, the hidden field carried,
    // the box's value sent, the nameless button sending nothing.
    expect_url("search", "<form action=/s><input type=hidden name=h value=1>"
               "<input name=q value=cats><input type=submit value=Go></form>",
               1, WANT_SENT, "http://host/s?h=1&q=cats");
    // A form with no action of its own goes back to the page — and a GET
    // form REPLACES the query that page already carried.
    expect_url("no action", "<form><input name=q value=x>"
               "<input type=submit name=go value=now></form>",
               1, WANT_SENT, "http://host/dir/page.html?q=x&go=now");
    // Spaces become plus, everything else that is not unreserved becomes
    // percent-and-two-digits, over the UTF-8 bytes the page itself holds —
    // which is why the page has to say it is UTF-8 for this to be the
    // encoding of one accented letter rather than of two Windows-1252 ones.
    expect_url("encoding", "<meta charset=utf-8>"
               "<form action=/s><input name='a b' value='c d/\xc3\xa9'>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?a+b=c+d%2F%C3%A9");
    // A box that is not ticked is not sent; one with no value of its own
    // sends `on`.
    expect_url("ticks", "<form action=/s><input type=checkbox name=a checked>"
               "<input type=checkbox name=b><input type=checkbox name=c value=yes checked>"
               "<input type=submit></form>",
               3, WANT_SENT, "http://host/s?a=on&c=yes");
    // A list sends the option's value, not the words shown for it.
    expect_url("choice", "<form action=/s><select name=pick>"
               "<option value=1>One<option value=2 selected>Two</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?pick=2");
    // Two buttons, and only the one pressed says it was.
    expect_url("which button", "<form action=/s><input type=submit name=go value=up>"
               "<input type=submit name=go value=down></form>",
               1, WANT_SENT, "http://host/s?go=down");
    expect_url("post request", "<form action=/s method=POST><input name=pw>"
               "<input type=submit></form>", 1, WANT_POST, NULL);
    expect_url("no form", "<input name=loose>", 0, WANT_NO_FORM, NULL);
    // A `dialog` form CLOSES A DIALOG and no server hears of it — whoever
    // says so, the form or the button. Sent as a GET, it put a password in
    // an address.
    expect_nothing("dialog form", "<form method=dialog action=/s>"
                   "<input type=password name=pw value=secret>"
                   "<input type=submit name=go value=Go></form>",
                   1, OS64_PAGE_REASON_DIALOG);
    expect_nothing("dialog button", "<form action=/s><input type=password name=pw value=s>"
                   "<button formmethod=dialog>Close</button></form>",
                   1, OS64_PAGE_REASON_DIALOG);
    // FINISHING A BOX PRESSES THE FORM'S DEFAULT BUTTON, and a disabled one
    // means the form is not sent at all, rather than sent as though it had
    // no button.
    expect_nothing("disabled default button", "<form action=/s><input name=q value=x>"
                   "<input type=submit disabled></form>",
                   0, OS64_PAGE_REASON_DEFAULT_BUTTON_DISABLED);

    // A DISABLED CONTROL IS DRAWN, IS NOT LANDED ON, AND IS NOT SENT. The
    // only spot on this page is the button, so the query carries nothing of
    // the two controls the page took away.
    expect_url("disabled", "<form action=/s><input name=a value=1 disabled>"
               "<input type=hidden name=h value=2 disabled>"
               "<input name=b value=3><input type=submit></form>",
               1, WANT_SENT, "http://host/s?b=3");
    {
        const char *want[] = { "[1___][1][___][2][Go]" };
        expect_lines("disabled drawn", "<form><input name=a value=1 size=4 disabled>"
                     "<input name=b size=3><input type=submit value=Go></form>",
                     40, want, 1);
    }
    {
        // A password is a length and nothing else, on the page and in the
        // prompt both.
        const char *want[] = { "[1][****______]" };
        expect_lines("password", "<input type=password value=hunt size=10>",
                     40, want, 1);
    }
    {
        // A textarea's contents are its VALUE: collapsed for the one row it
        // is drawn on, kept whole for the wire.
        wend_page_t *page = render_html_text(
            "<form action=/s><textarea name=note cols=10>two  spaces\nand a line"
            "</textarea><input type=submit></form>", 80, "http://host/p");
        CHECK(page && page->nspots == 2);
        if (page && page->nspots == 2) {
            CHECK(strcmp(control_at(page, 0)->value, "two  spaces\nand a line") == 0);
            os64_page_request_t request;
            CHECK(send_spot(page, 1, &request) == OS64_PAGE_NAVIGATE);
            // The wire spells a line break CRLF, whatever the page's own
            // bytes were.
            CHECK(request.url && strcmp(request.url,
                                        "http://host/s?note=two++spaces%0D%0Aand+a+line") == 0);
            os64_page_request_free(&request);
        }
        if (page)
            wend_page_free(page);
    }

    // A BUTTON MAY OVERRULE ITS FORM. The method decides whether this is a
    // submission wend performs at all, and the action decides where.
    expect_url("formmethod", "<form action=/s><input name=pw value=secret>"
               "<input type=submit formmethod=post></form>",
               1, WANT_POST, NULL);
    expect_url("formaction", "<form action=/s method=get><input name=q value=x>"
               "<input type=submit formaction=/other></form>",
               1, WANT_SENT, "http://host/other?q=x");
    // An IMAGE button sends where you clicked, which from a keyboard is the
    // origin — and never its value, which is what a server expecting
    // `name.x` is written against. With no name the fields are plain `x` and
    // `y`: the coordinates are how the button says it was the one pressed,
    // so a nameless one saying nothing would say nothing AT ALL.
    expect_url("image button", "<form action=/s><input name=q value=x>"
               "<input type=image name=go src=go.gif value=ignored></form>",
               1, WANT_SENT, "http://host/s?q=x&go.x=0&go.y=0");
    expect_url("image nameless", "<form action=/s><input name=q value=x>"
               "<input type=image src=go.gif></form>",
               1, WANT_SENT, "http://host/s?q=x&x=0&y=0");

    // THE FORM DATA SET GOES OUT IN TREE ORDER, hidden fields included —
    // visible only when two controls share a name, which is exactly when a
    // server is counting on it.
    expect_url("tree order", "<form action=/s><input name=k value=1>"
               "<input type=hidden name=k value=2><input name=k value=3>"
               "<input type=hidden name=k value=4><input type=submit></form>",
               2, WANT_SENT, "http://host/s?k=1&k=2&k=3&k=4");

    // A DISABLED OPTION is shown, never stepped onto, and never sent — which
    // is exactly what a "choose one" placeholder needs.
    expect_url("placeholder", "<form action=/s><select name=pick>"
               "<option disabled selected value=none>Choose one"
               "<option value=1>One</select><input type=submit></form>",
               1, WANT_SENT, "http://host/s?");
    expect_url("optgroup off", "<form action=/s><select name=pick>"
               "<optgroup disabled><option value=1 selected>One</optgroup>"
               "<option value=2>Two</select><input type=submit></form>",
               1, WANT_SENT, "http://host/s?");
    {
        // The placeholder is what the list SHOWS, all the same.
        const char *want[] = { "[1][v Choose one][2][Submit]" };
        expect_lines("placeholder drawn", "<form><select name=p>"
                     "<option disabled selected>Choose one<option>One</select>"
                     "<input type=submit></form>", 40, want, 1);
    }
    // READONLY is not DISABLED: the page keeps the value fixed and the value
    // still goes.
    expect_url("readonly sent", "<form action=/s><input name=a value=1 readonly>"
               "<input type=submit></form>", 1, WANT_SENT, "http://host/s?a=1");
    // A button's own action brings its own section with it.
    {
        wend_page_t *page = render_html_text(
            "<form action='/s#form'><input name=q value=x>"
            "<input type=submit formaction='/other#results'></form>",
            80, "http://host/p");
        CHECK(page && page->nspots == 2);
        if (page && page->nspots == 2) {
            os64_page_request_t request;
            CHECK(send_spot(page, 1, &request) == OS64_PAGE_NAVIGATE);
            CHECK(request.url && strcmp(request.url, "http://host/other?q=x") == 0);
            CHECK(request.has_fragment && strcmp(request.fragment, "results") == 0);
            os64_page_request_free(&request);
        }
        wend_page_free(page);
    }
    // A BASE WHOSE PORT IS ITS SCHEME'S DEFAULT resolves as if it named no
    // port — the shape a fetch hands over, and the one that would otherwise
    // spell `host:443` into every relative link on the page.
    {
        wend_page_t *page = render_html_text("<a href='rel.html'>r</a><a href='#x'>f</a>",
                                             40, "https://host:443/dir/page.html");
        CHECK(page && page->nspots == 2);
        if (page && page->nspots == 2) {
            CHECK(strcmp(link_url(page, 0), "https://host/dir/rel.html") == 0);
            CHECK(strcmp(link_url(page, 1), "https://host/dir/page.html") == 0);
            CHECK(link_of(page, 1)->same_document);
        }
        wend_page_free(page);
    }

    // `#` with nothing after it asked for a fragment; an empty href did not.
    {
        wend_page_t *page = render_html_text("<a href='#'>top</a><a href=''>again</a>",
                                             40, "http://host/p");
        CHECK(page && page->nspots == 2);
        if (page && page->nspots == 2) {
            CHECK(link_of(page, 0)->href.has_fragment && link_of(page, 0)->href.fragment[0] == '\0');
            CHECK(!link_of(page, 1)->href.has_fragment);
            CHECK(strcmp(link_url(page, 0), "http://host/p") == 0);
        }
        wend_page_free(page);
    }

    // An empty href names the page it is on, which is what a reload link is.
    {
        wend_page_t *page = render_html_text("<a href=''>again</a>", 40,
                                             "http://host/dir/page.html?q=1");
        CHECK(page && page->nspots == 1);
        if (page && page->nspots == 1)
            CHECK(strcmp(link_url(page, 0), "http://host/dir/page.html?q=1") == 0);
        wend_page_free(page);
    }

    // A `button` is a submit too, and carries the same overrules. Before
    // this was true, activating one read a pointer the renderer never set.
    expect_url("button method", "<form action=/s><input name=pw value=x>"
               "<button formmethod=post>Log in</button></form>",
               1, WANT_POST, NULL);
    expect_url("button action", "<form action=/s><input name=q value=x>"
               "<button formaction=/other>Go</button></form>",
               1, WANT_SENT, "http://host/other?q=x");
    expect_url("button plain", "<form action=/s><input name=q value=x>"
               "<button name=go value=now>Go</button></form>",
               1, WANT_SENT, "http://host/s?q=x&go=now");

    // A fieldset disables everything under it — except the words in its
    // first legend, which were never a control.
    expect_url("fieldset off", "<form action=/s><fieldset disabled>"
               "<legend>Title</legend><input name=a value=1></fieldset>"
               "<input name=b value=2><input type=submit></form>",
               1, WANT_SENT, "http://host/s?b=2");
    {
        // The fieldset is a block, so its row ends with it; the box inside
        // is drawn and unnumbered, the ones after it are spots.
        const char *want[] = { "Title[___]", "[1][___][2][Submit]" };
        expect_lines("fieldset drawn", "<form><fieldset disabled><legend>Title</legend>"
                     "<input name=a size=3></fieldset><input name=b size=3>"
                     "<input type=submit></form>", 40, want, 2);
    }
    // A form action can name a section, which the address cannot carry.
    {
        wend_page_t *page = render_html_text(
            "<form action='/s#results'><input name=q value=x>"
            "<input type=submit></form>", 80, "http://host/p");
        CHECK(page && page->nspots == 2);
        if (page && page->nspots == 2) {
            os64_page_request_t request;
            CHECK(send_spot(page, 1, &request) == OS64_PAGE_NAVIGATE);
            CHECK(request.url && strcmp(request.url, "http://host/s?q=x") == 0);
            CHECK(request.has_fragment && strcmp(request.fragment, "results") == 0);
            os64_page_request_free(&request);
        }
        wend_page_free(page);
    }

    // WHAT A PERSON TYPED SURVIVES A RE-WRAP, which is the whole reason the
    // edits live in the model and not in the lines: a page drawn again at
    // another width shows the value, and the form sends it.
    wend_page_t *first = render_html_text("<form action=/s><input name=q size=6 value=old>"
                                          "<input type=submit value=Go></form>",
                                          80, "http://host/p");
    CHECK(first != NULL && first->nspots == 2);
    if (first && first->nspots == 2) {
        CHECK(os64_page_set_text(model_of(first), first->spots[0].control, "new cat", 7) == 0);
        wend_page_t *again = redraw(first, 40);
        CHECK(again != NULL && again->nspots == 2);
        if (again) {
            CHECK(again->nlines > 0 && strstr(again->lines[0].text, "[ew cat]") != NULL);
            wend_page_free(again);
        }
        os64_page_request_t request;
        CHECK(send_spot(first, 1, &request) == OS64_PAGE_NAVIGATE);
        CHECK(request.url && strcmp(request.url, "http://host/s?q=new+cat") == 0);
        os64_page_request_free(&request);
    }
    wend_page_free(first);
}

// An allocation failure anywhere leaves a page that is honest about being
// partial, frees clean, and never hands back a line the painter cannot draw.
// The raw-text path, which has its own answer to a byte over 0x7F and its
// own idea of where a line ends.
static void text_checks(void)
{
    {
        // NOT UTF-8 MEANS WINDOWS-1252, the same answer libhtml gives the
        // markup half: 0x93/0x94 are the quotation marks a word processor
        // produces, which the fold spells `"`, and NOT the C1 controls
        // Latin-1 keeps there, which would every one of them draw as `?`.
        const char bytes[] = "he said \x93hello\x94 \x97 and \x85";
        wend_page_t *page = wend_render_text(bytes, strlen(bytes), false, 80);
        CHECK(page != NULL && page->nlines == 1);
        if (page) {
            CHECK(strcmp(page->lines[0].text, "he said \"hello\" - and ...") == 0);
            check_printable(page);
            wend_page_free(page);
        }
    }
    {
        // A byte the 1252 table does not move is still its own code point,
        // which is what keeps an accented name readable.
        const char bytes[] = "caf\xE9";
        wend_page_t *page = wend_render_text(bytes, strlen(bytes), false, 80);
        CHECK(page != NULL && page->nlines == 1);
        if (page) {
            CHECK(strcmp(page->lines[0].text, "caf\xE9") == 0);
            wend_page_free(page);
        }
    }
    {
        // Every way a text file ends a line, and a CRLF pair counted once.
        const char bytes[] = "a\rb\r\nc\nd";
        wend_page_t *page = wend_render_text(bytes, strlen(bytes), true, 80);
        CHECK(page != NULL && page->nlines == 4);
        if (page && page->nlines == 4) {
            CHECK(strcmp(page->lines[0].text, "a") == 0);
            CHECK(strcmp(page->lines[1].text, "b") == 0);
            CHECK(strcmp(page->lines[2].text, "c") == 0);
            CHECK(strcmp(page->lines[3].text, "d") == 0);
        }
        wend_page_free(page);
    }
}

// A list's numbers are the page's to choose, and an anchor names the row its
// own words are on.
static void list_and_anchor_checks(void)
{
    {
        // `start` begins the count and `value` moves it, taking the items
        // after it along — which is how a procedure carries on across a
        // paragraph.
        const char *want[] = { "  5. five", "  6. six", "  9. nine", "  10. ten" };
        expect_lines("ol start", "<ol start=5><li>five<li>six"
                     "<li value=9>nine<li>ten</ol>", 40, want, 4);
    }
    {
        // A nested list counts on its own; the outer one carries on where it
        // left off.
        const char *want[] = { "  1. one", "       1. inner", "  2. two" };
        expect_lines("ol nested", "<ol><li>one<ol><li>inner</ol><li>two</ol>",
                     40, want, 3);
    }
    {
        // A number longer than the type holds saturates rather than wraps,
        // and a negative one is drawn as the page wrote it.
        const char *want[] = { "  2147483647. big", "  2147483647. also" };
        expect_lines("ol saturating start",
                     "<ol start=99999999999999><li>big<li>also</ol>", 40, want, 2);
    }
    {
        const char *want[] = { "  -3. minus", "  -2. next" };
        expect_lines("ol negative start", "<ol start=-3><li>minus<li>next</ol>",
                     40, want, 2);
    }
    {
        // AN INLINE `id` NAMES THE ROW IT IS ON. It is met with the row
        // already open, so waiting for the next row to open would send a
        // reader past the words they asked for.
        wend_page_t *page = render_html_text(
            "<p>before <span id=target>here</span> after</p><p>next</p>",
            40, "http://host/p");
        CHECK(page != NULL);
        if (page) {
            int32_t at = fixture_node_line(page, "target");
            CHECK(at >= 0 && at < page->nlines
                  && strstr(page->lines[at].text, "here") != NULL);
            wend_page_free(page);
        }
    }
}

// The parts of a form that are not where they look, not what they look, or
// not visible at all.
static void form_edge_checks(void)
{
    // A CONTROL MAY NAME ITS FORM rather than sit inside it, and one that
    // names a form nothing answers to belongs to NO form — never to whatever
    // it happens to be written inside.
    expect_url("form attribute", "<form id=f action=/s></form>"
               "<input form=f name=q value=x><input type=submit form=f>",
               1, WANT_SENT, "http://host/s?q=x");
    expect_url("form attribute hidden", "<form id=f action=/s></form>"
               "<input type=hidden form=f name=h value=1>"
               "<input type=submit form=f>",
               0, WANT_SENT, "http://host/s?h=1");
    expect_url("form attribute unmatched", "<form action=/s>"
               "<input form=nosuch name=q value=x><input type=submit></form>",
               1, WANT_SENT, "http://host/s?");
    {
        wend_page_t *page = render_html_text(
            "<form action=/s><input form=nosuch name=q value=x></form>", 80,
            "http://host/p");
        CHECK(page && page->nspots == 1 && control_at(page, 0)->form == -1);
        // A control in no form has nowhere to be sent.
        if (page && page->nspots == 1) {
            os64_page_request_t request;
            CHECK(send_spot(page, 0, &request) == OS64_PAGE_NOTHING &&
                  request.reason == OS64_PAGE_REASON_NO_FORM);
            os64_page_request_free(&request);
        }
        wend_page_free(page);
    }
    // AN EMPTY `formaction` IS THE PAGE ITSELF, and an absent one is the
    // form's — the string alone cannot tell them apart.
    expect_url("formaction empty", "<form action=/other><input name=q value=x>"
               "<input type=submit formaction=''></form>",
               1, WANT_SENT, "http://host/dir/page.html?q=x");
    expect_url("formaction absent", "<form action=/other><input name=q value=x>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/other?q=x");
    // A TICK THAT SAYS `value=""` ASKED FOR AN EMPTY ANSWER; only one with
    // no value at all takes the `on` the standard supplies.
    expect_url("empty tick value", "<form action=/s>"
               "<input type=checkbox name=a value='' checked>"
               "<input type=checkbox name=b checked><input type=submit></form>",
               2, WANT_SENT, "http://host/s?a=&b=on");
    // A PAGE THAT MARKS TWO OF A GROUP HAS PICKED THE LAST ONE. Leaving both
    // would send a server the opposite of what it reads first.
    expect_url("radio group", "<form action=/s><input type=radio name=x value=a checked>"
               "<input type=radio name=x value=b checked>"
               "<input type=radio name=y value=c checked><input type=submit></form>",
               3, WANT_SENT, "http://host/s?x=b&y=c");
    {
        // ...and the ROW agrees, because the group is settled before the
        // control is drawn: a page showing two dots where one value goes is
        // the same lie in a different place.
        const char *want[] = { "[1]( )[2](*)[3](*)[4][Submit]" };
        expect_lines("radio group drawn",
                     "<form><input type=radio name=x value=a checked>"
                     "<input type=radio name=x value=b checked>"
                     "<input type=radio name=y value=c checked>"
                     "<input type=submit></form>", 40, want, 1);
    }
    {
        // A radio in no form is not in a group with radios inside one, and a
        // disabled member counts toward which one the group ends on.
        // The form is a block, so it takes its own row; the disabled one is
        // drawn and is not a spot, and being LAST it is what the group ends
        // on — so the enabled radio before it shows empty.
        const char *want[] = { "[1](*)", "[2]( )(*)" };
        expect_lines("radio groups apart",
                     "<input type=radio name=x value=a checked>"
                     "<form><input type=radio name=x value=b checked>"
                     "<input type=radio name=x value=c checked disabled></form>",
                     40, want, 2);
    }
    {
        // A FRAGMENT IS PERCENT-ENCODED AND THE `id` IT NAMES IS NOT, so the
        // one stored is decoded — every heading with a space in its id
        // depends on it.
        wend_page_t *page = render_html_text(
            "<p><a href='#section%202'>go</a><h2 id='section 2'>Two</h2>",
            40, "http://host/p");
        CHECK(page != NULL && page && page->nspots == 1);
        if (page && page->nspots == 1) {
            CHECK(strcmp(link_of(page, 0)->href.fragment, "section 2") == 0);
            CHECK(fixture_node_line(page, link_of(page, 0)->href.fragment) >= 0);
            wend_page_free(page);
        }
    }
    {
        // A COUNTDOWN COUNTS DOWN, starting at as many items as it has.
        const char *want[] = { "  3. C", "  2. B", "  1. A" };
        expect_lines("ol reversed", "<ol reversed><li>C<li>B<li>A</ol>",
                     40, want, 3);
    }
    {
        // A countdown reaches the most negative number there is, and stays.
        const char *want[] = { "  -2147483647. A", "  -2147483648. B", "  -2147483648. C" };
        expect_lines("ol reversed floor", "<ol reversed start=-2147483647><li>A<li>B<li>C</ol>",
                     40, want, 3);
    }
    {
        // ...and an explicit `start` still outranks the item count.
        const char *want[] = { "  9. C", "  8. B" };
        expect_lines("ol reversed start", "<ol reversed start=9><li>C<li>B</ol>",
                     40, want, 2);
    }
    {
        // `xmp` is the 1993 spelling of `pre`, and the pages still using it
        // are the ones whose columns are the meaning.
        const char *want[] = { "first", "  second" };
        expect_lines("xmp preformatted", "<xmp>first\n  second</xmp>", 40, want, 2);
    }
    {
        // A nameless radio is in no group: it sends nothing and cancels
        // nothing, so the page's own marks all stand.
        const char *want[] = { "[1](*)[2](*)" };
        expect_lines("radio no name",
                     "<form><input type=radio checked><input type=radio checked></form>",
                     40, want, 1);
    }
    // A group is per FORM, so the same name in two forms is two groups.
    expect_url("radio per form", "<form action=/s><input type=radio name=x value=a checked>"
               "<input type=submit></form>"
               "<form action=/t><input type=radio name=x value=b checked>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?x=a");
    // A `multiple` list sends every option the page marked, in the page's
    // own order.
    expect_url("multi select", "<form action=/s><select name=p multiple>"
               "<option value=1 selected>One<option value=2>Two"
               "<option value=3 selected>Three</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?p=1&p=3");
    {
        // ...and says on the row that there is more than one.
        const char *want[] = { "[1][v One +1][2][Submit]" };
        expect_lines("multi select drawn", "<form><select name=p multiple>"
                     "<option selected>One<option>Two<option selected>Three"
                     "</select><input type=submit></form>", 40, want, 1);
    }
    {
        // PICKING IS ONE THING EVEN ON A `multiple` LIST: the list cycles,
        // so one key says "this one" and the page's several give way to it.
        // These are the edits wend's step makes; the row and the wire must
        // then agree on the one.
        wend_page_t *page = render_html_text(
            "<form action=/s><select name=p multiple>"
            "<option value=1 selected>One<option value=2>Two"
            "<option value=3 selected>Three</select>"
            "<input type=submit></form>", 80, "http://host/p");
        CHECK(page != NULL && page->nspots == 2);
        if (page && page->nspots == 2) {
            os64_page_t *model = model_of(page);
            int32_t list = page->spots[0].control;
            CHECK(os64_page_set_chosen(model, list, 1, true) == 0);
            CHECK(os64_page_set_chosen(model, list, 0, false) == 0);
            CHECK(os64_page_set_chosen(model, list, 2, false) == 0);
            wend_page_t *again = redraw(page, 80);
            CHECK(again && again->nlines > 0 && strstr(again->lines[0].text, "[v Two]"));
            wend_page_free(again);
            os64_page_request_t request;
            CHECK(send_spot(page, 1, &request) == OS64_PAGE_NAVIGATE);
            CHECK(request.url && strcmp(request.url, "http://host/s?p=2") == 0);
            os64_page_request_free(&request);
        }
        wend_page_free(page);
    }
    {
        // A `multiple` list shows an option that will actually GO. With
        // nothing marked it holds nothing and is drawn empty; showing its
        // first would read exactly like a list sending that option.
        const char *want[] = { "[1][v ][2][Submit]" };
        expect_lines("multi select empty", "<form><select name=p multiple>"
                     "<option value=1>One<option value=2>Two</select>"
                     "<input type=submit></form>", 40, want, 1);
    }
    expect_url("multi select empty sends", "<form action=/s><select name=p multiple>"
               "<option value=1>One<option value=2>Two</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?");
    {
        // ...and a disabled first choice is not what the row shows either,
        // because it is not what goes.
        const char *want[] = { "[1][v B][2][Submit]" };
        expect_lines("multi select disabled first", "<form><select name=p multiple>"
                     "<option value=x selected disabled>X<option value=b selected>B"
                     "</select><input type=submit></form>", 40, want, 1);
    }
    expect_url("multi select disabled first sends",
               "<form action=/s><select name=p multiple>"
               "<option value=x selected disabled>X<option value=b selected>B"
               "</select><input type=submit></form>",
               1, WANT_SENT, "http://host/s?p=b");
    // A SUBMIT CONTROL NOBODY CAN PRESS IS STILL THE FORM'S DEFAULT BUTTON,
    // and finishing the form's one box presses it — method, action and all.
    // Out of sight, it still decides that the first form POSTS (which wend
    // sends with a body) and that the second goes where the second says.
    expect_url("hidden default button posts", "<form action=/s>"
               "<div hidden><button formmethod=post>Go</button></div>"
               "<input name=q value=x></form>", 0, WANT_POST, NULL);
    expect_url("hidden default button sends", "<form action=/t><input name=q value=x>"
               "<div hidden><input type=submit name=b value=v></div></form>",
               0, WANT_SENT, "http://host/t?q=x&b=v");
    // A GROUP IS EVERY RADIO OF ONE NAME WITH ONE FORM OWNER, so two
    // root-level radios naming different forms are two groups and neither
    // cancels the other.
    expect_url("radio by owner", "<form id=a action=/s></form><form id=b action=/t></form>"
               "<input type=radio form=a name=x value=1 checked>"
               "<input type=radio form=b name=x value=2 checked>"
               "<input type=submit form=a>",
               2, WANT_SENT, "http://host/s?x=1");
    // A SUBMIT CONTROL OUT OF REACH BELONGS TO THE FORM IT NAMES, not to the
    // document it happens to sit in.
    expect_url("hidden default button by owner", "<form id=a action=/s></form>"
               "<div hidden><button form=a formmethod=post>Go</button></div>"
               "<input form=a name=pw value=secret>", 0, WANT_POST, NULL);
    // A DISABLED FIELDSET'S FIRST `legend` IS LIVE wherever the section is
    // drawn, which includes not being drawn at all.
    expect_url("hidden legend", "<form action=/s><div hidden>"
               "<fieldset disabled><legend><input name=x value=y></legend>"
               "<input name=off value=1></fieldset></div>"
               "<input type=submit></form>",
               0, WANT_SENT, "http://host/s?x=y");
    {
        // A STATED DESTINATION THAT WILL NOT RESOLVE IS A REFUSAL, never the
        // page it is on: that is a different host to be wrong about. An
        // address longer than one may be is how a page reaches this.
        char html[OS64_URL_REF_MAX + 256], huge[OS64_URL_REF_MAX + 64];
        memset(huge, 'a', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        snprintf(html, sizeof(html), "<form action=/%s><input name=q value=x>"
                 "<input type=submit></form>", huge);
        expect_url("bad form action", html, 1, WANT_TOO_LONG, NULL);
        snprintf(html, sizeof(html), "<form action=/s><input name=q value=x>"
                 "<input type=submit formaction=/%s></form>", huge);
        expect_url("bad button action", html, 1, WANT_TOO_LONG, NULL);
    }
    // A `_charset_` field is the FORM's answer, not the page's: the encoding
    // the values go out in, which is the document's — windows-1252 for a
    // page that names none.
    expect_url("charset field", "<form action=/s>"
               "<input type=hidden name=_charset_><input name=q value=x>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?_charset_=windows-1252&q=x");
    // An option's `label` is its words; its VALUE still falls back to the
    // element's text, never to the label.
    {
        const char *want[] = { "[1][v Short]" };
        expect_lines("option label", "<form><select name=p>"
                     "<option label=Short>Long fallback</select></form>",
                     40, want, 1);
    }
    expect_url("option label value", "<form action=/s><select name=p>"
               "<option label=Short>Long fallback</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?p=Long+fallback");
    // A list drawn as several ROWS is not a drop-down: it holds nothing
    // until the page marks something, so nothing is invented for it.
    expect_url("select sized", "<form action=/s><select name=x size=2>"
               "<option value=a>A<option value=b>B</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?");
    expect_url("select dropdown", "<form action=/s><select name=x>"
               "<option value=a>A<option value=b>B</select>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?x=a");
    // A SUBTREE THE PAGE MARKED `hidden` DRAWS NOTHING AND IS NO PLACE TO
    // LAND — and its values still go with the form, which is what a page
    // that puts a section away relies on.
    {
        const char *want[] = { "shown[1][__________][2][Submit]" };
        expect_lines("hidden subtree drawn",
                     "<form><div hidden><p>secret prose<a href=/p>private</a>"
                     "<input name=internal value=v></div>shown"
                     "<input name=q size=10><input type=submit></form>",
                     40, want, 1);
    }
    // A control may BE the hidden element, with no children at all.
    expect_url("hidden control itself", "<form action=/s>"
               "<input name=away value=v hidden><input name=q value=x>"
               "<input type=submit></form>",
               1, WANT_SENT, "http://host/s?away=v&q=x");
    // A disabled control is not sent whether it is on the screen or not.
    expect_url("hidden and disabled", "<form action=/s>"
               "<div hidden><input name=a value=1 disabled>"
               "<fieldset disabled><input name=b value=2></fieldset>"
               "<input name=c value=3></div>"
               "<input type=submit></form>",
               0, WANT_SENT, "http://host/s?c=3");
    expect_url("hidden subtree sent",
               "<form action=/s><div hidden><input name=internal value=v>"
               "<input type=checkbox name=t value=1 checked>"
               "<input type=checkbox name=u value=1>"
               "<select name=pick><option value=a><option value=b selected></select>"
               "<textarea name=note>kept</textarea>"
               "<input type=submit name=btn value=no></div>"
               "<input name=q value=x><input type=submit></form>",
               1, WANT_SENT,
               "http://host/s?internal=v&t=1&pick=b&note=kept&q=x");
    {
        // A `hidden` subtree is not a form's excuse to forget where things
        // stood: the values go out where the page wrote them.
        wend_page_t *page = render_html_text(
            "<form action=/s><input name=q value=first>"
            "<div hidden><input name=q value=second></div>"
            "<input type=submit></form>", 80, "http://host/p");
        CHECK(page != NULL && page->nspots == 2);
        if (page && page->nspots == 2) {
            os64_page_request_t request;
            CHECK(send_spot(page, 1, &request) == OS64_PAGE_NAVIGATE);
            CHECK(request.url && strcmp(request.url, "http://host/s?q=first&q=second") == 0);
            os64_page_request_free(&request);
        }
        wend_page_free(page);
    }
    // WHITESPACE AT AN ELEMENT BOUNDARY IS STILL WHITESPACE. An option's
    // words are gathered as one sequence, so `A<b> B</b>` is two words in
    // what is shown AND in what is sent.
    expect_url("gather boundary", "<form action=/s><select name=p>"
               "<option>A<b> B</b></select><input type=submit></form>",
               1, WANT_SENT, "http://host/s?p=A+B");
    {
        const char *want[] = { "[1][v A B][2][Submit]" };
        expect_lines("gather boundary drawn", "<form><select name=p>"
                     "<option>A<b> B</b></select><input type=submit></form>",
                     40, want, 1);
    }
    {
        // THE FIRST `base` CARRYING AN href WINS, empty or not: an empty one
        // names the document, and skipping it would hand every relative link
        // on the page to the later base's host.
        wend_page_t *page = render_html_text(
            "<head><base href=''><base href='http://other/'></head>"
            "<body><a href=x>link</a>", 40, "http://host/dir/p.html");
        CHECK(page != NULL && page->nspots == 1);
        if (page && page->nspots == 1)
            CHECK(strcmp(link_url(page, 0), "http://host/dir/x") == 0);
        wend_page_free(page);
    }
}

// A PAGE IS A LEVER. Forty thousand marked radios in one form once ran past
// two minutes in a group search a person could not interrupt; the model
// settles a group once, and a page this size must draw one dot and send the
// one value the page's last mark chose.
static void big_group_checks(void)
{
    size_t n = 20000, cap = n * 48 + 64, at = 0;
    char *html = malloc(cap);
    CHECK(html != NULL);
    if (!html)
        return;
    at += (size_t)snprintf(html + at, cap - at, "<form action=/s>");
    for (size_t i = 0; i < n; i++)
        at += (size_t)snprintf(html + at, cap - at,
                               "<input type=radio name=x value=%zu checked>", i);
    snprintf(html + at, cap - at, "</form>");
    wend_page_t *page = render_html_text(html, 80, "http://host/p");
    CHECK(page != NULL);
    if (page) {
        CHECK(page->nspots == (int32_t)n);
        int32_t on = 0;
        for (int32_t i = 0; i < page->nlines; i++)
            for (const char *dot = page->lines[i].text; (dot = strstr(dot, "(*)")); dot++)
                on++;
        CHECK(on == 1);
        os64_page_request_t request;
        CHECK(send_spot(page, 0, &request) == OS64_PAGE_NAVIGATE);
        CHECK(request.url && strcmp(request.url, "http://host/s?x=19999") == 0);
        os64_page_request_free(&request);
        wend_page_free(page);
    }
    free(html);
}

static void oom_checks(void)
{
    // THE FORM MACHINERY UNDER FAILURE. The model and the renderer both
    // allocate as they go, so a refusal at any step can leave a page
    // half-built or a model missing — and a NUMBER typed at the status row
    // reaches any spot, drawn or not. What comes back must be paintable,
    // answerable and free-clean at every one of those steps, and the sweep
    // is as long as a clean run's allocations so a longer build cannot
    // quietly shorten it.
    static const char oom_page[] =
        "<h2 id=here>Form</h2><form id=f action='/s#r'>"
        "<input type=hidden name=h value=1><input name=q value=x size=4>"
        "<select name=p multiple><option disabled selected>Pick"
        "<option value=2 selected>Two<b> Deux</b></select>"
        "<input type=checkbox name=c checked><textarea name=t>a\nb</textarea>"
        "<div hidden><input name=away value=1><select name=s>"
        "<option value=z selected>Z</select><textarea name=n>hush</textarea></div>"
        "<button formmethod=post name=go formaction=''>Send</button></form>"
        "<input form=f name=outside value=o>"
        "<input type=radio form=f name=r value=1 checked>"
        "<select name=lab size=2><option label=Short>Long</select>"
        "<ol reversed><li>one<li>two</ol>"
        "<a href='#he%72e'>back up</a>";
    allocations = 0;
    wend_page_t *clean = render_html_text(oom_page, 40, "http://host/p");
    size_t steps = allocations;
    CHECK(clean != NULL && !clean->incomplete && clean->nspots > 5);
    wend_page_free(clean);
    for (size_t n = 1; n <= steps + 1; n++) {
        allocations = 0;
        fail_at = n;
        wend_page_t *page = render_html_text(oom_page, 40, "http://host/p");
        if (page) {
            check_printable(page);
            // Every spot is asked what it would do; none may fault.
            for (int32_t i = 0; i < page->nspots; i++) {
                os64_page_request_t request;
                if (page->spots[i].kind == WEND_SPOT_LINK) {
                    os64_page_what_t what = { OS64_PAGE_ACTIVATE_LINK, page->spots[i].link, 0, 0 };
                    (void)os64_page_activate(model_of(page), what, &request);
                } else {
                    (void)send_spot(page, i, &request);
                }
                os64_page_request_free(&request);
            }
            (void)fixture_node_line(page, "here");
            wend_page_free(page);
        }
        fail_at = 0;
    }
    CHECK(live == 0);

    for (size_t n = 1; n < 400; n++) {
        allocations = 0;
        fail_at = n;
        wend_page_t *page = render_html_text(
            "<h1>Title</h1><p>Some words that wrap at this width</p>"
            "<ul><li><a href=/a>one</a><li><a href=/b>two</a></ul>"
            "<pre>  kept\n  as is</pre>", 20, "http://host/p");
        if (page) {
            check_printable(page);
            wend_page_free(page);
        }
        fail_at = 0;
    }
    CHECK(live == 0);
}

// ── The dump ────────────────────────────────────────────────────────────

static void dump(const wend_page_t *page, const os64_page_t *model, const char *name)
{
    printf("page: %s\n", name);
    printf("title: %s\n", page->title);
    printf("width: %d\n", page->cols);
    printf("lines: %d\n", page->nlines);
    printf("spots: %d\n", page->nspots);
    printf("forms: %d\n", os64_page_nforms(model));
    printf("node rows: %d\n", page->nnode_rows);
    printf("incomplete: %s\n", page->incomplete ? "yes" : "no");
    printf("--\n");
    for (int32_t i = 0; i < page->nlines; i++) {
        printf("|%s\n", page->lines[i].text);
        char attrs[4096];
        size_t at = 0;
        for (int32_t j = 0; j < page->lines[i].nruns; j++) {
            const wend_run_t *run = &page->lines[i].runs[j];
            if (!run->attrs && !run->spot)
                continue;
            char one[64];
            char flags[24];
            size_t f = 0;
            if (run->attrs & WEND_ATTR_BOLD)
                flags[f++] = 'B';
            if (run->attrs & WEND_ATTR_UNDERLINE)
                flags[f++] = 'U';
            flags[f] = '\0';
            if (run->spot)
                snprintf(one, sizeof(one), " %s%c%d:%u-%u", flags,
                         page->spots[run->spot - 1].kind == WEND_SPOT_LINK ? 'L' : 'F',
                         run->spot, run->start, run->start + run->len);
            else
                snprintf(one, sizeof(one), " %s:%u-%u", flags, run->start,
                         run->start + run->len);
            size_t len = strlen(one);
            if (at + len + 1 < sizeof(attrs)) {
                memcpy(attrs + at, one, len);
                at += len;
            }
        }
        attrs[at] = '\0';
        if (at)
            printf("|~%s\n", attrs);
    }
    printf("--\n");
    // The spots, in the order the keyboard walks them, and what the MODEL
    // says each one is. A link shows where it goes; a control shows what it
    // would send, because that is the half a reader of this dump cannot get
    // from the rows above.
    static const char *const kinds[] = { "link", "text", "check", "radio",
                                         "choice", "submit", "toggle" };
    for (int32_t i = 0; i < page->nspots; i++) {
        const wend_spot_t *spot = &page->spots[i];
        printf("%d %s", i + 1, kinds[spot->kind]);
        if (spot->kind == WEND_SPOT_LINK) {
            const os64_page_link_t *link = os64_page_link(model, spot->link);
            printf(" %s", link && link->href.url ? link->href.url : "(unresolved)");
            // A fragment that was ASKED for prints even when it is empty:
            // `#` alone means the top, and it is a different behaviour from
            // no fragment at all.
            if (link && link->href.has_fragment)
                printf(" #%s", link->href.fragment ? link->href.fragment : "");
            printf("\n");
            continue;
        }
        const os64_page_control_t *c = os64_page_control(model, spot->control);
        if (c == NULL) {
            printf(" (no control)\n");
            continue;
        }
        if (c->form >= 0)
            printf(" form=%d", c->form + 1);
        if (c->overrides.action.spelled)
            printf(" formaction=%s", c->overrides.action.url ? c->overrides.action.url
                                                             : "(unresolved)");
        if (c->input == OS64_PAGE_INPUT_PASSWORD)
            printf(" secret");
        if (c->input == OS64_PAGE_INPUT_IMAGE)
            printf(" image");
        printf(" name=%s", c->name && c->name[0] ? c->name : "(none)");
        if (c->readonly)
            printf(" readonly");
        switch (spot->kind) {
            case WEND_SPOT_CHECK:
            case WEND_SPOT_RADIO:
                printf(" %s value=%s", c->checked ? "on" : "off", c->value);
                break;
            case WEND_SPOT_CHOICE:
                printf(" of %d%s", c->noptions, c->multiple ? " multiple" : "");
                for (int32_t j = 0; j < c->noptions; j++)
                    printf(" [%s=%s%s%s]", c->options[j].label, c->options[j].value,
                           c->options[j].selected ? " on" : "",
                           c->options[j].disabled ? " off" : "");
                break;
            default:
                printf(" value=%s", c->value);
                break;
        }
        printf("\n");
    }
    static const char *const methods[] = { "get", "post", "dialog" };
    for (int32_t i = 0; i < os64_page_nforms(model); i++) {
        const os64_page_form_t *form = os64_page_form(model, i);
        printf("form %d %s %s", i + 1, methods[form->method],
               !form->action.spelled || (form->action.url && !form->action.url[0])
                   ? "(this page)"
                   : form->action.url ? form->action.url : "(unresolved)");
        if (form->id && form->id[0])
            printf(" id=%s", form->id);
        // The values it carries and never shows: a hidden field, or a
        // control the page put out of sight. In tree order, which is the
        // order they are sent in.
        for (int32_t j = 0; j < os64_page_ncontrols(model); j++) {
            const os64_page_control_t *c = os64_page_control(model, j);
            bool tick = c->input == OS64_PAGE_INPUT_CHECKBOX || c->input == OS64_PAGE_INPUT_RADIO;
            if (c->form == i && (c->input == OS64_PAGE_INPUT_HIDDEN || c->hidden_subtree) &&
                !c->disabled && (!tick || c->checked) && c->name && c->name[0])
                printf(" %s=%s", c->name, c->value);
        }
        printf("\n");
    }
    // Node identities are process-local; snapshot row geometry by index.
    if (page->nnode_rows <= 40)
        for (int32_t i = 0; i < page->nnode_rows; i++)
            printf("node %d line %d\n", i, page->node_rows[i].line);
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    size_t cap = 1 << 16, at = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (at == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        size_t got = fread(buf + at, 1, cap - at, f);
        at += got;
        if (got == 0)
            break;
    }
    fclose(f);
    *len = at;
    return buf;
}

int main(int argc, char **argv)
{
    const char *file = NULL, *base_text = NULL, *charset = NULL, *name = "page";
    int32_t cols = 80;
    bool do_checks = false, plain = false, latin1 = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--checks"))
            do_checks = true;
        else if (!strcmp(argv[i], "--render") && i + 1 < argc)
            file = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) {
            file = argv[++i];
            plain = true;
        } else if (!strcmp(argv[i], "--latin1"))
            latin1 = true;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--base") && i + 1 < argc)
            base_text = argv[++i];
        else if (!strcmp(argv[i], "--charset") && i + 1 < argc)
            charset = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            name = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--checks] [--render FILE | --text FILE]"
                            " [--width N] [--base URL] [--charset NAME]"
                            " [--name NAME] [--latin1]\n", argv[0]);
            return 2;
        }
    }

    if (do_checks) {
        fold_checks();
        render_checks();
        text_checks();
        list_and_anchor_checks();
        form_checks();
        form_edge_checks();
        big_group_checks();
        anchor_checks();
        oom_checks();
        printf("renderer: %d checks, %d failures, %zu blocks live\n",
               checks, failures, live);
        if (live != 0)
            failures++;
        return failures ? 1 : 0;
    }

    if (!file) {
        fprintf(stderr, "nothing to do\n");
        return 2;
    }

    size_t len = 0;
    char *bytes = slurp(file, &len);
    wend_page_t *page = NULL;
    os64_html_document_t *doc = NULL;
    os64_page_t *model = NULL;
    if (plain) {
        page = wend_render_text(bytes, len, !latin1, cols);
    } else {
        os64_html_options_t opt = os64_html_options_default();
        opt.charset = charset;
        os64_html_parser_t *p = os64_html_parser_new(&opt);
        if (!p || os64_html_parser_feed(p, bytes, len) < 0) {
            fprintf(stderr, "parse failed\n");
            return 1;
        }
        doc = os64_html_parser_finish(p);
        if (!doc) {
            fprintf(stderr, "no document\n");
            return 1;
        }
        // The model resolves against where the page came from — and against
        // its own <base href> where it has one, exactly as in the browser.
        model = os64_page_build(doc, base_text ? base_text : "", NULL);
        page = wend_render_html(doc, model, cols, NULL, 0);
    }
    if (!page) {
        fprintf(stderr, "no page\n");
        return 1;
    }
    check_printable(page);
    dump(page, model, name);
    wend_page_free(page);
    os64_page_free(model);
    if (doc)
        os64_html_document_free(doc);
    free(bytes);
    if (failures) {
        fprintf(stderr, "%d invariant failures\n", failures);
        return 1;
    }
    return 0;
}
