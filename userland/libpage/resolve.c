// resolve.c — family A: the base, and what a reference on this page means.
//
// ONE DOOR, because a reference that resolves one way in a link and another
// way in a form action is two rules pretending to be one. Every `href`,
// every `action` and every `formaction` comes through here.
//
// os64/url.h owns the GRAMMAR and says so: it will not tell anyone what port
// a scheme defaults to, because that is policy and differs per protocol.
// This file is where that policy lives, and the reason it has to exist is
// that a page which spells `https://host:443/` must not put `:443` into
// every link it carries — an address nobody wrote, and one that stops
// matching the page it is on when a `#name` asks whether this is the same
// document.

#include "internal.h"

uint16_t p_default_port(const char *scheme)
{
    // The URL Standard's special schemes and their ports, which is the list
    // of ports that are never spelled. Anything else keeps whatever port the
    // page wrote: `gopher://host:70/` is not a default any more, and
    // pretending otherwise would rewrite somebody's address.
    if (os64_streq(scheme, "http") || os64_streq(scheme, "ws"))
        return 80;
    if (os64_streq(scheme, "https") || os64_streq(scheme, "wss"))
        return 443;
    if (os64_streq(scheme, "ftp"))
        return 21;
    return 0;
}

static int32_t hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

size_t p_percent_decode(char *s, size_t len)
{
    // In place and over BYTES, so `%C3%A9` becomes the same two UTF-8 bytes
    // the page was written in rather than two characters of nonsense. An
    // escape that is not two hex digits is text, which is what every browser
    // does with a stray `%`.
    size_t write = 0;
    for (size_t read = 0; read < len;) {
        int32_t hi, lo;
        if (s[read] == '%' && read + 2 < len && (hi = hex_digit(s[read + 1])) >= 0 &&
            (lo = hex_digit(s[read + 2])) >= 0) {
            s[write++] = (char)(hi * 16 + lo);
            read += 3;
            continue;
        }
        s[write++] = s[read++];
    }
    s[write] = '\0';
    return write;
}

bool p_canonical(os64_page_t *page, const char *text, const char **out,
                 os64_page_reason_t *refused)
{
    os64_url_t url;
    os64_url_result_t rc = os64_url_parse(text, &url);
    if (rc == OS64_URL_OK) {
        if (url.port != 0 && url.port == p_default_port(url.scheme))
            url.port = 0;
        char spelled[OS64_URL_REF_MAX];
        if (!os64_url_spell(&url, spelled, sizeof(spelled))) {
            *refused = OS64_PAGE_REASON_TOO_LONG;
            return false;
        }
        *out = p_copy(page, spelled, os64_strlen(spelled));
        if (*out == NULL) {
            *refused = OS64_PAGE_REASON_NO_MEMORY;
            return false;
        }
        return true;
    }
    if (rc == OS64_URL_TOO_LONG) {
        *refused = OS64_PAGE_REASON_TOO_LONG;
        return false;
    }
    // An address with no authority — `mailto:`, `data:`, `javascript:` —
    // which the grammar cannot take apart and a browser still navigates.
    // There is nothing to canonicalise, so it is kept as it was written.
    char scheme[OS64_URL_SCHEME_MAX];
    if (rc == OS64_URL_NOT_A_URL && os64_url_scheme_of(text, scheme, sizeof(scheme))) {
        size_t len = os64_strlen(text);
        if (len >= OS64_URL_REF_MAX) {
            *refused = OS64_PAGE_REASON_TOO_LONG;
            return false;
        }
        *out = p_copy(page, text, len);
        if (*out == NULL) {
            *refused = OS64_PAGE_REASON_NO_MEMORY;
            return false;
        }
        return true;
    }
    *refused = OS64_PAGE_REASON_BAD_ACTION;
    return false;
}

// An address for COMPARING and for resolving against carries no fragment:
// `#name` names a place inside a document, and the document is the same one
// either way.
static char *without_fragment(os64_page_t *page, const char *text)
{
    size_t len = 0;
    while (text[len] != '\0' && text[len] != '#')
        len++;
    return p_copy(page, text, len);
}

bool p_document_url(os64_page_t *page, const char *text)
{
    char *raw = without_fragment(page, text != NULL ? text : "");
    if (raw == NULL)
        return false;
    page->document_url = raw;
    os64_page_reason_t refused = OS64_PAGE_REASON_OK;
    const char *canonical = NULL;
    if (p_canonical(page, raw, &canonical, &refused))
        page->document_url = canonical;
    // Parsed for resolution, and separately: an address this grammar cannot
    // take apart is still an address a downgrade can be judged against, so
    // the scheme is read even when the rest is opaque.
    page->document_hierarchical = os64_url_parse(page->document_url, &page->document) == OS64_URL_OK;
    if (page->document_hierarchical && page->document.port != 0 &&
        page->document.port == p_default_port(page->document.scheme))
        page->document.port = 0;
    if (!os64_url_scheme_of(page->document_url, page->document_scheme,
                            sizeof(page->document_scheme)))
        page->document_scheme[0] = '\0';
    return true;
}

// The first `base` with an `href`, wherever libhtml kept it — the body
// included, since a misplaced one is left where it was found. An EMPTY href
// counts and outranks a later base that names somewhere else, because
// `<base href="">` resolves to the document's own address.
static const char *find_base(const os64_html_node_t *n)
{
    for (; n != NULL; n = n->next) {
        if (p_is(n, OS64_HTML_TAG_BASE)) {
            const char *href = p_attr(n, "href");
            if (href != NULL)
                return href;
        }
        const char *found = find_base(n->first_child);
        if (found != NULL)
            return found;
    }
    return NULL;
}

bool p_base(os64_page_t *page)
{
    // With no base of its own a page resolves against where it came from.
    page->base_url = page->document_url;
    page->base = page->document;
    page->base_hierarchical = page->document_hierarchical;

    const os64_html_node_t *root = page->doc != NULL ? page->doc->document : NULL;
    if (root == NULL && page->doc != NULL)
        root = page->doc->html;
    const char *href = find_base(root);
    if (href == NULL)
        return true;

    char *head = without_fragment(page, href);
    if (head == NULL)
        return false;
    if (head[0] == '\0')
        return true;   // `<base href="">` is the document's own address

    const char *resolved = NULL;
    os64_page_reason_t refused = OS64_PAGE_REASON_OK;
    char absolute[OS64_URL_REF_MAX];
    char scheme[OS64_URL_SCHEME_MAX];
    if (page->document_hierarchical &&
        os64_url_absolute(&page->document, head, absolute, sizeof(absolute)))
        p_canonical(page, absolute, &resolved, &refused);
    else if (os64_url_scheme_of(head, scheme, sizeof(scheme)))
        p_canonical(page, head, &resolved, &refused);
    // A base that will not resolve is not a base: the document's own address
    // stands, which is what the standard's fallback base URL is for.
    if (resolved == NULL)
        return refused != OS64_PAGE_REASON_NO_MEMORY;

    page->base_url = resolved;
    page->base_hierarchical = os64_url_parse(resolved, &page->base) == OS64_URL_OK;
    if (page->base_hierarchical && page->base.port != 0 &&
        page->base.port == p_default_port(page->base.scheme))
        page->base.port = 0;
    return true;
}

void p_resolve(os64_page_t *page, const char *ref, os64_page_ref_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    out->refused = OS64_PAGE_REASON_OK;
    if (ref == NULL)
        // The page spelled nothing at all, and what THAT means belongs to
        // whoever asked: a form with no action means the page it is on, and
        // an `a` with no href is not a link.
        return;
    out->spelled = true;

    size_t head = 0;
    while (ref[head] != '\0' && ref[head] != '#')
        head++;
    if (ref[head] == '#') {
        out->has_fragment = true;
        char *fragment = p_copy(page, ref + head + 1, os64_strlen(ref + head + 1));
        if (fragment == NULL) {
            out->refused = OS64_PAGE_REASON_NO_MEMORY;
            return;
        }
        // Matched DECODED, because a heading whose name has a space in it is
        // written `%20` in the link and plainly in the `id`.
        p_percent_decode(fragment, os64_strlen(fragment));
        out->fragment = fragment;
    }

    if (head == 0) {
        // An EMPTY reference names the document it appeared on — every
        // reload link on the old web — and so does a fragment-only one.
        out->url = page->document_url;
        return;
    }
    if (head >= OS64_URL_REF_MAX) {
        out->refused = OS64_PAGE_REASON_TOO_LONG;
        return;
    }
    // The fragment is cut off before anything resolves, so it is carried
    // beside the address in exactly one place rather than surviving in two.
    char bare[OS64_URL_REF_MAX];
    os64_memcpy(bare, ref, head);
    bare[head] = '\0';

    char absolute[OS64_URL_REF_MAX];
    char scheme[OS64_URL_SCHEME_MAX];
    if (page->base_hierarchical) {
        if (!os64_url_absolute(&page->base, bare, absolute, sizeof(absolute))) {
            out->refused = OS64_PAGE_REASON_TOO_LONG;
            return;
        }
        p_canonical(page, absolute, &out->url, &out->refused);
        return;
    }
    if (os64_url_scheme_of(bare, scheme, sizeof(scheme))) {
        // No base worth resolving against, but an absolute reference needs
        // none, so a `mailto:` on an opaque page still works.
        p_canonical(page, bare, &out->url, &out->refused);
        return;
    }
    out->refused = OS64_PAGE_REASON_BAD_ACTION;
}
