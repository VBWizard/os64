// wire.c — RFC 959's control channel, as text.
//
// No syscall in this file. See wire.h for what each piece is for and which
// trap it exists to disarm.

#include "wire.h"

#include "os64/str.h"

// ── The control channel ─────────────────────────────────────────────────

void ftp_control_init(ftp_control_t *c, ftp_source_fn source, void *ctx)
{
    os64_memset(c, 0, sizeof(*c));
    c->source = source;
    c->ctx = ctx;
}

bool ftp_control_drained(const ftp_control_t *c)
{
    return c->pos >= c->len && (c->eof || c->failed);
}

// 1 = a byte is buffered, 0 = the source is finished, -1 = it failed,
// FILL_STALLED = its deadline expired and the channel is still good.
#define FILL_STALLED (-2)

static int control_fill(ftp_control_t *c)
{
    if (c->pos < c->len)
        return 1;
    if (c->failed)
        return -1;
    if (c->eof)
        return 0;

    int64_t n = c->source(c->ctx, c->buf, sizeof(c->buf));
    if (n == FTP_SOURCE_STALLED) return FILL_STALLED;   // `failed` stays clear
    if (n < 0) { c->failed = true; return -1; }
    if (n == 0) { c->eof = true; return 0; }

    c->pos = 0;
    c->len = (size_t)n;
    return 1;
}

// One line into `out`, terminator stripped, always NUL-terminated.
//
// Returns the number of bytes CONSUMED from the channel (>0), 0 at a clean
// end, -1 on a source error, or LINE_STALLED when the deadline expired with
// NOTHING of this line taken. The consumed count is what the caller charges
// against FTP_REPLY_CONSUME_MAX, so an over-long line's discarded tail counts
// against the ceiling exactly like the part that was kept — otherwise a peer
// sending one endless line is bounded by nothing.
//
// A stall PART WAY through a line is an error and not a stall: those bytes are
// gone from the channel and the line they belonged to can never be completed.
//
// A line with no terminator at the close of the channel is still a line: a
// server that says "221 Goodbye" and shuts down in the same breath has
// answered, and refusing that answer would turn every polite disconnection
// into an error.
#define LINE_STALLED ((int64_t)-2)

static int64_t control_line(ftp_control_t *c, char *out, size_t cap,
                            bool *overlong)
{
    size_t have = 0;
    int64_t consumed = 0;
    *overlong = false;

    for (;;) {
        int rc = control_fill(c);
        if (rc == FILL_STALLED) {
            if (consumed > 0) {
                c->failed = true;
                return -1;
            }
            out[0] = '\0';
            return LINE_STALLED;
        }
        if (rc < 0)
            return -1;
        if (rc == 0)
            break;                      // source finished

        char ch = c->buf[c->pos++];
        consumed++;

        if (ch == '\n')
            break;
        if (have + 1 < cap)
            out[have++] = ch;
        else
            *overlong = true;
    }

    // The CR of a CRLF, and only there. A lone CR mid-line is somebody's
    // byte and stays one.
    if (have > 0 && out[have - 1] == '\r')
        have--;

    out[have] = '\0';
    return consumed;
}

// ── A reply ─────────────────────────────────────────────────────────────

// `NNN` followed by a space or a hyphen, at the head of a line. Returns the
// code, or 0 when the line does not open one. `*more` says the hyphen form.
static int reply_line_code(const char *line, bool *more)
{
    if (line[0] < '0' || line[0] > '9' ||
        line[1] < '0' || line[1] > '9' ||
        line[2] < '0' || line[2] > '9')
        return 0;

    if (line[3] == '-')
        *more = true;
    else if (line[3] == ' ' || line[3] == '\0')
        *more = false;      // a bare `220` with nothing after it is still a close
    else
        return 0;

    int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    return (code >= 100 && code < 600) ? code : 0;
}

// Append one line of reply text, joined by '\n'. Silent past the cap: the
// reply's `truncated` flag is the report, and the READ still runs to the
// closing line so the channel stays synchronised.
static void reply_append(ftp_reply_t *r, const char *line)
{
    size_t want = os64_strlen(line);
    size_t room = (r->len < sizeof(r->text)) ? sizeof(r->text) - r->len - 1 : 0;

    if (r->len > 0) {
        if (room == 0) { r->truncated = true; return; }
        r->text[r->len++] = '\n';
        room--;
    }
    if (want > room) {
        want = room;
        r->truncated = true;
    }
    os64_memcpy(r->text + r->len, line, want);
    r->len += want;
    r->text[r->len] = '\0';
}

ftp_reply_result_t ftp_reply_read(ftp_control_t *c, ftp_reply_t *out)
{
    char line[FTP_LINE_MAX];
    bool overlong = false;
    uint64_t consumed = 0;

    os64_memset(out, 0, sizeof(*out));

    int64_t n = control_line(c, line, sizeof(line), &overlong);
    if (n == LINE_STALLED)
        return FTP_REPLY_STALLED;   // nothing taken; the channel is still good
    if (n < 0)
        return FTP_REPLY_FAILED;
    if (n == 0)
        return FTP_REPLY_END;
    consumed += (uint64_t)n;

    bool more = false;
    int code = reply_line_code(line, &more);
    if (code == 0)
        return FTP_REPLY_MALFORMED;

    out->code = code;
    out->lines = 1;
    out->truncated = overlong;
    reply_append(out, line);

    while (more) {
        if (consumed >= FTP_REPLY_CONSUME_MAX)
            return FTP_REPLY_TOO_LONG;

        n = control_line(c, line, sizeof(line), &overlong);
        if (n == LINE_STALLED) {
            // The opening line is already consumed, so this reply can never be
            // completed and the next read would begin inside it. Mark the
            // channel so nothing tries.
            c->failed = true;
            return FTP_REPLY_FAILED;
        }
        if (n < 0)
            return FTP_REPLY_FAILED;
        if (n == 0)
            return FTP_REPLY_END;      // the close never came
        consumed += (uint64_t)n;

        out->lines++;
        if (overlong)
            out->truncated = true;

        // ONLY THE OPENING CODE CLOSES THE REPLY. Any other code, and any
        // line that is not a code at all, is text — including one that would
        // pass for a reply on its own.
        bool lineMore = false;
        int lineCode = reply_line_code(line, &lineMore);
        if (lineCode == code && !lineMore)
            more = false;
        reply_append(out, line);
    }

    return FTP_REPLY_OK;
}

const char *ftp_reply_reason(ftp_reply_result_t rc)
{
    switch (rc) {
        case FTP_REPLY_OK:        return "ok";
        case FTP_REPLY_END:       return "the server closed the control connection";
        case FTP_REPLY_FAILED:    return "the control connection failed";
        case FTP_REPLY_MALFORMED: return "the server sent something that is not a reply";
        case FTP_REPLY_TOO_LONG:  return "the server's reply never ended";
        case FTP_REPLY_STALLED:   return "the server stopped answering";
    }
    return "the control connection failed";
}

// ── Building a command ──────────────────────────────────────────────────

ftp_command_result_t ftp_command(const char *verb, const char *arg,
                                 char *out, size_t cap, size_t *written)
{
    size_t vlen = os64_strlen(verb);
    size_t alen = arg ? os64_strlen(arg) : 0;

    for (size_t i = 0; i < alen; i++) {
        if (arg[i] == '\r' || arg[i] == '\n')
            return FTP_COMMAND_NEWLINE;
    }
    // verb + (space + arg) + CRLF + NUL
    size_t want = vlen + (alen ? 1 + alen : 0) + 2;
    if (want + 1 > cap)
        return FTP_COMMAND_TOO_LONG;

    size_t at = 0;
    os64_memcpy(out + at, verb, vlen);        at += vlen;
    if (alen) {
        out[at++] = ' ';
        os64_memcpy(out + at, arg, alen);     at += alen;
    }
    out[at++] = '\r';
    out[at++] = '\n';
    out[at] = '\0';

    if (written)
        *written = at;
    return FTP_COMMAND_OK;
}

const char *ftp_command_reason(ftp_command_result_t rc)
{
    switch (rc) {
        case FTP_COMMAND_OK:       return "ok";
        case FTP_COMMAND_TOO_LONG: return "the name is longer than a command can carry";
        case FTP_COMMAND_NEWLINE:  return "the name contains a line ending, which FTP cannot carry";
    }
    return "the command could not be built";
}

// ── The 227 reply ───────────────────────────────────────────────────────

// Read the WHOLE comma-separated number list starting at `s` and return the
// byte after it. `*count` is how many numbers it held and `*range` says one of
// them will not fit a byte; the first six values land in `v`.
//
// The list is taken whole rather than six-at-a-time on purpose. Stopping at
// six would let `10,0,2,2,195,80,7` match its own TAIL — the scan resumes
// inside the list and `0,2,2,195,80,7` is six numbers — and dial a different
// machine than the one the server named. A list that is not exactly six long
// is not an address, and the caller refuses it rather than guessing which six
// were meant.
static const char *number_list(const char *s, unsigned *v, int *count,
                               bool *range)
{
    *count = 0;
    *range = false;

    for (;;) {
        if (*s < '0' || *s > '9')
            break;

        unsigned value = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            // Bounded by the digit count, so a run of four hundred digits
            // cannot overflow on its way to being rejected.
            if (digits < 9)
                value = value * 10 + (unsigned)(*s - '0');
            digits++;
            s++;
        }
        if (digits > 3 || value > 255)
            *range = true;
        else if (*count < 6)
            v[*count] = value;
        (*count)++;

        if (*s != ',' || s[1] < '0' || s[1] > '9')
            break;
        s++;
    }
    return s;
}

ftp_pasv_result_t ftp_pasv_parse(const char *text, uint32_t *ip, uint16_t *port)
{
    unsigned best[6];
    bool found = false;
    bool parenthesized = false;
    bool range = false;

    for (const char *s = text; *s; s++) {
        // Start only where a number starts, so the scan cannot begin in the
        // middle of one and read `0,2,2,195,80,x` out of a longer address.
        if (*s < '0' || *s > '9')
            continue;
        if (s > text && s[-1] >= '0' && s[-1] <= '9')
            continue;

        unsigned v[6];
        int count = 0;
        bool listRange = false;
        const char *end = number_list(s, v, &count, &listRange);

        if (count == 6 && !listRange) {
            // A list that opens right after '(' is the spelled form and wins
            // over any prose either side of it. Otherwise the FIRST list
            // stands, which is what ftplib and curl do — matching them is what
            // lets the host harness diff against a reference rather than
            // against an opinion.
            bool paren = (s > text && s[-1] == '(');
            if (!found || (paren && !parenthesized)) {
                for (int i = 0; i < 6; i++)
                    best[i] = v[i];
                found = true;
                parenthesized = paren;
            }
            if (parenthesized)
                break;
        } else if (count == 6) {
            range = true;
        }

        s = end - 1;    // resume past the whole list; the loop's ++ lands on `end`
    }

    if (!found)
        return range ? FTP_PASV_RANGE : FTP_PASV_NO_TUPLE;

    uint16_t p = (uint16_t)((best[4] << 8) | best[5]);
    if (p == 0)
        return FTP_PASV_PORT_ZERO;

    *ip = (best[0] << 24) | (best[1] << 16) | (best[2] << 8) | best[3];
    *port = p;
    return FTP_PASV_OK;
}

const char *ftp_pasv_reason(ftp_pasv_result_t rc)
{
    switch (rc) {
        case FTP_PASV_OK:        return "ok";
        case FTP_PASV_NO_TUPLE:  return "the server's passive-mode reply names no address";
        case FTP_PASV_RANGE:     return "the server's passive-mode address is out of range";
        case FTP_PASV_PORT_ZERO: return "the server's passive-mode reply names port 0";
    }
    return "the server's passive-mode reply could not be read";
}

// ── The 257 reply ───────────────────────────────────────────────────────

bool ftp_path_from_257(const char *text, char *out, size_t cap)
{
    const char *s = text;

    while (*s && *s != '"')
        s++;
    if (*s != '"')
        return false;
    s++;

    size_t have = 0;
    for (;;) {
        if (*s == '\0')
            return false;           // the closing quote never came
        if (*s == '"') {
            // A doubled quote is one literal quote INSIDE the path; a single
            // one ends it. Reading that wrong reports the wrong directory to
            // the person standing in it.
            if (s[1] != '"')
                break;
            s++;
        }
        if (have + 1 < cap)
            out[have++] = *s;
        s++;
    }

    out[have] = '\0';
    return true;
}
