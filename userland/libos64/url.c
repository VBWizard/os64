// url.c — the common URL grammar. See os64/url.h for what is here and what
// is deliberately not.

#include "os64/url.h"

#include "os64/fmt.h"
#include "os64/str.h"

// ── Bytes ───────────────────────────────────────────────────────────────

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

// A host label's alphabet. Underscore is not legal in a hostname and is
// nonetheless in use (service records, and more than one home router), so it
// is accepted; everything else is refused, which is what keeps a '@', a '['
// or a stray space from reaching the resolver or a request line.
static bool is_host_byte(char c)
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_';
}

// A scheme's alphabet (RFC 3986 §3.1): a letter, then letters, digits and
// "+-.". The first-byte rule is what keeps a Windows path ("c:/tmp") and a
// port-only word from being read as one.
static bool is_scheme_byte(char c)
{
    return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.';
}

// os64_strcopy's contract (the WANTED length, not the written one) over a
// span rather than a NUL-terminated string, with and without case folding.
// Case matters in exactly one direction here: a scheme and a host are
// COMPARED by their callers, so they are folded once on the way in; a path
// is DATA and is copied byte for byte.
static size_t copy_span(char *dst, size_t cap, const char *src, size_t len)
{
    size_t i = 0;
    if (cap != 0) {
        for (; i < len && i + 1 < cap; i++)
            dst[i] = src[i];
        dst[i] = '\0';
    }
    return len;
}

static size_t copy_lower(char *dst, size_t cap, const char *src, size_t len)
{
    size_t i = 0;
    if (cap != 0) {
        for (; i < len && i + 1 < cap; i++)
            dst[i] = to_lower(src[i]);
        dst[i] = '\0';
    }
    return len;
}

// ── The parse ───────────────────────────────────────────────────────────

os64_url_result_t os64_url_parse(const char *text, os64_url_t *out)
{
    if (text == NULL || out == NULL)
        return OS64_URL_NOT_A_URL;

    os64_memset(out, 0, sizeof(*out));

    // THE SCHEME IS WHAT MAKES IT A URL. Requiring "scheme://" is what lets
    // a caller tell an address from whatever else its first operand can be
    // — a valet file name, a bare gopher host — without guesswork. (wget
    // accepts "host/path" and guesses http; a guess like that collides with
    // every other meaning an operand has, which is why it is the caller's to
    // make and not this parser's.)
    const char *sep = NULL;
    for (const char *p = text; *p != '\0'; p++)
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') { sep = p; break; }
    if (sep == NULL || sep == text)
        return OS64_URL_NOT_A_URL;

    if (!is_alpha(text[0]))
        return OS64_URL_NOT_A_URL;
    for (const char *p = text; p < sep; p++)
        if (!is_scheme_byte(*p))
            return OS64_URL_NOT_A_URL;   // whatever this is, it is not a scheme

    if (copy_lower(out->scheme, sizeof(out->scheme), text, (size_t)(sep - text)) >=
        sizeof(out->scheme))
        return OS64_URL_TOO_LONG;

    // ── The authority: everything up to the path, query or fragment ─────
    const char *authority = sep + 3;
    const char *p = authority;
    while (*p != '\0' && *p != '/' && *p != '?' && *p != '#')
        p++;
    const char *authorityEnd = p;
    if (authorityEnd == authority)
        return OS64_URL_NO_HOST;

    // The port, if the authority names one. Searched from the RIGHT so that
    // the refusals below still see the whole host: "user@host" and "[::1]"
    // are rejected as host bytes rather than mistaken for ports.
    const char *colon = NULL;
    for (const char *q = authorityEnd; q > authority; q--)
        if (q[-1] == ':') { colon = q - 1; break; }

    const char *hostEnd = colon != NULL ? colon : authorityEnd;
    if (hostEnd == authority)
        return OS64_URL_NO_HOST;
    for (const char *q = authority; q < hostEnd; q++)
        if (!is_host_byte(*q))
            return OS64_URL_HOST_CHARS;   // userinfo, an IPv6 literal, or junk
    // THE HOST IS FOLDED, THE PATH IS NOT. A DNS name is case-insensitive by
    // definition (RFC 4343), and so is the name a server matches it against
    // — so the case a person typed carries no information, and keeping it
    // would put two spellings of one machine in every diagnostic. A PATH is
    // the opposite: it is a name on somebody else's filesystem, and /Case
    // and /case are two different things.
    if (copy_lower(out->host, sizeof(out->host), authority, (size_t)(hostEnd - authority)) >=
        sizeof(out->host))
        return OS64_URL_TOO_LONG;

    if (colon != NULL) {
        const char *q = colon + 1;
        if (q == authorityEnd)
            return OS64_URL_PORT;         // "host:" names no port
        uint32_t port = 0;
        for (; q < authorityEnd; q++) {
            if (!is_digit(*q))
                return OS64_URL_PORT;
            port = port * 10 + (uint32_t)(*q - '0');
            if (port > 65535)
                return OS64_URL_PORT;
        }
        if (port == 0)
            return OS64_URL_PORT;         // and so zero can mean "none named"
        out->port = (uint16_t)port;
    }

    // ── The path, with the query and without the fragment ───────────────
    // A fragment is the CLIENT's business — it names a place inside a
    // document and has never crossed a wire (RFC 1945 onward). Dropping it
    // here rather than sending it is not tidiness: a '#' in a request is a
    // request the server was never asked.
    const char *pathStart = authorityEnd;
    const char *pathEnd = pathStart;
    while (*pathEnd != '\0' && *pathEnd != '#')
        pathEnd++;

    // A space or a control byte in a request line ends the line early on the
    // far side and starts a second request nobody typed. Refuse; percent-
    // encoding what a person typed is a guess about their intent, and this
    // is the wrong place to guess. A caller that DECODES escapes afterwards
    // owes this check again — see the header.
    for (const char *q = pathStart; q < pathEnd; q++)
        if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7F)
            return OS64_URL_PATH_CHARS;

    size_t wanted;
    if (pathStart == pathEnd || *pathStart != '/') {
        // "scheme://host", "scheme://host?q=1" — the root, plus whatever
        // query came with it. Normalising the empty path to "/" serves both
        // customers: it is the origin's root to HTTP, and RFC 4266's type-1
        // menu with an empty selector to gopher.
        out->path[0] = '/';
        wanted = 1 + copy_span(out->path + 1, sizeof(out->path) - 1, pathStart,
                               (size_t)(pathEnd - pathStart));
    } else {
        wanted = copy_span(out->path, sizeof(out->path), pathStart,
                           (size_t)(pathEnd - pathStart));
    }
    if (wanted >= sizeof(out->path))
        return OS64_URL_TOO_LONG;

    return OS64_URL_OK;
}

const char *os64_url_reason(os64_url_result_t rc)
{
    switch (rc) {
        case OS64_URL_OK:         return "ok";
        case OS64_URL_NOT_A_URL:  return "not an address (no scheme://)";
        case OS64_URL_NO_HOST:    return "the address names no host";
        case OS64_URL_HOST_CHARS: return "the host holds characters a host cannot";
        case OS64_URL_PATH_CHARS: return "the path holds a space or a control character";
        case OS64_URL_PORT:       return "the port is not a number between 1 and 65535";
        case OS64_URL_TOO_LONG:   return "the address is too long";
    }
    return "the address cannot be read";
}

// ── Reference resolution (RFC 3986 §5.2) ───────────────────────────────
//
// Where a relative reference points, given the page it appeared on. Two
// customers with one arithmetic: a redirect's Location (libfetch) and every
// href on a page (the browser's navigator). It lived beside HTTP while the
// redirect was its only customer; the grammar it resolves against is this
// file's, and so is it now.

// DOES THIS REFERENCE NAME ITS OWN SCHEME? RFC 3986 §3.1's grammar exactly: a
// letter, then letters, digits, '+', '-' and '.', ended by a colon — and the
// search stops at the first '/', '?' or '#', because a colon past one of
// those is a byte of the path or query, not a scheme's punctuation.
//
// The whole difficulty lives in that stopping rule. This used to be "does the
// string contain '://'", which made `/login?next=https://id.example/` — an
// ordinary root-relative redirect carrying a URL in its query — look like an
// absolute reference, and the advice printed the query's contents as the
// address to fetch (Codex review round 3, 2026-09-02). Asking the URL parser
// instead fixed that one and left `mailto:someone@example.com` looking
// relative, which joined it onto the base and produced a plausible, wrong
// http address for something that was never a page at all. The grammar
// answers both.
//
// THE STRICT READING, where RFC 3986 §5.4.2 offers two. `http:page.html`
// names the scheme http and the opaque path "page.html"; browsers and
// urllib join it to the base instead, for compatibility with references
// written before 1998. Read strictly it is an address a fetcher cannot fetch
// and says so; read loosely it is a guess about what a malformed header
// meant. A fetch that guesses wrong downloads the wrong page and calls it
// success, so this guesses not at all.
static bool reference_has_scheme(const char *ref)
{
    if (!is_alpha(ref[0]))
        return false;               // a scheme starts with a letter, always
    for (const char *p = ref; *p != '\0'; p++) {
        if (*p == ':')
            return true;
        if (*p == '/' || *p == '?' || *p == '#')
            return false;
        if (!is_alpha(*p) && !is_digit(*p) && *p != '+' && *p != '-' && *p != '.')
            return false;
    }
    return false;
}

// The longest path this file ever builds: a base's whole path with a whole
// Location joined onto it, before the dot segments come out.
#define URL_JOIN_MAX (OS64_URL_PATH_MAX + OS64_URL_REF_MAX + 2)

// RFC 3986 §5.2.4: resolve away the "." and ".." segments a merge just
// created. Written as the segment walk the spec describes rather than the
// spec's own five-case string rewrite, because the walk is the thing anybody
// reading this can check: split on '/', drop a ".", pop the previous segment
// for a "..", and keep everything else.
//
// TWO EDGES DECIDE WHETHER THIS IS RIGHT. A ".." at the very top pops
// nothing — the leading '/' is a floor, so `/..` is `/` and not an escape
// upwards into somebody's parent directory. And a "." or ".." that ends the
// path leaves a trailing '/' behind it (`/a/b/..` is `/a/`, which is a
// DIRECTORY, not the file `/a`), because the segment it replaced was there.
// An empty segment is data and is kept: `//x` names something a server may
// well serve, and is not `/x`.
static bool path_dots(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    const char *p = in;

    if (*p == '/')
        p++;                          // the leading empty segment: the root

    for (;;) {
        const char *seg = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t n = (size_t)(p - seg);
        bool last = (*p == '\0');
        if (!last)
            p++;

        if (n == 1 && seg[0] == '.') {
            n = 0;                    // dropped; if it ended the path, its '/' stays
            if (!last)
                continue;
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;                // and the '/' that introduced it
            n = 0;
            if (!last)
                continue;
        }

        if (len + 1 + n + 1 > cap)
            return false;
        out[len++] = '/';
        for (size_t i = 0; i < n; i++)
            out[len++] = seg[i];
        if (last)
            break;
    }

    out[len] = '\0';
    return true;
}

// A reference that carries its own authority — `scheme://host...` or
// `//host...` — keeps that authority verbatim, and its PATH still gets
// §5.2.2's remove_dot_segments, its fragment cut. The path is what the
// request line carries, and every other client squashes it before sending
// (curl, wget, the browsers), so a server has never had to decide what
// `/a/../b` means and may decide it badly — RFC 3986 already decided.
// `prefix` is the byte count up to and including the authority. An empty
// path stays empty: `http://host` and `http://host/` are one place, and
// the circle check compares spellings.
static bool authority_path_dots(const char *ref, size_t prefix, char *out, size_t cap)
{
    const char *path = ref + prefix;
    size_t pathLen = 0;
    while (path[pathLen] != '\0' && path[pathLen] != '?' && path[pathLen] != '#')
        pathLen++;
    const char *query = path + pathLen;
    size_t queryLen = 0;
    while (query[queryLen] != '\0' && query[queryLen] != '#')
        queryLen++;

    if (prefix >= cap)
        return false;
    copy_span(out, cap, ref, prefix);
    size_t len = prefix;
    if (pathLen != 0) {
        char raw[URL_JOIN_MAX];
        char dots[URL_JOIN_MAX];
        if (pathLen >= sizeof(raw))
            return false;
        copy_span(raw, sizeof(raw), path, pathLen);
        if (!path_dots(raw, dots, sizeof(dots)))
            return false;
        size_t n = os64_strlen(dots);
        if (len + n >= cap)
            return false;
        copy_span(out + len, cap - len, dots, n);
        len += n;
    }
    if (len + queryLen >= cap)
        return false;
    copy_span(out + len, cap - len, query, queryLen);
    return true;
}

bool os64_url_absolute(const os64_url_t *base, const char *location,
                       char *out, size_t cap)
{
    if (location == NULL || location[0] == '\0')
        return false;

    // ALREADY WHOLE: the authority is whoever it names, verbatim. With no
    // `//` after the scheme there is no authority and no path to squash —
    // `mailto:someone`, or `http:page.html`, which §5.4.2 lets a resolver
    // join to the base and this one refuses — so it is copied through for
    // the caller to judge.
    if (reference_has_scheme(location)) {
        const char *p = location;
        while (*p != ':')
            p++;
        p++;
        if (p[0] != '/' || p[1] != '/')
            return os64_strcopy(out, cap, location) < cap;
        p += 2;
        while (*p != '\0' && *p != '/' && *p != '?' && *p != '#')
            p++;
        return authority_path_dots(location, (size_t)(p - location), out, cap);
    }

    // A SCHEME-RELATIVE REFERENCE — `//cdn.example.com/file` — NAMES A
    // DIFFERENT HOST. It looks like an absolute path because it starts with a
    // slash, and treating it as one produced
    // `https://original.example//cdn.example.com/file`: an address pointing
    // back at the server that just redirected away from itself, which would
    // fetch something unrelated and look like it worked. Only the scheme is
    // inherited; the authority comes from the reference. (RFC 3986 §4.2, and
    // Codex review round 2, 2026-09-02.)
    if (location[0] == '/' && location[1] == '/') {
        char whole[OS64_URL_REF_MAX];
        int32_t n = os64_snprintf(whole, sizeof(whole), "%s:%s", base->scheme, location);
        if (n <= 0 || (size_t)n >= sizeof(whole))
            return false;
        const char *p = whole + os64_strlen(base->scheme) + 3;
        while (*p != '\0' && *p != '/' && *p != '?' && *p != '#')
            p++;
        return authority_path_dots(whole, (size_t)(p - whole), out, cap);
    }

    // ── Everything left is relative to the page that answered ───────────
    // RFC 3986 §5.2.2, for the case where the reference brings no scheme and
    // no authority: the origin is inherited whole, and only the path and
    // query are worked out. The fragment is cut off first — it names a place
    // inside the document and has never crossed the wire.
    char ref[OS64_URL_REF_MAX];
    size_t reflen = 0;
    while (location[reflen] != '\0' && location[reflen] != '#')
        reflen++;
    if (reflen >= sizeof(ref))
        return false;
    copy_span(ref, sizeof(ref), location, reflen);

    // Both sides split at their query, because a '?' ends the path and every
    // rule below is about paths. `..` inside a query is a byte, not a step.
    const char *baseQuery = base->path;
    while (*baseQuery != '\0' && *baseQuery != '?')
        baseQuery++;
    size_t basePathLen = (size_t)(baseQuery - base->path);

    const char *refQuery = ref;
    while (*refQuery != '\0' && *refQuery != '?')
        refQuery++;
    size_t refPathLen = (size_t)(refQuery - ref);

    char path[URL_JOIN_MAX];
    const char *query;

    if (refPathLen == 0) {
        // `?page=2`, or a reference that was nothing but a fragment: the page
        // stays, and only the question changes. With no '?' of its own the
        // reference is the SAME address, which is a redirect in a circle —
        // said plainly by whoever compares them, not papered over here.
        copy_span(path, sizeof(path), base->path, basePathLen);
        query = (*refQuery == '?') ? refQuery : baseQuery;
    } else {
        char joined[URL_JOIN_MAX];
        if (ref[0] == '/') {
            copy_span(joined, sizeof(joined), ref, refPathLen);
        } else {
            // §5.2.3's merge: the base path up to and INCLUDING its last
            // '/', then the reference. `/a/b.html` + `c.html` is `/a/c.html`
            // — the page's directory, not the page.
            size_t cut = basePathLen;
            while (cut > 0 && base->path[cut - 1] != '/')
                cut--;
            if (cut + refPathLen + 1 > sizeof(joined))
                return false;
            copy_span(joined, sizeof(joined), base->path, cut);
            copy_span(joined + cut, sizeof(joined) - cut, ref, refPathLen);
        }
        if (!path_dots(joined, path, sizeof(path)))
            return false;
        query = refQuery;
    }

    // The origin that just answered, KEEPING THAT ORIGIN'S SCHEME.
    // Hard-coding http here sent an https page's `/login` redirect back as
    // `http://host:443/login`: plaintext at a TLS port, an address that
    // cannot work, printed as a command to copy. The port rides along only
    // when the base SPELLED one (port 0 = it did not, os64_url_t's rule; a caller
    // that holds a filled-in default hands over 0 for it), for the same
    // reason a Host header omits a default port. (Codex review, 2026-09-02.)
    int32_t n;
    if (base->port != 0)
        n = os64_snprintf(out, cap, "%s://%s:%u%s%s", base->scheme, base->host,
                          (unsigned)base->port, path, query);
    else
        n = os64_snprintf(out, cap, "%s://%s%s%s", base->scheme, base->host, path, query);
    return n > 0 && (size_t)n < cap;
}
