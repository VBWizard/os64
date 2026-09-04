// http.c — the world's dialect. See http.h for what this is and is not.

#include "http.h"

#include "os64/fmt.h"
#include "os64/str.h"

// ── Bytes ───────────────────────────────────────────────────────────────

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_blank(char c) { return c == ' ' || c == '\t'; }
// What a reason phrase or a field value may hold: RFC 9110's field-vchar,
// obs-text and the blanks — bytes a terminal draws, not bytes it obeys. Both
// are the origin's words and both get printed to the person's terminal, so
// a control byte is refused once, here, rather than escaped at every print.
static bool is_field_byte(char c)
{
    return c == '\t' || ((unsigned char)c >= 0x20 && c != 0x7F);
}
static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

// A FIELD NAME IS A TOKEN, and both places that read one judge it by this.
// RFC 7230's tchar: letters, digits, and "!#$%&'*+-.^_`|~". Nothing else —
// no space, no tab, no colon, no control byte.
//
// It exists because the two paths were judging names by different rules and
// the looser one was reachable. `Transfer-Encoding :` is refused by
// header_take (which checks the byte before the colon) and was ACCEPTED by
// overlong_verdict, whose name simply carried the trailing blank and matched
// nothing — so the malformed short form was rejected while the malformed
// LONG form was dropped, and dropping it published chunk markers as a file.
// A shared rule cannot drift like two spellings of one rule can.
// (Codex review round 3, 2026-09-02.)
static bool is_token_byte(char c)
{
    return is_alpha(c) || is_digit(c) ||
           c == '!' || c == '#' || c == '$'  || c == '%' || c == '&' ||
           c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
           c == '^' || c == '_' || c == '`'  || c == '|' || c == '~';
}

// os64_strcopy's contract (the wanted length, not the written one), plus
// ASCII case folding. Case matters here in exactly one direction: a scheme
// and a coding name are compared, so they are folded once on the way in
// rather than at every comparison; a path and a Location are DATA and are
// copied verbatim.
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

// ── A URL, taken apart ──────────────────────────────────────────────────

// A host label's alphabet. Underscore is not legal in a hostname and is
// nonetheless in use (service records, and more than one home router), so it
// is accepted; everything else is refused, which is what keeps a '@', a '['
// or a stray space from reaching the resolver or the request line.
static bool is_host_byte(char c)
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_';
}

http_url_result_t http_url_parse(const char *url, http_url_t *out)
{
    os64_memset(out, 0, sizeof(*out));

    // THE SCHEME IS WHAT MAKES IT A URL. os64get's first operand has always
    // been a valet HOST or a file NAME, and both are bare words; requiring
    // "scheme://" is what keeps the two dialects from ever having to be told
    // apart by guesswork. (wget accepts a bare "host/path" and guesses http;
    // here that guess would collide with a name the valet serves.)
    const char *sep = NULL;
    for (const char *p = url; *p != '\0'; p++)
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') { sep = p; break; }
    if (sep == NULL || sep == url)
        return HTTP_URL_NOT_A_URL;

    for (const char *p = url; p < sep; p++)
        if (!is_alpha(*p) && !is_digit(*p) && *p != '+' && *p != '-' && *p != '.')
            return HTTP_URL_NOT_A_URL;   // whatever this is, it is not a scheme

    // BOTH SCHEMES PARSE. Which of them this machine can actually FETCH is
    // policy, not grammar, and it changes with the configuration: an https
    // URL is unreachable on its own and perfectly reachable through a
    // TLS-terminating proxy. A parser that refused https would make that
    // decision here, in the one place that cannot know about it — so the
    // parser answers "what is this address", and the caller answers "can I
    // go there". The default port comes from the scheme, as it has since
    // 1994.
    copy_lower(out->scheme, sizeof(out->scheme), url, (size_t)(sep - url));
    if (os64_streq(out->scheme, "http"))
        out->port = 80;
    else if (os64_streq(out->scheme, "https"))
        out->port = 443;
    else
        return HTTP_URL_SCHEME;

    // ── The authority: everything up to the path, query or fragment ─────
    const char *authority = sep + 3;
    const char *p = authority;
    while (*p != '\0' && *p != '/' && *p != '?' && *p != '#')
        p++;
    const char *authorityEnd = p;
    if (authorityEnd == authority)
        return HTTP_URL_NO_HOST;

    // The port, if the authority names one. Searched from the RIGHT so that
    // the refusals below still see the whole host: "user@host" and "[::1]"
    // are rejected as host bytes rather than mistaken for ports.
    const char *colon = NULL;
    for (const char *q = authorityEnd; q > authority; q--)
        if (q[-1] == ':') { colon = q - 1; break; }

    const char *hostEnd = colon != NULL ? colon : authorityEnd;
    if (hostEnd == authority)
        return HTTP_URL_NO_HOST;
    for (const char *q = authority; q < hostEnd; q++)
        if (!is_host_byte(*q))
            return HTTP_URL_HOST_CHARS;   // userinfo, an IPv6 literal, or junk
    // THE HOST IS FOLDED, THE PATH IS NOT. A DNS name is case-insensitive by
    // definition (RFC 4343), and so is the Host header a server matches it
    // against — so the case a person typed carries no information, and
    // keeping it would put two spellings of one machine in every diagnostic
    // and every Host line. A PATH is the opposite: it is a name on somebody
    // else's filesystem, and /Case and /case are two different pages.
    if (copy_lower(out->host, sizeof(out->host), authority, (size_t)(hostEnd - authority)) >=
        sizeof(out->host))
        return HTTP_URL_TOO_LONG;

    if (colon != NULL) {
        const char *q = colon + 1;
        if (q == authorityEnd)
            return HTTP_URL_PORT;         // "host:" names no port
        uint32_t port = 0;
        for (; q < authorityEnd; q++) {
            if (!is_digit(*q))
                return HTTP_URL_PORT;
            port = port * 10 + (uint32_t)(*q - '0');
            if (port > 65535)
                return HTTP_URL_PORT;
        }
        if (port == 0)
            return HTTP_URL_PORT;
        out->port = (uint16_t)port;
        out->explicitPort = true;
    }

    // ── The path, with the query and without the fragment ───────────────
    // A fragment is the BROWSER's business — it names a place inside the
    // document and has never crossed the wire (RFC 1945 onward). Dropping it
    // here rather than sending it is not tidiness: a '#' in a request line
    // is a request the server was never asked.
    const char *pathStart = authorityEnd;
    const char *pathEnd = pathStart;
    while (*pathEnd != '\0' && *pathEnd != '#')
        pathEnd++;

    // A space or a control byte in a request line ends the line early on the
    // far side and starts a second request nobody typed. Refuse; percent-
    // encoding what a person typed is a guess about their intent, and this
    // is the wrong place to guess.
    for (const char *q = pathStart; q < pathEnd; q++)
        if ((unsigned char)*q <= 0x20 || (unsigned char)*q >= 0x7F)
            return HTTP_URL_PATH_CHARS;

    size_t wanted;
    if (pathStart == pathEnd || *pathStart != '/') {
        // "http://host", "http://host?q=1" — the origin's root, plus
        // whatever query came with it.
        out->path[0] = '/';
        wanted = 1 + copy_span(out->path + 1, sizeof(out->path) - 1, pathStart,
                               (size_t)(pathEnd - pathStart));
    } else {
        wanted = copy_span(out->path, sizeof(out->path), pathStart,
                           (size_t)(pathEnd - pathStart));
    }
    if (wanted >= sizeof(out->path))
        return HTTP_URL_TOO_LONG;

    return HTTP_URL_OK;
}

const char *http_url_reason(http_url_result_t rc)
{
    switch (rc) {
        case HTTP_URL_OK:         return "no problem";
        case HTTP_URL_NOT_A_URL:  return "not a URL (a URL starts 'scheme://')";
        case HTTP_URL_SCHEME:     return "not http or https";
        case HTTP_URL_NO_HOST:    return "no host between the '//' and the path";
        case HTTP_URL_HOST_CHARS: return "the host holds something a host name cannot"
                                         " (a user@, an IPv6 literal in brackets, or junk)";
        case HTTP_URL_PATH_CHARS: return "the path holds a space or a control byte";
        case HTTP_URL_PORT:       return "the port is not a number from 1 to 65535";
        case HTTP_URL_TOO_LONG:   return "too long to hold";
    }
    return "refused";
}

// The port a scheme implies when a URL does not spell one. Used in two
// places that must agree: the Host header (which omits a default port,
// because a server matching virtual hosts on the literal string is entitled
// to treat "example.com" and "example.com:80" as different names, and the
// short form is the one everybody writes) and http_url_render.
static uint16_t scheme_default_port(const char *scheme)
{
    return os64_streq(scheme, "https") ? 443 : 80;
}

// Write a parsed URL back out as text. The inverse of http_url_parse for
// everything the parse KEPT — which means a rendered URL has lost its
// fragment and any default port that was spelled out, both on purpose.
//
// It exists because a proxied request puts the whole address in the request
// line, and reconstructing that from the struct rather than replaying what
// the person typed is what keeps a fragment (`#top`, which has never crossed
// the wire) from reaching a server that was never asked for it.
bool http_url_render(const http_url_t *url, char *out, size_t cap)
{
    int32_t n;
    if (url->port != scheme_default_port(url->scheme))
        n = os64_snprintf(out, cap, "%s://%s:%u%s", url->scheme, url->host,
                          (unsigned)url->port, url->path);
    else
        n = os64_snprintf(out, cap, "%s://%s%s", url->scheme, url->host, url->path);
    return n > 0 && (size_t)n < cap;
}

bool http_request(char *out, size_t cap, const http_url_t *url, bool absoluteForm)
{
    char host[HTTP_HOST_MAX + 8];
    if (url->port != scheme_default_port(url->scheme)) {
        if (os64_snprintf(host, sizeof(host), "%s:%u", url->host, (unsigned)url->port) >=
            (int32_t)sizeof(host))
            return false;
    } else {
        if (os64_strcopy(host, sizeof(host), url->host) >= sizeof(host))
            return false;
    }

    // THE REQUEST TARGET IS THE WHOLE URL WHEN A PROXY IS ANSWERING. That is
    // "absolute-form" (RFC 7230 §5.3.2), and it is how every proxy since the
    // CERN one in 1994 has been told which origin a request is for — the
    // connection goes to the proxy, so the path alone would name a file on
    // the proxy instead of a page on the web. `Host:` still names the ORIGIN,
    // which is what lets the proxy pass it on unchanged.
    char target[HTTP_SCHEME_MAX + HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
    const char *requestTarget = url->path;
    if (absoluteForm) {
        if (!http_url_render(url, target, sizeof(target)))
            return false;
        requestTarget = target;
    }

    // Name both codings this client can turn back into the representation.
    // A missing Accept-Encoding means any coding may be acceptable; an
    // explicit list lets a server choose gzip without licensing br, compress,
    // or another envelope that would otherwise have to be refused afterward.
    int32_t n = os64_snprintf(out, cap,
                              "GET %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "User-Agent: os64get/1 (os64)\r\n"
                              "Accept-Encoding: gzip, identity\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              requestTarget, host);
    return n > 0 && (size_t)n < cap;
}


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
// written before 1998. Read strictly it is an address os64get cannot fetch
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
#define HTTP_JOIN_MAX (HTTP_PATH_MAX + HTTP_LINE_MAX + 2)

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
        char raw[HTTP_JOIN_MAX];
        char dots[HTTP_JOIN_MAX];
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

bool http_url_absolute(const http_url_t *base, const char *location,
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
        char whole[HTTP_LINE_MAX];
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
    char ref[HTTP_LINE_MAX];
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

    char path[HTTP_JOIN_MAX];
    const char *query;

    if (refPathLen == 0) {
        // `?page=2`, or a reference that was nothing but a fragment: the page
        // stays, and only the question changes. With no '?' of its own the
        // reference is the SAME address, which is a redirect in a circle —
        // said plainly by whoever compares them, not papered over here.
        copy_span(path, sizeof(path), base->path, basePathLen);
        query = (*refQuery == '?') ? refQuery : baseQuery;
    } else {
        char joined[HTTP_JOIN_MAX];
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
    // when it is not the default FOR THAT SCHEME, for the same reason the
    // Host header omits it. (Codex review, 2026-09-02.)
    int32_t n;
    if (base->port != scheme_default_port(base->scheme))
        n = os64_snprintf(out, cap, "%s://%s:%u%s%s", base->scheme, base->host,
                          (unsigned)base->port, path, query);
    else
        n = os64_snprintf(out, cap, "%s://%s%s%s", base->scheme, base->host, path, query);
    return n > 0 && (size_t)n < cap;
}

// ── The stream ──────────────────────────────────────────────────────────

void http_stream_init(http_stream_t *s, http_source_fn source, void *ctx)
{
    os64_memset(s, 0, sizeof(*s));
    s->source = source;
    s->ctx = ctx;
}

// 1 there are unspent bytes, 0 the peer is done, -1 the connection broke.
static int stream_fill(http_stream_t *s)
{
    if (s->next < s->have)
        return 1;
    if (s->failed)
        return -1;
    if (s->ended)
        return 0;

    s->next = 0;
    s->have = 0;
    int64_t n = s->source(s->ctx, s->buf, sizeof(s->buf));
    if (n < 0) { s->failed = true; return -1; }
    if (n == 0) { s->ended = true; return 0; }
    s->have = (size_t)n;
    return 1;
}

typedef enum {
    LINE_OK = 0,
    LINE_LONG,     // read and thrown away: longer than we will hold
    LINE_NUL,      // a NUL byte: not a line at all, and not one to keep reading
    LINE_END,      // the peer stopped talking mid-head
    LINE_ERROR
} line_result_t;

// One line, CRLF or bare LF, the terminator stripped. `*consumed` counts the
// RAW bytes eaten — including the ones a long line threw away — because that
// is the number the head's size cap has to be measured in.
//
// `budget` bounds the DRAIN, and it is the difference between a cap and a
// promise. Without it, a peer that streams forever and never sends a newline
// is read forever: the caller's cap is checked between lines, and this call
// would never return to be checked. Running out of budget reads as an
// over-long line, which is what it is.
//
// A NUL BYTE IS REFUSED WHEREVER IT FALLS IN THE LINE, because the line is
// handed on as a C string and a NUL inside it ends the string early. A line
// that BEGAN with one read as empty — the blank line that ends the head — so
// `\0Transfer-Encoding: chunked` was taken for the terminator, the framing
// header behind it was read as body, and the raw chunk markers were published
// as the file. No HTTP head may contain NUL (RFC 9110 §5.5 forbids it in
// values, and a name is a token), so there is no reply to save by tolerating
// it. (Codex review round 4, 2026-09-03.)
static line_result_t line_read(http_stream_t *s, char *out, size_t cap,
                               size_t budget, size_t *consumed)
{
    size_t len = 0;
    bool overlong = false;

    *consumed = 0;
    for (;;) {
        if (*consumed >= budget)
            return LINE_LONG;

        int r = stream_fill(s);
        if (r < 0)
            return LINE_ERROR;
        if (r == 0)
            return LINE_END;         // no newline ever came: the head is a fragment

        char c = (char)s->buf[s->next++];
        (*consumed)++;
        if (c == '\n')
            break;
        if (c == '\0')
            return LINE_NUL;
        if (len + 1 < cap)
            out[len++] = c;
        else
            overlong = true;
    }

    if (len > 0 && out[len - 1] == '\r')
        len--;
    out[len] = '\0';
    return overlong ? LINE_LONG : LINE_OK;
}

// ── The answer ──────────────────────────────────────────────────────────

// "HTTP/1.1 404 Not Found" — the version is read and ignored (a 1.0 server
// answers a 1.1 request with a 1.0 reply, and every framing either may use is
// one http_body_read reads), the code must be exactly three digits, and the
// reason phrase is the server's own words, kept so a refusal can be reported
// in them rather than in ours.
static bool status_parse(const char *line, http_response_t *out)
{
    if (line[0] != 'H' || line[1] != 'T' || line[2] != 'T' || line[3] != 'P' || line[4] != '/')
        return false;

    const char *p = line + 5;
    if (!is_digit(*p))
        return false;
    while (is_digit(*p) || *p == '.')
        p++;
    if (*p != ' ')
        return false;
    while (*p == ' ')
        p++;

    if (!is_digit(p[0]) || !is_digit(p[1]) || !is_digit(p[2]))
        return false;
    out->status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    p += 3;
    if (*p != '\0' && *p != ' ')
        return false;                    // "2000" is not a status code
    while (*p == ' ')
        p++;

    for (const char *q = p; *q != '\0'; q++)
        if (!is_field_byte(*q))
            return false;            // a reason phrase that would drive the terminal
    copy_span(out->reason, sizeof(out->reason), p, os64_strlen(p));
    return true;
}

// Store a coding name. A SECOND, DIFFERENT answer to the same question is
// refused rather than resolved: which of two Transfer-Encodings a stream is
// in has no correct guess, and guessing wrong writes a file of framing bytes.
// THE SAME answer twice is not a conflict, it is a LIST — RFC 7230 §3.2.2
// joins repeated field lines with commas, so two `Content-Encoding: gzip`
// lines say `gzip, gzip`: encoded twice. The list is kept as the value, so
// the caller meets a coding it does not speak and refuses it by name,
// instead of undoing one layer and publishing the other as the page (Codex
// review of PR #54, 2026-09-03).
//
// THE VALUE IS JUDGED BEFORE IT IS STORED, because the slot is small and
// "absent" is spelled by an empty slot. An EMPTY value stored there read as
// no header at all — so `Transfer-Encoding:` with nothing after it slipped
// past the conflict check and the framing refusal both, and a chunked body
// or a body beside a Content-Length went through as if nothing framed it
// (Codex review round 7, 2026-09-03). A value LONGER than the slot used to
// be folded into it truncated, so two different long values compared equal
// and a truncated name matched nothing — safe only by accident. A coding
// this program will act on is a short word; one that does not fit, or a
// list that no longer does, is a framing header it cannot read, and says so.
static http_head_result_t coding_take(char *slot, size_t cap, const char *value, size_t vlen)
{
    if (vlen == 0)
        return HTTP_HEAD_SYNTAX;      // a coding header must name a coding
    if (vlen >= HTTP_TOKEN_MAX)
        return HTTP_HEAD_FRAMING;

    char folded[HTTP_TOKEN_MAX];
    copy_lower(folded, sizeof(folded), value, vlen);
    if (slot[0] == '\0') {
        os64_strcopy(slot, cap, folded);
        return HTTP_HEAD_OK;
    }
    if (!os64_streq(slot, folded))
        return HTTP_HEAD_CONFLICT;

    size_t have = os64_strlen(slot);
    if (have + 2 + vlen >= cap)
        return HTTP_HEAD_FRAMING;
    slot[have] = ',';
    slot[have + 1] = ' ';
    os64_strcopy(slot + have + 2, cap - have - 2, folded);
    return HTTP_HEAD_OK;
}

static http_head_result_t header_take(char *line, http_response_t *out)
{
    // THE NAME IS A TOKEN, judged by the same is_token_byte the over-long
    // path uses — which is the point, because these two used to judge it
    // differently and the looser one was reachable. That single rule covers
    // what were three separate checks: a line beginning with whitespace
    // (obs-fold, a continuation of the line above, deprecated by RFC 7230
    // §3.2.4 and required to be rejected by anything that is not a proxy), a
    // blank before the colon, and any other byte a field name may not hold.
    // Each of those, left through, hides a framing header under a name that
    // matches nothing and gets its chunk markers published as a file.
    char *colon = line;
    while (*colon != '\0' && *colon != ':') {
        if (!is_token_byte(*colon))
            return HTTP_HEAD_SYNTAX;
        colon++;
    }
    if (*colon != ':' || colon == line)
        return HTTP_HEAD_SYNTAX;
    *colon = '\0';

    char *value = colon + 1;
    while (is_blank(*value))
        value++;
    size_t vlen = os64_strlen(value);
    while (vlen > 0 && is_blank(value[vlen - 1]))
        value[--vlen] = '\0';
    for (size_t i = 0; i < vlen; i++)
        if (!is_field_byte(value[i]))
            return HTTP_HEAD_SYNTAX;  // a Location that would clear the screen

    if (os64_streq_nocase(line, "Content-Length")) {
        uint64_t length = 0;
        if (!os64_parse_u64(value, &length))
            return HTTP_HEAD_SYNTAX;
        if (out->hasLength && out->length != length)
            return HTTP_HEAD_CONFLICT;
        out->hasLength = true;
        out->length = length;
    } else if (os64_streq_nocase(line, "Transfer-Encoding")) {
        http_head_result_t rc = coding_take(out->transferEncoding,
                                            sizeof(out->transferEncoding), value, vlen);
        if (rc != HTTP_HEAD_OK)
            return rc;
    } else if (os64_streq_nocase(line, "Content-Encoding")) {
        http_head_result_t rc = coding_take(out->contentEncoding,
                                            sizeof(out->contentEncoding), value, vlen);
        if (rc != HTTP_HEAD_OK)
            return rc;
    } else if (os64_streq_nocase(line, "Location")) {
        // Location is a singleton: a redirect has ONE destination, and two
        // different ones is a reply with no unambiguous destination at all —
        // an intermediary that combines or reorders the lines would send
        // another client somewhere else. The same answer given twice is one
        // answer, as for Content-Length. PRESENCE IS ITS OWN FLAG, because
        // an empty value is a legal Location that names nowhere, and
        // spelling "seen" as "non-empty" let `Location:` then `Location:
        // /target` through while the reverse order was refused — the same
        // reply, judged by field order (Codex review of PR #60, rounds 1
        // and 2).
        char seen[HTTP_LINE_MAX];
        copy_span(seen, sizeof(seen), value, vlen);
        if (out->hasLocation)
            return os64_streq(out->location, seen) ? HTTP_HEAD_OK : HTTP_HEAD_CONFLICT;
        out->hasLocation = true;
        copy_span(out->location, sizeof(out->location), value, vlen);
    }
    return HTTP_HEAD_OK;
}

// WHAT WAS THAT LINE, before we throw it away. An over-long header is
// dropped rather than fatal (see HTTP_LINE_MAX in http.h), and that is safe
// only for a header nothing depends on. It is NOT safe for the three that
// decide how the body is framed and coded: a server chooses the length of
// its own header lines, so `Transfer-Encoding:` followed by three kilobytes
// of the optional whitespace RFC 7230 permits and then `chunked` would be
// dropped, leave the reply looking unframed, and end with raw chunk lengths
// published as the file — the precise failure the refusal rule exists to
// prevent. (Codex review, 2026-09-02. The rule was right; the premise under
// it — "the headers this acts on are all short" — was the server's to break,
// not ours to assume.)
//
// The NAME is readable even when the value is not: it arrives first and is
// short, so the truncated prefix still carries it. A line so long that not
// even a colon fits is refused too — an unidentifiable multi-kilobyte header
// is not something to shrug at.
static http_head_result_t overlong_verdict(const char *prefix)
{
    // The name, judged by is_token_byte — the SAME rule header_take applies,
    // so the two paths cannot disagree about what a header is called. Any
    // byte that is not a token byte ends the walk: a blank, a control byte,
    // or the obs-fold whitespace that starts a continuation line.
    char name[HTTP_TOKEN_MAX];
    size_t n = 0;
    while (prefix[n] != '\0' && prefix[n] != ':') {
        if (!is_token_byte(prefix[n]))
            return HTTP_HEAD_SYNTAX;  // malformed, exactly as the short form would be
        if (n + 1 >= sizeof(name))
            return HTTP_HEAD_OK;      // a legal name, too long to BE one of the three
        name[n] = prefix[n];
        n++;
    }
    if (prefix[n] != ':' || n == 0)
        return HTTP_HEAD_FRAMING;     // no name and no colon in the whole prefix
    name[n] = '\0';

    if (os64_streq_nocase(name, "Content-Length") ||
        os64_streq_nocase(name, "Transfer-Encoding") ||
        os64_streq_nocase(name, "Content-Encoding"))
        return HTTP_HEAD_FRAMING;

    return HTTP_HEAD_OK;              // genuinely nothing depends on it
}

static http_head_result_t head_read_once(http_stream_t *s, http_response_t *out)
{
    char line[HTTP_LINE_MAX];
    size_t consumed = 0;
    size_t used = 0;

    os64_memset(out, 0, sizeof(*out));

    switch (line_read(s, line, sizeof(line), HTTP_HEAD_MAX, &consumed)) {
        case LINE_OK:    break;
        // Two kilobytes without a CRLF is not a status line, and neither is
        // sixty-four of them: both come back LINE_LONG, and the answer to
        // both is that whatever is on the far end is not an HTTP server.
        case LINE_LONG:  return HTTP_HEAD_STATUS;
        case LINE_NUL:   return HTTP_HEAD_STATUS;
        case LINE_END:   return HTTP_HEAD_SOURCE;
        case LINE_ERROR: return HTTP_HEAD_SOURCE;
    }
    used += consumed;
    if (!status_parse(line, out))
        return HTTP_HEAD_STATUS;

    // `fields` counts header FIELDS — dropped over-long ones included, since
    // they were fields too — and the blank line is not one, so it is
    // recognised before the count is judged: a reply with exactly
    // HTTP_HEADERS_MAX fields is legal, and the cap means what it says.
    size_t fields = 0;
    for (;;) {
        line_result_t r = line_read(s, line, sizeof(line), HTTP_HEAD_MAX - used + 1, &consumed);
        used += consumed;
        if (used > HTTP_HEAD_MAX)
            return HTTP_HEAD_TOO_MUCH;

        if (r == LINE_END || r == LINE_ERROR)
            return HTTP_HEAD_SOURCE;
        if (r == LINE_NUL)
            return HTTP_HEAD_SYNTAX;
        if (r == LINE_OK && line[0] == '\0')
        {
            // The blank line: the head is whole. A HEAD THAT NAMES BOTH A
            // TRANSFER CODING AND A LENGTH IS REFUSED HERE, as the same
            // conflict two Content-Lengths are: when both are present the
            // coding is the framing and the length is to be ignored (RFC
            // 9112 §6.3), so a body read to the length — even under a
            // coding of `identity` that changes nothing — is a PREFIX of the
            // body, published as the whole of it. A server must never send
            // both; one that does is confused or hostile, and neither gets
            // a guess. (Codex review round 6, 2026-09-03.)
            if (out->hasLength && out->transferEncoding[0] != '\0')
                return HTTP_HEAD_CONFLICT;
            return HTTP_HEAD_OK;
        }

        if (fields >= HTTP_HEADERS_MAX)
            return HTTP_HEAD_TOO_MUCH;
        fields++;

        if (r == LINE_LONG)
        {
            // Dropped, but only once we know nothing depends on it.
            http_head_result_t verdict = overlong_verdict(line);
            if (verdict != HTTP_HEAD_OK)
                return verdict;
            continue;
        }

        http_head_result_t rc = header_take(line, out);
        if (rc != HTTP_HEAD_OK)
            return rc;
    }
}

// A 1xx is an INTERIM answer — the server clearing its throat before the
// real one — and a client that mistook one for the response would save an
// empty file and call it a page. They are read and discarded, but not
// forever: a peer that only ever clears its throat is a peer to hang up on.
#define HTTP_INTERIM_MAX 8

http_head_result_t http_head_read(http_stream_t *s, http_response_t *out)
{
    for (int i = 0; i <= HTTP_INTERIM_MAX; i++) {
        http_head_result_t rc = head_read_once(s, out);
        if (rc != HTTP_HEAD_OK)
            return rc;
        // 101 IS NOT INTERIM. After it the connection speaks whatever was
        // upgraded to, so reading on for "the real reply" would parse a
        // foreign protocol as an HTTP head — and hang on the idle deadline
        // when it is not one, or accept it as the download when its first
        // bytes happen to look like a status line. Nothing was asked for, so
        // there is nothing to follow. (Codex review round 7, 2026-09-03.)
        if (out->status == 101)
            return HTTP_HEAD_SWITCHED;
        if (out->status < 100 || out->status >= 200)
            return HTTP_HEAD_OK;
    }
    return HTTP_HEAD_TOO_MUCH;
}

const char *http_head_reason(http_head_result_t rc)
{
    switch (rc) {
        case HTTP_HEAD_OK:       return "no problem";
        case HTTP_HEAD_SOURCE:   return "the connection ended before the reply was whole";
        case HTTP_HEAD_STATUS:   return "the first line is not an HTTP status line";
        case HTTP_HEAD_SYNTAX:   return "a header line is not 'Name: value'";
        case HTTP_HEAD_TOO_MUCH: return "more headers than this program will read";
        case HTTP_HEAD_CONFLICT: return "the headers answer one question two ways";
        case HTTP_HEAD_FRAMING:  return "a header saying how to read the body was too long to read";
        case HTTP_HEAD_SWITCHED: return "the server switched protocols (101), which a fetch cannot follow";
    }
    return "refused";
}

int64_t http_stream_read(http_stream_t *s, void *out, size_t cap)
{
    if (cap == 0)
        return 0;

    size_t held = s->have - s->next;
    if (held == 0) {
        if (s->failed)
            return -1;
        if (s->ended)
            return 0;
        // A caller asking for more than this buffer holds gets the socket
        // read straight into its own buffer. The head's over-read is what
        // this buffer exists for; a five-megabyte body has no reason to be
        // copied twice on the way to the disk.
        if (cap >= sizeof(s->buf)) {
            int64_t n = s->source(s->ctx, out, cap);
            if (n < 0) { s->failed = true; return -1; }
            if (n == 0) { s->ended = true; return 0; }
            return n;
        }
        int r = stream_fill(s);
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        held = s->have - s->next;
    }

    size_t n = held < cap ? held : cap;
    os64_memcpy(out, s->buf + s->next, n);
    s->next += n;
    return (int64_t)n;
}

// ── The body, with its framing taken off ────────────────────────────────

// Where a chunked reader is between calls. The framing is a grammar
// (RFC 9112 §7.1) — size line, data, CRLF, repeat; a zero size, then trailer
// lines to a blank one — and a read may hand the caller only part of one
// chunk's data, so the position has to outlive the call.
enum {
    CHUNK_SIZE = 0,   // a chunk-size line is next
    CHUNK_DATA,       // inside chunk-data, `remaining` bytes to go
    CHUNK_DATA_END,   // the CRLF that closes a chunk's data is next
    CHUNK_TRAILER,    // after the last chunk: trailer lines until a blank one
    CHUNK_FINISHED
};

bool http_body_open(http_body_t *b, http_stream_t *s, const http_response_t *reply)
{
    os64_memset(b, 0, sizeof(*b));
    b->s = s;
    b->result = HTTP_BODY_OPEN;

    if (reply->transferEncoding[0] != '\0' &&
        !os64_streq_nocase(reply->transferEncoding, "identity")) {
        if (!os64_streq_nocase(reply->transferEncoding, "chunked"))
            return false;
        b->framing = HTTP_FRAMING_CHUNKED;
        b->state = CHUNK_SIZE;
        return true;
    }
    if (reply->hasLength) {
        b->framing = HTTP_FRAMING_LENGTH;
        b->remaining = reply->length;
        return true;
    }
    b->framing = HTTP_FRAMING_CLOSE;
    return true;
}

static int64_t body_finish(http_body_t *b, http_body_result_t rc)
{
    b->result = rc;
    return (rc == HTTP_BODY_DONE || rc == HTTP_BODY_CUT) ? 0 : -1;
}

// Read `cap` bytes at most of the current chunk's data (or of a length-framed
// body — the same arithmetic). Ending here is a CUT: the peer promised more.
static int64_t body_read_counted(http_body_t *b, void *out, size_t cap)
{
    if (b->remaining < (uint64_t)cap)
        cap = (size_t)b->remaining;
    int64_t n = http_stream_read(b->s, out, cap);
    if (n < 0)
        return body_finish(b, HTTP_BODY_BROKE);
    if (n == 0)
        return body_finish(b, HTTP_BODY_CUT);
    b->remaining -= (uint64_t)n;
    b->delivered += (uint64_t)n;
    return n;
}

// "1a2f;ext=val" → 0x1a2f. Hex digits, then optional blanks, then either the
// end or a ';' opening a chunk extension, which is ignored whole: this code
// defines none, and RFC 9112 §7.1.1 says a recipient that does not define one
// ignores it. Anything else on the line is not a chunk size.
//
// THE BOUND IS THE VALUE, NOT THE DIGIT COUNT. The grammar is 1*HEXDIG, so
// `00000000000000001` is a legal spelling of 1 and a digit cap refused it
// (Codex review of PR #60, round 2); what no body can have is a size past
// 2^64-1, which is caught as the arithmetic is about to lose it. Leading
// zeros are bounded by the line buffer the size line arrives in.
static bool chunk_size_parse(const char *line, uint64_t *size)
{
    const char *p = line;
    uint64_t value = 0;
    size_t digits = 0;

    for (;; p++) {
        uint64_t d;
        if (is_digit(*p))                 d = (uint64_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')  d = (uint64_t)(*p - 'a') + 10;
        else if (*p >= 'A' && *p <= 'F')  d = (uint64_t)(*p - 'A') + 10;
        else break;
        if (value > (UINT64_MAX >> 4))
            return false;
        value = value << 4 | d;
        digits++;
    }
    if (digits == 0)
        return false;
    while (is_blank(*p))
        p++;
    if (*p != '\0' && *p != ';')
        return false;
    *size = value;
    return true;
}

// A token, then a colon, somewhere. Enough to tell a trailer field from junk
// without parsing what this code does not read.
static bool field_line_shaped(const char *line)
{
    if (!is_token_byte(line[0]))
        return false;
    for (const char *p = line; *p != '\0'; p++)
        if (*p == ':')
            return true;
    return false;
}

static int64_t body_read_chunked(http_body_t *b, void *out, size_t cap)
{
    char line[HTTP_LINE_MAX];
    size_t consumed;

    for (;;) {
        switch (b->state) {
        case CHUNK_SIZE: {
            line_result_t r = line_read(b->s, line, sizeof(line), sizeof(line), &consumed);
            if (r == LINE_END)   return body_finish(b, HTTP_BODY_CUT);
            if (r == LINE_ERROR) return body_finish(b, HTTP_BODY_BROKE);
            if (r == LINE_NUL)   return body_finish(b, HTTP_BODY_SYNTAX);
            if (r == LINE_LONG)  return body_finish(b, HTTP_BODY_TOO_MUCH);
            uint64_t size;
            if (!chunk_size_parse(line, &size))
                return body_finish(b, HTTP_BODY_SYNTAX);
            if (size == 0) {
                b->state = CHUNK_TRAILER;
                break;
            }
            b->remaining = size;
            b->state = CHUNK_DATA;
            break;
        }
        case CHUNK_DATA: {
            int64_t n = body_read_counted(b, out, cap);
            if (n > 0 && b->remaining == 0)
                b->state = CHUNK_DATA_END;
            return n;
        }
        case CHUNK_DATA_END: {
            // Exactly a line terminator. The budget of two is the length of
            // CRLF: a third byte before the newline is data the size line
            // did not count, and line_read reports it as an over-long line.
            line_result_t r = line_read(b->s, line, sizeof(line), 2, &consumed);
            if (r == LINE_END)   return body_finish(b, HTTP_BODY_CUT);
            if (r == LINE_ERROR) return body_finish(b, HTTP_BODY_BROKE);
            if (r != LINE_OK || line[0] != '\0')
                return body_finish(b, HTTP_BODY_SYNTAX);
            b->state = CHUNK_SIZE;
            break;
        }
        case CHUNK_TRAILER: {
            // Trailer fields are read to find their end and otherwise
            // ignored: nothing this program does depends on one, and a
            // sender may not put framing there (RFC 9110 §6.5.1). Bounded
            // exactly as the head is, or the trailer is the endless-head
            // attack wearing a different hat. An over-long trailer line is
            // dropped, as an over-long non-framing header is.
            line_result_t r = line_read(b->s, line, sizeof(line),
                                        HTTP_HEAD_MAX - b->trailerUsed + 1, &consumed);
            b->trailerUsed += consumed;
            if (b->trailerUsed > HTTP_HEAD_MAX)
                return body_finish(b, HTTP_BODY_TOO_MUCH);
            if (r == LINE_END)   return body_finish(b, HTTP_BODY_CUT);
            if (r == LINE_ERROR) return body_finish(b, HTTP_BODY_BROKE);
            if (r == LINE_NUL)   return body_finish(b, HTTP_BODY_SYNTAX);
            if (r == LINE_OK && line[0] == '\0') {
                b->state = CHUNK_FINISHED;
                return body_finish(b, HTTP_BODY_DONE);
            }
            if (b->trailers >= HTTP_HEADERS_MAX)
                return body_finish(b, HTTP_BODY_TOO_MUCH);
            b->trailers++;
            // The shape of a field line, checked so far as it costs nothing:
            // a name, a colon. A line beginning with whitespace is obs-fold,
            // refused here for the reason the head refuses it.
            if (r == LINE_OK && !field_line_shaped(line))
                return body_finish(b, HTTP_BODY_SYNTAX);
            break;
        }
        default:
            return body_finish(b, HTTP_BODY_DONE);
        }
    }
}

int64_t http_body_read(http_body_t *b, void *out, size_t cap)
{
    if (b->result != HTTP_BODY_OPEN)
        return (b->result == HTTP_BODY_DONE || b->result == HTTP_BODY_CUT) ? 0 : -1;
    if (cap == 0)
        return 0;

    switch (b->framing) {
    case HTTP_FRAMING_LENGTH:
        if (b->remaining == 0)
            return body_finish(b, HTTP_BODY_DONE);
        {
            int64_t n = body_read_counted(b, out, cap);
            if (n > 0 && b->remaining == 0)
                b->result = HTTP_BODY_DONE;   // the next call answers 0
            return n;
        }
    case HTTP_FRAMING_CHUNKED:
        return body_read_chunked(b, out, cap);
    default: {
        int64_t n = http_stream_read(b->s, out, cap);
        if (n < 0)
            return body_finish(b, HTTP_BODY_BROKE);
        if (n == 0)
            return body_finish(b, HTTP_BODY_DONE);
        b->delivered += (uint64_t)n;
        return n;
    }
    }
}

const char *http_body_reason(http_body_result_t rc)
{
    switch (rc) {
    case HTTP_BODY_OPEN:     return "the body is still arriving";
    case HTTP_BODY_DONE:     return "the body arrived whole";
    case HTTP_BODY_CUT:      return "the connection closed before the body was whole";
    case HTTP_BODY_BROKE:    return "the connection broke";
    case HTTP_BODY_SYNTAX:   return "the chunk framing is not chunk framing";
    case HTTP_BODY_TOO_MUCH: return "more chunk framing than this will read";
    }
    return "unknown";
}
