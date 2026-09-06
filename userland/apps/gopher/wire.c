// wire.c — RFC 1436 on the wire. See wire.h for what is here and why the
// grammar is not.

#include "wire.h"

#include "os64/fmt.h"
#include "os64/str.h"

// ── Bytes ───────────────────────────────────────────────────────────────

// WHAT MAY REACH THE SCREEN. A byte a terminal OBEYS is refused; a byte it
// merely draws is kept. High bytes are kept on purpose: a menu written in
// Latin-1 by somebody in 1994 is not an attack, and what a byte over 0x7F
// looks like is the font's business.
static bool is_printable(char c)
{
    return (unsigned char)c >= 0x20 && (unsigned char)c != 0x7F;
}

static bool span_is_printable(const char *s)
{
    for (; *s != '\0'; s++)
        if (!is_printable(*s))
            return false;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool needs_escape(char c)
{
    unsigned char b = (unsigned char)c;
    // Everything the shared parser would refuse in a raw path, plus the two
    // bytes that mean something to a URL reader. Encoding all of these is
    // what makes gopher_url_text's output parse back to the same address —
    // a displayed location a person cannot retype is a decoration.
    return b <= 0x20 || b >= 0x7F || c == '%' || c == '#';
}

// ── The type character ──────────────────────────────────────────────────

gopher_framing_t gopher_framing_for(char type)
{
    switch (type) {
        case '1': case '7': return GOPHER_FRAMING_MENU;
        case '0':           return GOPHER_FRAMING_TEXT;
        default:            return GOPHER_FRAMING_BINARY;
    }
}

bool gopher_type_followable(char type)
{
    switch (type) {
        // Fetched over gopher: a menu, a text file, or bytes for a file.
        case '0': case '1': case '4': case '5': case '6': case '7':
        case '9': case 'd': case 'g': case 'I': case 's':
            return true;
        // Followed, but not over gopher: an `h` item's selector is a
        // `URL:http://…` that os64get is handed.
        case 'h':
            return true;
        default:
            // `i` says something, `3` is the server complaining, `8` and `T`
            // are terminal sessions, `2` is a phone book, `+` is a mirror of
            // a resource whose real type nobody stated — and everything
            // unknown, because guessing what a type means is how you
            // download a thing that is not what it said it was.
            return false;
    }
}

// A TYPE'S NAME FITS THE COLUMN IT IS DRAWN IN. Seven characters, because
// that is what /bin/gopher's label column holds — a longer one is clipped by
// the renderer and reads as a typo ("phone book" arrived on a real Floodgap
// menu and was drawn as "phone b"). Short and true beats long and cut.
const char *gopher_type_name(char type)
{
    switch (type) {
        case '0': return "text";
        case '1': return "menu";
        case '2': return "phone";
        case '3': return "error";
        case '4': return "binhex";
        case '5': return "archive";
        case '6': return "uucode";
        case '7': return "search";
        case '8': return "telnet";
        case '9': return "binary";
        case '+': return "mirror";
        case 'T': return "tn3270";
        case 'd': return "doc";
        case 'g': return "gif";
        case 'h': return "web";
        case 'i': return "";
        case 's': return "sound";
        case 'I': return "image";
        default:  return "unknown";
    }
}

// ── An address ──────────────────────────────────────────────────────────

static gopher_url_result_t url_result_from(os64_url_result_t rc)
{
    switch (rc) {
        case OS64_URL_OK:         return GOPHER_URL_OK;
        case OS64_URL_NO_HOST:    return GOPHER_URL_NO_HOST;
        case OS64_URL_HOST_CHARS: return GOPHER_URL_HOST_CHARS;
        case OS64_URL_PATH_CHARS: return GOPHER_URL_PATH_CHARS;
        case OS64_URL_PORT:       return GOPHER_URL_PORT;
        case OS64_URL_TOO_LONG:   return GOPHER_URL_TOO_LONG;
        case OS64_URL_NOT_A_URL:  return GOPHER_URL_SCHEME;
    }
    return GOPHER_URL_SCHEME;
}

// Decode `%XX` into `out`. Returns false for a '%' that is not followed by
// two hex digits — refused rather than passed through, because "%zz" in a
// selector is somebody's mistake and sending it on would ask the server a
// question the person did not.
//
// AND FALSE FOR `%00`, WHICH HAS TO BE REFUSED HERE OR NOWHERE. Everything
// downstream of this function is a C string operation, so a decoded NUL is
// the one byte no later check can see: it ends the selector early, and
// `gopher://host/1/a%00b` would quietly fetch `a`. The control-byte sweep
// below cannot catch it because the sweep stops AT it. (Found by the host
// suite, 2026-09-04 — it read `ok` where it should have read a refusal.)
static gopher_url_result_t percent_decode(const char *src, char *out, size_t cap)
{
    size_t n = 0;
    for (; *src != '\0'; src++) {
        if (n + 1 >= cap)
            return GOPHER_URL_TOO_LONG;
        if (*src == '%') {
            int hi = hex_value(src[1]);
            int lo = (hi >= 0) ? hex_value(src[2]) : -1;
            if (hi < 0 || lo < 0)
                return GOPHER_URL_ESCAPE;
            char decoded = (char)((hi << 4) | lo);
            if (decoded == '\0')
                return GOPHER_URL_PATH_CHARS;   // it IS a control character
            out[n++] = decoded;
            src += 2;
        } else {
            out[n++] = *src;
        }
    }
    out[n] = '\0';
    return GOPHER_URL_OK;
}

gopher_url_result_t gopher_url_parse(const char *text, gopher_addr_t *out)
{
    os64_memset(out, 0, sizeof(*out));

    os64_url_t parsed;
    os64_url_result_t rc = os64_url_parse(text, &parsed);

    // A BARE `host[:port]` IS A GOPHER HOST. The library says only "no
    // scheme"; what that MEANS is this program's to decide, and gopher is
    // the one protocol whose addresses people still type without one.
    // Spelling the scheme in and re-parsing keeps every host and port rule
    // in one place rather than growing a second, laxer path for bare names.
    char spelled[OS64_URL_HOST_MAX + OS64_URL_PATH_MAX + 16];
    if (rc == OS64_URL_NOT_A_URL) {
        if (os64_snprintf(spelled, sizeof(spelled), "gopher://%s", text) <= 0)
            return GOPHER_URL_TOO_LONG;
        rc = os64_url_parse(spelled, &parsed);
        if (rc == OS64_URL_NOT_A_URL)
            return GOPHER_URL_SCHEME;
    } else if (rc == OS64_URL_OK && !os64_streq(parsed.scheme, "gopher")) {
        return GOPHER_URL_SCHEME;
    }
    if (rc != OS64_URL_OK)
        return url_result_from(rc);

    os64_strcopy(out->host, sizeof(out->host), parsed.host);
    out->port = parsed.port != 0 ? parsed.port : 70;

    // The path is raw, and decoding it is this program's job and this
    // program's duty: `%00` and `%1B` are control bytes that were not in the
    // text the library judged, so everything below is checked again.
    char decoded[GOPHER_SELECTOR_MAX + GOPHER_QUERY_MAX + 4];
    gopher_url_result_t decodeRc = percent_decode(parsed.path, decoded, sizeof(decoded));
    if (decodeRc != GOPHER_URL_OK)
        return decodeRc;

    // `decoded[0]` is the '/' the library normalises every path to start
    // with, so `decoded[1]` is the type and the selector follows it. An
    // empty path is the root menu, which is RFC 4266's own reading.
    char type = decoded[1] != '\0' ? decoded[1] : '1';
    if (!is_printable(type))
        return GOPHER_URL_PATH_CHARS;
    out->type = type;

    const char *rest = decoded[1] != '\0' ? decoded + 2 : "";

    // A TAB separates a search query from its selector (RFC 4266 §2.2). A
    // '?' does not: it is an ordinary byte that some server is using.
    const char *tab = NULL;
    for (const char *p = rest; *p != '\0'; p++)
        if (*p == '\t') { tab = p; break; }

    size_t selectorLen = tab != NULL ? (size_t)(tab - rest) : os64_strlen(rest);
    if (selectorLen >= sizeof(out->selector))
        return GOPHER_URL_TOO_LONG;
    for (size_t i = 0; i < selectorLen; i++) {
        if (!is_printable(rest[i]))
            return GOPHER_URL_PATH_CHARS;
        out->selector[i] = rest[i];
    }
    out->selector[selectorLen] = '\0';

    if (tab != NULL) {
        if (os64_strcopy(out->query, sizeof(out->query), tab + 1) >= sizeof(out->query))
            return GOPHER_URL_TOO_LONG;
        if (!span_is_printable(out->query))
            return GOPHER_URL_PATH_CHARS;
    }
    return GOPHER_URL_OK;
}

const char *gopher_url_reason(gopher_url_result_t rc)
{
    switch (rc) {
        case GOPHER_URL_OK:         return "ok";
        case GOPHER_URL_SCHEME:     return "not a gopher address";
        case GOPHER_URL_NO_HOST:    return "the address names no host";
        case GOPHER_URL_HOST_CHARS: return "the host holds something a host name cannot"
                                           " (a user@, an IPv6 literal in brackets, or junk)";
        case GOPHER_URL_PATH_CHARS: return "the selector holds a control character";
        case GOPHER_URL_ESCAPE:     return "a '%' is not followed by two hex digits";
        case GOPHER_URL_PORT:       return "the port is not a number from 1 to 65535";
        case GOPHER_URL_TOO_LONG:   return "the address is too long";
    }
    return "the address cannot be read";
}

size_t gopher_url_text(const gopher_addr_t *addr, char *out, size_t cap)
{
    // Built by hand rather than with snprintf because the selector is
    // escaped byte by byte, and the WANTED length is reported whether it fit
    // or not (os64_strcopy's contract) so a caller can tell.
    size_t n = 0;
    const char *scheme = "gopher://";

    #define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (const char *p = scheme; *p != '\0'; p++) PUT(*p);
    for (const char *p = addr->host; *p != '\0'; p++) PUT(*p);
    if (addr->port != 70) {
        char port[8];
        os64_snprintf(port, sizeof(port), ":%u", (unsigned)addr->port);
        for (const char *p = port; *p != '\0'; p++) PUT(*p);
    }
    PUT('/');
    PUT(addr->type);
    for (const char *p = addr->selector; *p != '\0'; p++) {
        if (needs_escape(*p)) {
            static const char digits[] = "0123456789ABCDEF";
            PUT('%'); PUT(digits[((unsigned char)*p) >> 4]); PUT(digits[*p & 0x0F]);
        } else {
            PUT(*p);
        }
    }
    if (addr->query[0] != '\0') {
        PUT('%'); PUT('0'); PUT('9');       // the TAB, spelled so it survives
        for (const char *p = addr->query; *p != '\0'; p++) {
            if (needs_escape(*p)) {
                static const char digits[] = "0123456789ABCDEF";
                PUT('%'); PUT(digits[((unsigned char)*p) >> 4]); PUT(digits[*p & 0x0F]);
            } else {
                PUT(*p);
            }
        }
    }
    #undef PUT

    if (cap != 0)
        out[n < cap ? n : cap - 1] = '\0';
    return n;
}

size_t gopher_request(const gopher_addr_t *addr, char *out, size_t cap)
{
    // CRLF and not a bare LF: a 1991 protocol means it, and a server written
    // to the letter of the RFC is entitled to wait for the second byte.
    int32_t n = addr->query[0] != '\0'
        ? os64_snprintf(out, cap, "%s\t%s\r\n", addr->selector, addr->query)
        : os64_snprintf(out, cap, "%s\r\n", addr->selector);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// ── An item line ────────────────────────────────────────────────────────

gopher_item_result_t gopher_item_parse(const char *line, gopher_item_t *out)
{
    os64_memset(out, 0, sizeof(*out));

    // A line with no type character at all cannot say what it is.
    if (line[0] == '\0')
        return (out->result = GOPHER_ITEM_MALFORMED);

    out->type = line[0];
    const char *rest = line + 1;

    // Split on tabs. A menu line is four fields; a fifth (Gopher+ marks its
    // items with a trailing '+') is ignored rather than refused, because
    // ignoring it is what lets a Gopher+ server's ordinary items be read.
    const char *field[4] = { rest, NULL, NULL, NULL };
    size_t len[4] = { 0, 0, 0, 0 };
    int count = 1;
    for (const char *p = rest; ; p++) {
        if (*p == '\t' || *p == '\0') {
            len[count - 1] = (size_t)(p - field[count - 1]);
            if (*p == '\0' || count == 4)
                break;
            field[count] = p + 1;
            count++;
        }
    }

    if (len[0] >= sizeof(out->display))
        len[0] = sizeof(out->display) - 1;
    for (size_t i = 0; i < len[0]; i++) {
        // A DISPLAY NAME IS A STRANGER'S BYTES ON A TERMINAL THAT OBEYS
        // ESCAPES. Refused here, once, where the line is parsed — an
        // arrangement that escapes at each print is one that eventually
        // misses a print.
        if (!is_printable(field[0][i]))
            return (out->result = GOPHER_ITEM_REFUSED);
        out->display[i] = field[0][i];
    }

    // An `i` line is text in a menu, not a link. Its selector, host and port
    // are conventionally junk (`fake`/`(NULL)`/`0`), so they are not read at
    // all — validating them would refuse most of gopherspace.
    if (out->type == 'i')
        return (out->result = GOPHER_ITEM_INFO);

    if (count < 4)
        return (out->result = GOPHER_ITEM_MALFORMED);

    if (len[1] >= sizeof(out->addr.selector) || len[2] >= sizeof(out->addr.host))
        return (out->result = GOPHER_ITEM_MALFORMED);
    for (size_t i = 0; i < len[1]; i++) {
        if (!is_printable(field[1][i]))
            return (out->result = GOPHER_ITEM_REFUSED);
        out->addr.selector[i] = field[1][i];
    }
    for (size_t i = 0; i < len[2]; i++) {
        if (!is_printable(field[2][i]))
            return (out->result = GOPHER_ITEM_REFUSED);
        out->addr.host[i] = field[2][i];
    }

    char port[8];
    if (len[3] == 0 || len[3] >= sizeof(port))
        return (out->result = GOPHER_ITEM_MALFORMED);
    for (size_t i = 0; i < len[3]; i++)
        port[i] = field[3][i];
    port[len[3]] = '\0';

    // A PORT IS DECIMAL DIGITS, and saying so here is not a second copy of
    // the host alphabet: it is what makes the round trip below meaningful.
    // `70#junk` parses to port 70 because the '#' starts a fragment, and a
    // port field that was never a number must not be read as one.
    for (size_t i = 0; i < len[3]; i++)
        if (port[i] < '0' || port[i] > '9')
            return (out->result = GOPHER_ITEM_MALFORMED);

    // THE HOST AND PORT ARE JUDGED BY THE SHARED GRAMMAR, by spelling them
    // as the address they are and handing it to os64_url_parse. A private
    // host-alphabet check here would be the second copy of exactly the rule
    // that was hoisted into libos64 to stop there being two.
    char spelled[OS64_URL_HOST_MAX + 32];
    if (os64_snprintf(spelled, sizeof(spelled), "gopher://%s:%s/",
                      out->addr.host, port) <= 0)
        return (out->result = GOPHER_ITEM_MALFORMED);
    os64_url_t checked;
    if (os64_url_parse(spelled, &checked) != OS64_URL_OK)
        return (out->result = GOPHER_ITEM_MALFORMED);

    // AND THE ANSWER MUST BE THE QUESTION. Interpolating a field into a URL
    // lets that field rewrite the URL's shape: `evil.com/path` spells an
    // authority of `evil.com` with a path after it, and the parse SUCCEEDS
    // on an address the menu line never named. Compare what came back with
    // what went in — without regard to case, because the parser lowercases a
    // host and a menu may not have.
    if (!os64_streq_nocase(checked.host, out->addr.host))
        return (out->result = GOPHER_ITEM_MALFORMED);

    os64_strcopy(out->addr.host, sizeof(out->addr.host), checked.host);
    out->addr.port = checked.port != 0 ? checked.port : 70;
    out->addr.type = out->type;
    out->followable = gopher_type_followable(out->type);
    return (out->result = GOPHER_ITEM_OK);
}

// ── Reading an answer ───────────────────────────────────────────────────

void gopher_stream_init(gopher_stream_t *s, gopher_source_fn source, void *ctx)
{
    os64_memset(s, 0, sizeof(*s));
    s->source = source;
    s->ctx = ctx;
}

// One read into the buffer. 1 = bytes available, 0 = the source is done,
// -1 = it failed.
static int stream_fill(gopher_stream_t *s)
{
    if (s->pos < s->len)
        return 1;
    if (s->eof)
        return 0;
    int64_t n = s->source(s->ctx, s->buf, sizeof(s->buf));
    if (n < 0) { s->failed = true; return -1; }
    if (n == 0) { s->eof = true; return 0; }
    s->pos = 0;
    s->len = (size_t)n;
    s->bytes += (uint64_t)n;
    return 1;
}

int gopher_stream_line(gopher_stream_t *s, char *out, size_t cap, bool unstuff)
{
    if (s->terminated || s->failed || s->ceiling)
        return s->failed ? -1 : 0;

    size_t n = 0;
    bool sawAny = false;
    bool overlong = false;

    for (;;) {
        int got = stream_fill(s);
        if (got < 0)
            return -1;
        if (got == 0)
            break;                       // the connection ended

        char c = s->buf[s->pos++];
        sawAny = true;
        if (c == '\n')
            break;
        if (n + 1 < cap)
            out[n++] = c;
        else
            overlong = true;             // keep reading to the newline

        // The ceiling is checked HERE and not between lines, because a cap
        // a single endless line never returns to be measured by is not a cap.
        // Crossing it ENDS the stream: marking only this line left `bytes`
        // over the ceiling and every later call tripping the same check one
        // byte at a time, so a dribbling peer kept the fetch alive far past
        // the bound. The partial line is still handed back — it was read.
        if (s->bytes > GOPHER_RESPONSE_MAX) {
            s->truncated = true;
            s->ceiling = true;
            out[n < cap ? n : cap - 1] = '\0';
            return n > 0 ? 1 : 0;
        }
    }

    if (n > 0 && out[n - 1] == '\r')     // CRLF, and a bare LF from a server
        n--;                             // that forgot: both end a line here
    out[n < cap ? n : cap - 1] = '\0';
    if (overlong)
        s->truncated = true;

    if (!sawAny)
        return 0;                        // nothing left; the close was the end

    if (n == 1 && out[0] == '.') {
        s->terminated = true;
        return 0;
    }

    // RFC 1436's transparency rule, undone. Only a line beginning with TWO
    // periods is unstuffed: a server that never stuffed anything then keeps
    // its lone leading period instead of losing it.
    if (unstuff && n >= 2 && out[0] == '.' && out[1] == '.') {
        for (size_t i = 0; i < n; i++)
            out[i] = out[i + 1];
    }
    return 1;
}

int64_t gopher_stream_raw(gopher_stream_t *s, void *out, size_t cap)
{
    int got = stream_fill(s);
    if (got <= 0)
        return got == 0 ? 0 : -1;

    size_t have = s->len - s->pos;
    size_t take = have < cap ? have : cap;
    os64_memcpy(out, s->buf + s->pos, take);
    s->pos += take;
    return (int64_t)take;
}
