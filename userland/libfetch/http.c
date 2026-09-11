// http.c — the world's dialect. See http.h for what this is and is not.

#include "fetch/http.h"

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

static uint16_t scheme_default_port(const char *scheme);

// The grammar is os64_url_parse's; what is left here is the part that is
// HTTP's own. One refusal maps to another, one for one, so that os64get's
// diagnostics keep saying "not http or https" in http's words rather than
// the library's more general ones.
static http_url_result_t url_result_from(os64_url_result_t rc)
{
    switch (rc) {
        case OS64_URL_OK:         return HTTP_URL_OK;
        case OS64_URL_NOT_A_URL:  return HTTP_URL_NOT_A_URL;
        case OS64_URL_NO_HOST:    return HTTP_URL_NO_HOST;
        case OS64_URL_HOST_CHARS: return HTTP_URL_HOST_CHARS;
        case OS64_URL_PATH_CHARS: return HTTP_URL_PATH_CHARS;
        case OS64_URL_PORT:       return HTTP_URL_PORT;
        case OS64_URL_TOO_LONG:   return HTTP_URL_TOO_LONG;
    }
    return HTTP_URL_NOT_A_URL;
}

http_url_result_t http_url_parse(const char *url, http_url_t *out)
{
    os64_memset(out, 0, sizeof(*out));

    os64_url_t parsed;
    os64_url_result_t rc = os64_url_parse(url, &parsed);

    // AN UNSUPPORTED SCHEME OUTRANKS EVERY OTHER COMPLAINT, which is why the
    // scheme is judged before `rc` is. Telling somebody that their `ftp://`
    // address has a bad port answers a question they cannot use the answer
    // to; naming the scheme sends them somewhere that can help. The library
    // fills the scheme before it looks at anything else, so it is readable
    // on a failed parse as well as a good one — and http.h promises it.
    if (parsed.scheme[0] != '\0') {
        copy_span(out->scheme, sizeof(out->scheme), parsed.scheme,
                  os64_strlen(parsed.scheme));
        if (!os64_streq(out->scheme, "http") && !os64_streq(out->scheme, "https"))
            return HTTP_URL_SCHEME;
    }
    if (rc != OS64_URL_OK)
        return url_result_from(rc);

    // Both HTTP schemes share URL grammar. The caller selects direct TLS
    // or an explicit proxy and owns the trust/routing decisions.
    copy_span(out->host, sizeof(out->host), parsed.host, os64_strlen(parsed.host));
    copy_span(out->path, sizeof(out->path), parsed.path, os64_strlen(parsed.path));

    // The library reports port 0 for "the URL named none", which is where
    // the scheme's default gets applied — the one place in this program
    // that knows what http and https imply.
    out->port = parsed.port != 0 ? parsed.port : scheme_default_port(out->scheme);
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

void http_url_to_os64(const http_url_t *url, os64_url_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    copy_span(out->scheme, sizeof(out->scheme), url->scheme, os64_strlen(url->scheme));
    copy_span(out->host, sizeof(out->host), url->host, os64_strlen(url->host));
    copy_span(out->path, sizeof(out->path), url->path, os64_strlen(url->path));
    out->port = url->port != scheme_default_port(url->scheme) ? url->port : 0;
}

// A caller-supplied header value, judged byte by byte. `whole_lines` is for
// extra_headers, where CR LF is the line ending the caller wrote and is
// legal only as the pair, at a line's end: a lone LF, a lone CR, or a CR
// with anything but LF after it is the splitting shape and refuses.
static bool extras_clean(const char *text, bool whole_lines)
{
    if (text == NULL)
        return true;
    for (const char *p = text; *p != '\0'; p++) {
        if (whole_lines && p[0] == '\r' && p[1] == '\n') {
            p++;
            continue;
        }
        if (!is_field_byte(*p))
            return false;
    }
    if (whole_lines && text[0] != '\0') {
        size_t n = os64_strlen(text);
        if (n < 2 || text[n - 2] != '\r' || text[n - 1] != '\n')
            return false;         // a last line with no ending would swallow the blank line
    }
    return true;
}

bool http_request_extras_ok(const http_request_extras_t *extras)
{
    if (extras == NULL)
        return true;
    return extras_clean(extras->user_agent, false) && extras_clean(extras->accept, false) &&
           extras_clean(extras->extra_headers, true);
}

bool http_request(char *out, size_t cap, const http_url_t *url, bool absoluteForm,
                  const http_request_extras_t *extras)
{
    static const http_request_extras_t none = {0};
    if (extras == NULL)
        extras = &none;
    if (!http_request_extras_ok(extras))
        return false;

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
    // The caller's headers go between the fixed ones and the blank line, in
    // the order given; the blank line is written LAST and only here.
    int32_t n = os64_snprintf(out, cap,
                              "GET %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "%s%s%s"
                              "%s%s%s"
                              "Accept-Encoding: gzip, identity\r\n"
                              "Connection: close\r\n"
                              "%s"
                              "\r\n",
                              requestTarget, host,
                              extras->user_agent ? "User-Agent: " : "",
                              extras->user_agent ? extras->user_agent : "",
                              extras->user_agent ? "\r\n" : "",
                              extras->accept ? "Accept: " : "",
                              extras->accept ? extras->accept : "",
                              extras->accept ? "\r\n" : "",
                              extras->extra_headers ? extras->extra_headers : "");
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
    } else if (os64_streq_nocase(line, "Content-Type")) {
        // `type/subtype; charset=x; other=y`. Both the type and the charset
        // label are folded to lowercase (RFC 2045: a media type is
        // case-insensitive; the Encoding Standard: so is a label), quotes
        // around the charset stripped.
        // Two Content-Type lines is a reply that does not know what it is
        // sending; the first is kept and the second ignored, the same
        // "nothing about the framing depends on it" reasoning that lets an
        // over-long one be dropped — refusing here would cost a fetch over a
        // header the consumer only advises on.
        if (out->contentType[0] != '\0')
            return HTTP_HEAD_OK;
        size_t tlen = 0;
        while (tlen < vlen && value[tlen] != ';' && !is_blank(value[tlen]))
            tlen++;
        if (tlen == 0 || tlen >= sizeof(out->contentType))
            return HTTP_HEAD_OK;      // unreadable type: the consumer sniffs instead
        copy_lower(out->contentType, sizeof(out->contentType), value, tlen);

        const char *p = value + tlen;
        while (*p != '\0') {
            while (*p == ';' || is_blank(*p))
                p++;
            const char *name = p;
            while (*p != '\0' && *p != '=' && *p != ';')
                p++;
            if (*p != '=')
                continue;
            size_t nlen = (size_t)(p - name);
            while (nlen > 0 && is_blank(name[nlen - 1]))
                nlen--;                   // blanks around '=' are tolerated, not licensed
            p++;
            while (is_blank(*p))
                p++;
            bool quoted = (*p == '"');
            if (quoted)
                p++;
            const char *cs = p;
            while (*p != '\0' && *p != ';' && !(quoted && *p == '"'))
                p++;
            size_t clen = (size_t)(p - cs);
            while (clen > 0 && is_blank(cs[clen - 1]))
                clen--;
            if (quoted && *p == '"')
                p++;
            char pname[16];
            if (nlen < sizeof(pname))
                copy_lower(pname, sizeof(pname), name, nlen);
            else
                pname[0] = '\0';
            if (os64_streq(pname, "charset") && clen != 0 && clen < sizeof(out->charset)) {
                copy_lower(out->charset, sizeof(out->charset), cs, clen);
                break;
            }
        }
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
// only for a header nothing depends on. It is NOT safe for the headers that
// decide how the body is framed and coded: a server chooses the length of
// its own header lines, so `Transfer-Encoding:` followed by three kilobytes
// of the optional whitespace RFC 7230 permits and then `chunked` would be
// dropped, leave the reply looking unframed, and end with raw chunk lengths
// published as the file — the precise failure the refusal rule exists to
// prevent. (Codex review, 2026-09-02. The rule was right; the premise under
// it — "the headers this acts on are all short" — was the server's to break,
// not ours to assume.) NOR is it safe for Location, since redirects are
// followed: Location is a singleton, and dropping an unreadable one lets a
// readable twin through the conflict check — `Location: /safe` beside a
// padded `Location: /evil` reads as an unambiguous /safe, in either order
// (Codex review of PR #60, round 3). A destination too long to read is a
// redirect this program cannot honestly follow, whatever the status.
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
            return HTTP_HEAD_OK;      // a legal name, too long to BE one this depends on
        name[n] = prefix[n];
        n++;
    }
    if (prefix[n] != ':' || n == 0)
        return HTTP_HEAD_FRAMING;     // no name and no colon in the whole prefix
    name[n] = '\0';

    if (os64_streq_nocase(name, "Content-Length") ||
        os64_streq_nocase(name, "Transfer-Encoding") ||
        os64_streq_nocase(name, "Content-Encoding") ||
        os64_streq_nocase(name, "Location"))
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
        case HTTP_HEAD_FRAMING:  return "a header this fetch depends on was too long to read";
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

// A token, then a colon: the NAME is every byte before the colon, and each
// one is judged by is_token_byte — the rule header_take applies, so the
// trailer section cannot be looser about what a field is called than the
// head is (it was: only the first byte was checked, and `X Bad: value`
// ended a body as DONE — Codex review of PR #60, round 4). Nothing past the
// colon is read, so nothing past it is judged.
static bool field_line_shaped(const char *line)
{
    if (!is_token_byte(line[0]))
        return false;
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == ':')
            return true;
        if (!is_token_byte(*p))
            return false;
    }
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
