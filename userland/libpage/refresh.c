// refresh.c — family K: the navigation a DOCUMENT declares.
//
// `<meta http-equiv="refresh" content="0;URL=...">` is the only way a page
// can send a reader somewhere with no server redirect and no script, which
// is exactly why it is still everywhere: it is what a search engine's click
// logger falls back to for a browser with no JavaScript, and what every
// "this page has moved" placeholder is made of. `http-equiv` means "pretend
// the server sent this as a header", and `Refresh:` was a Netscape header
// that never entered any HTTP specification — the in-document twin is the
// half that survived, and it is written into HTML itself now.
//
// IT BELONGS ON THIS SIDE OF THE SEAM because it is a navigation, not a
// drawing: its address resolves against the base like any other reference,
// it can carry a page's values from an encrypted document to a plain one,
// and it can name the page it is already on. A face decides whether to
// follow it and how long to wait; what it MEANS is answered here, once, for
// every face.
//
// THE GRAMMAR IS WRITTEN AS THE STANDARD WRITES IT, jumps included. The
// shared declarative refresh steps scan the letters of `url` one at a time
// and leave that scan at three different points, and each exit keeps a
// DIFFERENT candidate address — so `content="0;used=x"` asks for an address
// called `used=x`, which no tidier parser would ever produce. A version of
// this that reads better is a version that disagrees with every browser.

#include "internal.h"

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool p_refresh_content(const char *input, uint32_t *seconds, const char **url, size_t *url_len)
{
    *seconds = 0;
    *url = NULL;
    *url_len = 0;
    if (input == NULL)
        return false;
    size_t at = 0, n = os64_strlen(input);
    while (at < n && ws(input[at]))
        at++;
    size_t first_digit = at;
    uint64_t time = 0;
    while (at < n && digit(input[at])) {
        if (time <= 0xFFFFFFFFull)
            time = time * 10 + (uint64_t)(input[at] - '0');
        at++;
    }
    if (at == first_digit && (at >= n || input[at] != '.'))
        // No digits and no full stop: the standard declares nothing at all,
        // so `content="url=somewhere"` is not a refresh however much it
        // looks like one.
        return false;
    // A page may ask for a delay longer than a delay can be; what it means
    // is "not soon", and saturating says that without overflowing.
    *seconds = time > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)time;
    // Digits and full stops past the integer are COLLECTED AND THROWN AWAY,
    // which is how a fractional delay survives as its whole part.
    while (at < n && (digit(input[at]) || input[at] == '.'))
        at++;
    if (at >= n)
        return true;   // a delay and nothing else: this page, again
    if (input[at] != ';' && input[at] != ',' && !ws(input[at]))
        return false;
    while (at < n && ws(input[at]))
        at++;
    if (at < n && (input[at] == ';' || input[at] == ','))
        at++;
    while (at < n && ws(input[at]))
        at++;
    if (at >= n)
        return true;

    // Everything left is the candidate address, and the keyword scan below
    // can only shorten it: leaving that scan early keeps what was already
    // in hand, which is the standard's `parse` label.
    size_t candidate = at;
    bool reached_quotes = false;
    if (input[at] != 'u' && input[at] != 'U') {
        reached_quotes = true;   // no keyword to scan; quotes still count
    } else if (++at < n && (input[at] == 'r' || input[at] == 'R') && ++at < n &&
               (input[at] == 'l' || input[at] == 'L')) {
        at++;
        while (at < n && ws(input[at]))
            at++;
        if (at < n && input[at] == '=') {
            at++;
            while (at < n && ws(input[at]))
                at++;
            reached_quotes = true;
        }
    }
    size_t start = reached_quotes ? at : candidate;
    size_t stop = n;
    if (reached_quotes && start < n && (input[start] == '\'' || input[start] == '"')) {
        char quote = input[start++];
        for (size_t i = start; i < n; i++)
            if (input[i] == quote) {
                stop = i;
                break;
            }
    }
    if (start >= stop)
        return true;   // an address of nothing is this page, again
    *url = input + start;
    *url_len = stop - start;
    return true;
}

// Invalid pragmas leave the page looking. Allocation failure instead
// retains this candidate's refusal: uncertainty cannot authorize a later
// pragma to replace the one that should have won.
bool p_refresh_from(os64_page_t *page, const os64_html_node_t *n)
{
    const char *equiv = p_attr(n, "http-equiv");
    const char *content = p_attr(n, "content");
    if (equiv == NULL || content == NULL || !os64_streq_nocase(equiv, "refresh"))
        return false;
    uint32_t seconds = 0;
    const char *url = NULL;
    size_t url_len = 0;
    if (!p_refresh_content(content, &seconds, &url, &url_len))
        return false;

    os64_page_refresh_t found;
    os64_memset(&found, 0, sizeof(found));
    found.node = n;
    found.seconds = seconds;
    if (url == NULL) {
        // No address: the target is the document itself, and `spelled` is
        // what says the page did not name one.
        found.url.url = page->document_url;
    } else {
        char *reference = p_copy(page, url, url_len);
        if (reference == NULL) {
            found.url.spelled = true;
            found.url.refused = OS64_PAGE_REASON_NO_MEMORY;
        } else {
            p_resolve(page, reference, &found.url);
        }
        // An unusable address declares nothing. Failure to retain or
        // resolve it in memory is different: preserve the refusal below.
        if (found.url.url == NULL && found.url.refused != OS64_PAGE_REASON_NO_MEMORY)
            return false;
    }
    found.names_this_document = found.url.url != NULL &&
        os64_streq(found.url.url, page->document_url);
    page->refresh = found;
    page->has_refresh = true;
    return true;
}

const os64_page_refresh_t *os64_page_refresh(const os64_page_t *page)
{
    return page != NULL && page->has_refresh ? &page->refresh : NULL;
}
