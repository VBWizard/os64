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

static wend_page_t *render_html_text(const char *html, int32_t cols, const char *base_text)
{
    os64_url_t base;
    memset(&base, 0, sizeof(base));
    if (base_text && os64_url_parse(base_text, &base) != OS64_URL_OK) {
        fprintf(stderr, "bad base %s\n", base_text);
        exit(2);
    }
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
    wend_page_t *page = wend_render_html(doc, base_text ? &base : NULL, cols, NULL, 0);
    os64_html_document_free(doc);
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
        CHECK(page->nspots == 2 && strcmp(page->spots[0].url, "http://host/two") == 0);
        CHECK(page->nspots == 2 && strcmp(page->spots[1].url, "http://other/x") == 0);
        // one, blank, [1]two, blank, [2]three
        CHECK(page->nspots == 2 && page->spots[0].line == 2);
        CHECK(page->nspots == 2 && page->spots[1].line == 4);
        wend_page_free(page);
    }

    // A <base href> is the page's own word about where it lives, and it wins
    // over the address the reply came from.
    os64_html_parser_t *p = os64_html_parser_new(NULL);
    CHECK(p != NULL);
    const char *doc_text = "<head><base href='http://elsewhere/sub/'><title> A  long\n"
                           "title </title></head><body><a href=rel>r</a>";
    if (p && os64_html_parser_feed(p, doc_text, strlen(doc_text)) >= 0) {
        os64_html_document_t *doc = os64_html_parser_finish(p);
        CHECK(doc != NULL);
        char href[256];
        CHECK(doc && wend_base_href(doc, href, sizeof(href)));
        CHECK(strcmp(href, "http://elsewhere/sub/") == 0);
        os64_url_t base;
        CHECK(os64_url_parse(href, &base) == OS64_URL_OK);
        wend_page_t *page2 = wend_render_html(doc, &base, 40, NULL, 0);
        CHECK(page2 != NULL);
        if (page2) {
            CHECK(strcmp(page2->title, "A long title") == 0);
            CHECK(page2->nspots == 1 &&
                  strcmp(page2->spots[0].url, "http://elsewhere/sub/rel") == 0);
            wend_page_free(page2);
        }
        os64_html_document_free(doc);
    }

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

// Render, then ask what activating one spot would ask for. The page URL is
// the one a form with no action of its own falls back to.
static void expect_url(const char *what, const char *html, int32_t spot,
                       wend_form_result_t want_result, const char *want_url)
{
    const char *page_url = "http://host/dir/page.html?old=1";
    wend_page_t *page = render_html_text(html, 80, page_url);
    checks++;
    if (!page) {
        failures++;
        fprintf(stderr, "FAIL %s: no page\n", what);
        return;
    }
    char url[2048], frag[256];
    url[0] = '\0';
    wend_form_result_t got = wend_form_url(page, spot, page_url, url, sizeof(url), frag, sizeof(frag));
    if (got != want_result || (want_url && strcmp(url, want_url) != 0)) {
        failures++;
        fprintf(stderr, "FAIL %s: result %d url |%s|\n            want %d |%s|\n",
                what, (int)got, url, (int)want_result, want_url ? want_url : "");
    }
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
        // The resolver drops a fragment, correctly; the spot keeps it.
        CHECK(strcmp(page->spots[0].url, "http://host/dir/page.html") == 0);
        CHECK(strcmp(page->spots[0].fragment, "two") == 0);
        CHECK(strcmp(page->spots[1].url, "http://host/dir/other.html") == 0);
        CHECK(strcmp(page->spots[1].fragment, "frag") == 0);
    }
    CHECK(wend_anchor_line(page, "one") >= 0);
    CHECK(wend_anchor_line(page, "two") > wend_anchor_line(page, "one"));
    CHECK(wend_anchor_line(page, "old") >= 0);      // the 1994 spelling
    CHECK(wend_anchor_line(page, "nothing") == -1);
    // The row an anchor names is the row its element begins on, so a reader
    // sent there finds the heading at the top rather than above the screen.
    int32_t at = wend_anchor_line(page, "two");
    CHECK(at >= 0 && at < page->nlines && strcmp(page->lines[at].text, "Two") == 0);
    wend_page_free(page);
}

static void form_checks(void)
{
    // The plain search box: the action resolved, the hidden field carried,
    // the box's value sent, the nameless button sending nothing.
    expect_url("search", "<form action=/s><input type=hidden name=h value=1>"
               "<input name=q value=cats><input type=submit value=Go></form>",
               1, WEND_FORM_OK, "http://host/s?h=1&q=cats");
    // A form with no action of its own goes back to the page — and a GET
    // form REPLACES the query that page already carried.
    expect_url("no action", "<form><input name=q value=x>"
               "<input type=submit name=go value=now></form>",
               1, WEND_FORM_OK, "http://host/dir/page.html?q=x&go=now");
    // Spaces become plus, everything else that is not unreserved becomes
    // percent-and-two-digits, over the UTF-8 bytes the page itself holds —
    // which is why the page has to say it is UTF-8 for this to be the
    // encoding of one accented letter rather than of two Windows-1252 ones.
    expect_url("encoding", "<meta charset=utf-8>"
               "<form action=/s><input name='a b' value='c d/\xc3\xa9'>"
               "<input type=submit></form>",
               1, WEND_FORM_OK, "http://host/s?a+b=c+d%2F%C3%A9");
    // A box that is not ticked is not sent; one with no value of its own
    // sends `on`.
    expect_url("ticks", "<form action=/s><input type=checkbox name=a checked>"
               "<input type=checkbox name=b><input type=checkbox name=c value=yes checked>"
               "<input type=submit></form>",
               3, WEND_FORM_OK, "http://host/s?a=on&c=yes");
    // A list sends the option's value, not the words shown for it.
    expect_url("choice", "<form action=/s><select name=pick>"
               "<option value=1>One<option value=2 selected>Two</select>"
               "<input type=submit></form>",
               1, WEND_FORM_OK, "http://host/s?pick=2");
    // Two buttons, and only the one pressed says it was.
    expect_url("which button", "<form action=/s><input type=submit name=go value=up>"
               "<input type=submit name=go value=down></form>",
               1, WEND_FORM_OK, "http://host/s?go=down");
    expect_url("post refused", "<form action=/s method=POST><input name=pw>"
               "<input type=submit></form>", 1, WEND_FORM_POST, NULL);
    expect_url("no form", "<a href=/x>link</a>", 0, WEND_FORM_NONE, NULL);
    expect_url("no form either", "<input name=loose>", 0, WEND_FORM_NONE, NULL);

    // A DISABLED CONTROL IS DRAWN, IS NOT LANDED ON, AND IS NOT SENT. The
    // only spot on this page is the button, so the query carries nothing of
    // the two controls the page took away.
    expect_url("disabled", "<form action=/s><input name=a value=1 disabled>"
               "<input type=hidden name=h value=2 disabled>"
               "<input name=b value=3><input type=submit></form>",
               1, WEND_FORM_OK, "http://host/s?b=3");
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
        if (page) {
            CHECK(strcmp(page->spots[0].value, "two  spaces\nand a line") == 0);
            char url[256];
            CHECK(wend_form_url(page, 1, "http://host/p", url, sizeof(url), NULL, 0)
                  == WEND_FORM_OK);
            CHECK(strcmp(url, "http://host/s?note=two++spaces%0Aand+a+line") == 0);
            wend_page_free(page);
        }
    }

    // A BUTTON MAY OVERRULE ITS FORM. The method decides whether this is a
    // submission wend performs at all, and the action decides where.
    expect_url("formmethod", "<form action=/s><input name=pw value=secret>"
               "<input type=submit formmethod=post></form>",
               1, WEND_FORM_POST, NULL);
    expect_url("formaction", "<form action=/s method=get><input name=q value=x>"
               "<input type=submit formaction=/other></form>",
               1, WEND_FORM_OK, "http://host/other?q=x");
    // A fieldset disables everything under it — except the words in its
    // first legend, which were never a control.
    expect_url("fieldset off", "<form action=/s><fieldset disabled>"
               "<legend>Title</legend><input name=a value=1></fieldset>"
               "<input name=b value=2><input type=submit></form>",
               1, WEND_FORM_OK, "http://host/s?b=2");
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
        CHECK(page && page->nforms == 1);
        if (page) {
            char url[256], frag[64];
            CHECK(wend_form_url(page, 1, "http://host/p", url, sizeof(url),
                                frag, sizeof(frag)) == WEND_FORM_OK);
            CHECK(strcmp(url, "http://host/s?q=x") == 0);
            CHECK(strcmp(frag, "results") == 0);
            wend_page_free(page);
        }
    }

    // WHAT A PERSON TYPED SURVIVES A RE-WRAP, which is the whole reason the
    // edits live outside the page: the same spot number carries the value
    // into the next render, at whatever width.
    const char *html = "<form action=/s><input name=q size=6 value=old>"
                       "<input type=submit value=Go></form>";
    wend_page_t *first = render_html_text(html, 80, "http://host/p");
    CHECK(first != NULL && first->nspots == 2);
    if (first) {
        wend_edit_t edits[2] = { { (char *)"new cat", -1, -1 }, { NULL, -1, -1 } };
        os64_html_parser_t *p = os64_html_parser_new(NULL);
        os64_url_t base;
        CHECK(p != NULL && os64_url_parse("http://host/p", &base) == OS64_URL_OK);
        if (p && os64_html_parser_feed(p, html, strlen(html)) >= 0) {
            os64_html_document_t *doc = os64_html_parser_finish(p);
            wend_page_t *again = doc ? wend_render_html(doc, &base, 40, edits, 2) : NULL;
            CHECK(again != NULL);
            if (again) {
                CHECK(again->nspots == 2);
                CHECK(strcmp(again->spots[0].value, "new cat") == 0);
                CHECK(again->nlines > 0 && strstr(again->lines[0].text, "[ew cat]") != NULL);
                char url[256];
                CHECK(wend_form_url(again, 1, "http://host/p", url, sizeof(url), NULL, 0)
                      == WEND_FORM_OK);
                CHECK(strcmp(url, "http://host/s?q=new+cat") == 0);
                wend_page_free(again);
            }
            if (doc)
                os64_html_document_free(doc);
        }
        wend_page_free(first);
    }
}

// An allocation failure anywhere leaves a page that is honest about being
// partial, frees clean, and never hands back a line the painter cannot draw.
static void oom_checks(void)
{
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

static void dump(const wend_page_t *page, const char *name)
{
    printf("page: %s\n", name);
    printf("title: %s\n", page->title);
    printf("width: %d\n", page->cols);
    printf("lines: %d\n", page->nlines);
    printf("spots: %d\n", page->nspots);
    printf("forms: %d\n", page->nforms);
    printf("anchors: %d\n", page->nanchors);
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
    // The spots, in the order the keyboard walks them. A link shows where it
    // goes; a control shows what it would send, because that is the half a
    // reader of this dump cannot get from the rows above.
    static const char *const kinds[] = { "link", "text", "check", "radio",
                                         "choice", "submit" };
    for (int32_t i = 0; i < page->nspots; i++) {
        const wend_spot_t *spot = &page->spots[i];
        printf("%d %s", i + 1, kinds[spot->kind]);
        if (spot->form)
            printf(" form=%d", spot->form);
        if (spot->kind == WEND_SPOT_LINK) {
            printf(" %s", spot->url[0] ? spot->url : "(unresolved)");
            if (spot->fragment[0])
                printf(" #%s", spot->fragment);
            printf("\n");
            continue;
        }
        if (spot->secret)
            printf(" secret");
        printf(" name=%s", spot->name[0] ? spot->name : "(none)");
        switch (spot->kind) {
            case WEND_SPOT_CHECK:
            case WEND_SPOT_RADIO:
                printf(" %s value=%s", spot->on ? "on" : "off", spot->value);
                break;
            case WEND_SPOT_CHOICE:
                printf(" chosen=%d of %d", spot->chosen, spot->noptions);
                for (int32_t j = 0; j < spot->noptions; j++)
                    printf(" [%s=%s]", spot->options[j], spot->option_values[j]);
                break;
            case WEND_SPOT_SUBMIT:
                printf(" label=%s value=%s", spot->label, spot->value);
                break;
            default:
                printf(" value=%s width=%d", spot->value, spot->width);
                break;
        }
        printf("\n");
    }
    for (int32_t i = 0; i < page->nforms; i++) {
        const wend_form_t *form = &page->forms[i];
        printf("form %d %s %s", i + 1, form->post ? "post" : "get",
               form->action[0] ? form->action : "(this page)");
        for (int32_t j = 0; j < form->nhidden; j++)
            printf(" %s=%s", form->hidden_names[j], form->hidden_values[j]);
        printf("\n");
    }
    // The anchors are a page's own index. Only the count goes in the dump
    // for a big page — the names are the document's own and would double
    // its size — but a small one lists them, which is what the fixtures use.
    if (page->nanchors <= 40)
        for (int32_t i = 0; i < page->nanchors; i++)
            printf("anchor #%s line %d\n", page->anchors[i].name,
                   page->anchors[i].line);
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
        form_checks();
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
    if (plain) {
        page = wend_render_text(bytes, len, !latin1, cols);
    } else {
        os64_url_t base;
        memset(&base, 0, sizeof(base));
        if (base_text && os64_url_parse(base_text, &base) != OS64_URL_OK) {
            fprintf(stderr, "bad base %s\n", base_text);
            return 2;
        }
        os64_html_options_t opt = os64_html_options_default();
        opt.charset = charset;
        os64_html_parser_t *p = os64_html_parser_new(&opt);
        if (!p || os64_html_parser_feed(p, bytes, len) < 0) {
            fprintf(stderr, "parse failed\n");
            return 1;
        }
        os64_html_document_t *doc = os64_html_parser_finish(p);
        if (!doc) {
            fprintf(stderr, "no document\n");
            return 1;
        }
        // The page's own <base href> outranks where it was fetched from,
        // exactly as it does in the browser.
        char href[OS64_URL_REF_MAX];
        os64_url_t from_page;
        if (base_text && wend_base_href(doc, href, sizeof(href))) {
            char absolute[OS64_URL_REF_MAX];
            if (os64_url_absolute(&base, href, absolute, sizeof(absolute)) &&
                os64_url_parse(absolute, &from_page) == OS64_URL_OK)
                base = from_page;
        }
        page = wend_render_html(doc, base_text ? &base : NULL, cols, NULL, 0);
        os64_html_document_free(doc);
    }
    if (!page) {
        fprintf(stderr, "no page\n");
        return 1;
    }
    check_printable(page);
    dump(page, name);
    wend_page_free(page);
    free(bytes);
    if (failures) {
        fprintf(stderr, "%d invariant failures\n", failures);
        return 1;
    }
    return 0;
}
