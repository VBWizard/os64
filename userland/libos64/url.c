// url.c — the common URL grammar. See os64/url.h for what is here and what
// is deliberately not.

#include "os64/url.h"

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
