// load.c — the I/O half of a browsing session: an address in, a page out,
// through libfetch. Everything a face sees of it arrives as a sentence in
// the leg's status or a question through the leg's face.

#include <stdarg.h>

#include "internal.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"

// A load's sentences are its leg's, never the session's: the leg may be on
// another thread than the session (way.h § THREADS).
__attribute__((format(printf, 2, 3))) static void leg_say(way_leg_t *s, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(s->status, sizeof(s->status), fmt, args);
    va_end(args);
}

// ── What libfetch asks us ───────────────────────────────────────────────

static bool fetch_cancelled(void *ctx)
{
    way_leg_t *s = ctx;
    return s->face.cancelled != NULL && s->face.cancelled(s->face.ctx);
}

// A DOWNGRADE IS A PERSON'S DECISION, which is the whole reason libfetch
// takes a callback: a script may not follow https into http, and somebody at
// a keyboard may. Every other hop takes the library's own verdict.
static os64_fetch_verdict_t hop_ask(void *ctx, const os64_fetch_hop_t *hop)
{
    way_leg_t *s = ctx;
    if (hop->kind != OS64_FETCH_HOP_DOWNGRADE)
        return OS64_FETCH_HOP_DEFAULT;
    char question[WAY_SENTENCE_MAX];
    // A POST that the hop would REPLAY says so: what goes out in clear is
    // what somebody typed, not just an address.
    os64_snprintf(question, sizeof(question),
                  hop->to_method == OS64_FETCH_METHOD_POST
                      ? " Resend form data unencrypted to %s?"
                      : " %s sends you to unencrypted http - follow?",
                  hop->target.host);
    bool yes = s->face.confirm(s->face.ctx, question, true, " stopped at the unencrypted hop");
    if (yes)
        leg_say(s, " following an unencrypted hop");
    return yes ? OS64_FETCH_HOP_FOLLOW : OS64_FETCH_HOP_STOP;
}

// Says something and has the face show it now.
static void show(way_leg_t *s)
{
    if (s->face.progress != NULL)
        s->face.progress(s->face.ctx, s->status);
}

// ── Reading a body ──────────────────────────────────────────────────────

// A media type from libfetch is already lowercased, so these compare
// verbatim.
static bool type_is_text(const char *type)
{
    return type[0] == 't' && type[1] == 'e' && type[2] == 'x' && type[3] == 't'
           && type[4] == '/';
}

// Read the body into the tree. Feeding stops at the parser's first refusal —
// which still leaves a document, because a page that was too large or too
// deep is a page you can read the beginning of.
static os64_html_document_t *parse_body(way_leg_t *s, os64_fetch_t *f, const char *charset,
                                        const char *url)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = charset && charset[0] ? charset : NULL;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (!p)
        return NULL;
    char buf[8192];
    uint64_t shown = 0;
    for (;;) {
        int64_t n = os64_fetch_read(f, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (os64_html_parser_feed(p, buf, (size_t)n) != OS64_HTML_OK)
            break;
        const os64_fetch_progress_t *progress = os64_fetch_progress(f);
        if (progress->produced >= shown + 64u * 1024u) {
            shown = progress->produced;
            leg_say(s, " reading %lu KB of %s", (unsigned long)(shown / 1024), url);
            show(s);
        }
    }
    return os64_html_parser_finish(p);
}

// Read the body as bytes. The cap is the parser's, because a page is a page
// whichever way it is written.
static char *read_body(way_leg_t *s, os64_fetch_t *f, size_t cap, size_t *len,
                       const char *url, bool *whole)
{
    size_t at = 0, size = 16u * 1024u;
    *len = 0;
    *whole = true;
    char *text = os64_malloc(size);
    if (!text)
        return NULL;
    uint64_t shown = 0;
    for (;;) {
        if (at == size) {
            if (size >= cap) {
                // ASK FOR THE BYTE THAT WOULD CROSS THE CAP. libfetch refuses
                // a body at `max_body` on the read that would pass it, not
                // before, so stopping at a full buffer leaves the fetch
                // reporting OK and this page claiming to be whole when it is
                // the first 8 MB of something longer.
                char probe;
                (void)os64_fetch_read(f, &probe, 1);
                break;
            }
            size_t want = size * 2 > cap ? cap : size * 2;
            char *grown = os64_realloc(text, want);
            if (!grown) {
                // OUR failure, not the server's: libfetch will report OK
                // because no read of ours ever failed, so the truncation has
                // to be carried out of here by hand.
                *whole = false;
                break;
            }
            text = grown;
            size = want;
        }
        int64_t n = os64_fetch_read(f, text + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
        if (at >= shown + 64u * 1024u) {
            shown = at;
            leg_say(s, " reading %lu KB of %s", (unsigned long)(shown / 1024), url);
            show(s);
        }
    }
    *len = at;
    return text;
}

// What the reply's own charset label says about a text/plain body. UTF-8 is
// the modern answer, and everything else is read as windows-1252 — which is
// what libhtml does with the markup half of the same web, so a smart quote
// cannot draw as `"` in a page and as `?` in the text file beside it. The
// label is asked of libhtml's own table, so every spelling of UTF-8 the
// markup half knows (`unicode-1-1-utf-8` among them) is known here too. A
// server that says nothing about a .txt file written in 1994 is not talking
// about UTF-8. A label naming an encoding in neither family is booked in
// BROWSER.md; it reads as windows-1252 rather than as a refusal.
//
// BUT A FILE MAY SAY IT ITSELF. The three bytes `EF BB BF` are a UTF-8 byte
// order mark, put there to be read by whatever opens the file, and a server
// that mentioned no charset has not contradicted them. Taken as
// windows-1252 they draw as `ï»¿` and every accented character after them
// breaks into pieces.
static bool charset_is_utf8(const char *charset)
{
    const char *named = os64_html_encoding_for_label(charset, os64_strlen(charset));
    return named != NULL && os64_streq(named, "utf-8");
}

static bool bytes_begin_utf8(const char *text, size_t len)
{
    return text && len >= 3 && (unsigned char)text[0] == 0xEF
           && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF;
}

// ── Loading ─────────────────────────────────────────────────────────────

bool way_load(way_leg_t *s, const char *url, const os64_page_request_t *request,
              way_page_t *out, os64_fetch_status_t *why)
{
    bool short_of_memory = false;        // a truncation of OURS, not the wire's
    if (why)
        *why = OS64_FETCH_OK;
    leg_say(s, " fetching %s", url);
    show(s);

    os64_html_options_t limits = os64_html_options_default();
    os64_fetch_options_t opt = {0};
    opt.user_agent = s->session->agent;
    opt.accept = s->session->accept;
    opt.max_body = limits.max_bytes;     // the same page, the same cap
    opt.cancelled = fetch_cancelled;
    opt.on_hop = hop_ask;
    opt.ctx = s;
    if (request != NULL && request->method == OS64_PAGE_METHOD_POST) {
        opt.method = OS64_FETCH_METHOD_POST;
        opt.body = request->body;
        opt.body_len = request->body_len;
        opt.content_type = request->content_type;
    }

    os64_fetch_t *f = os64_fetch_open(url, &opt);
    if (!f) {
        leg_say(s, " out of memory fetching %s", url);
        return false;
    }
    const os64_fetch_head_t *head = os64_fetch_head(f);
    if (!head) {
        if (why)
            *why = os64_fetch_status(f);
        leg_say(s, " %s", os64_fetch_reason(f));
        os64_fetch_close(f);
        return false;
    }

    // A 404 IS A PAGE AND IS SHOWN AS ONE. The status goes in the note, not
    // in a refusal: servers say a great deal in the body of a reply a
    // downloader would throw away.
    const char *type = head->content_type;
    bool html = type[0] == '\0' || os64_streq(type, "text/html")
                || os64_streq(type, "application/xhtml+xml");
    bool plain = !html && type_is_text(type);
    if (!html && !plain) {
        // Not a page at all. Say what it is and how to keep it — unless the
        // reply was to a POST, which a new GET of the address cannot fetch
        // again. The FINAL method decides: a redirect may have turned it.
        if (head->method == OS64_FETCH_METHOD_POST)
            leg_say(s, " that is %s - %s cannot display or save this POST response",
                    type, s->session->name);
        else if (way_shell_quotable(head->url_text))
            leg_say(s, " that is %s, not a page - save it with:  os64get '%s'",
                    type, head->url_text);
        else
            leg_say(s, " that is %s, not a page - and its address holds a quote,"
                       " so save it by hand", type);
        os64_fetch_close(f);
        return false;
    }

    os64_strcopy(out->url, sizeof(out->url), head->url_text);

    if (html) {
        out->doc = parse_body(s, f, head->charset, url);
        if (!out->doc) {
            leg_say(s, " out of memory reading %s", url);
            os64_fetch_close(f);
            return false;
        }
        // WHAT THE PAGE MEANS, from the tree and the address it came from —
        // `<base href>` included, which libpage reads. A page it cannot build
        // a model of is still a page worth reading, so this is not a failure
        // to return: what it costs is the links and boxes, and the face says
        // so.
        out->model = os64_page_build(out->doc, head->url_text, NULL);
    } else {
        bool whole = true;
        out->text = read_body(s, f, limits.max_bytes, &out->textlen, url, &whole);
        if (!whole)
            short_of_memory = true;
        out->text_utf8 = head->charset[0] != '\0'
                             ? charset_is_utf8(head->charset)
                             : bytes_begin_utf8(out->text, out->textlen);
        if (!out->text) {
            leg_say(s, " out of memory reading %s", url);
            os64_fetch_close(f);
            return false;
        }
    }

    // EVERY WAY A PAGE CAN BE INCOMPLETE GETS A SENTENCE, because half a
    // page that says so is worth reading and half a page that pretends to be
    // whole is not.
    char trouble[WAY_SENTENCE_MAX];
    trouble[0] = '\0';
    os64_fetch_status_t st = os64_fetch_status(f);
    if (why)
        *why = st;
    if (short_of_memory)
        os64_strcopy(trouble, sizeof(trouble),
                     " - this machine ran out of memory partway, so the page"
                     " stops where it does");
    else if (st != OS64_FETCH_OK)
        os64_snprintf(trouble, sizeof(trouble), " - %s", os64_fetch_reason(f));
    else if (out->doc && out->doc->refusal)
        os64_snprintf(trouble, sizeof(trouble), " - the page is bigger than this"
                      " browser will parse (%s)",
                      os64_html_status_name(out->doc->refusal));
    os64_snprintf(out->note, sizeof(out->note), "%ld%s%s%s", (long)head->status,
                  head->reason[0] ? " " : "", head->reason, trouble);
    os64_fetch_close(f);
    return true;
}
