// load.c — the I/O half of a browsing session: an address in, a page out,
// through libfetch. Everything a face sees of it arrives as a sentence in
// the leg's status or a question through the leg's face.

#include <stdarg.h>

#include "internal.h"
#include "os64/date.h"
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

// The wall clock a cookie's expiry is judged by, in UTC seconds.
static int64_t now_utc(void)
{
    os64_time_t t;
    return os64_time(&t) == 0 ? t.epoch : 0;
}

// Each hop's Cookie and Referer, from the browser's jar and the page the
// load was asked for from.
static bool leg_headers(void *ctx, const os64_url_t *hop, bool encrypted, char *out, size_t cap)
{
    way_leg_t *s = ctx;
    (void)way_hop_headers(s->session->jar, s->referrer, hop, encrypted, now_utc(), out, cap);
    return true;
}

static void leg_cookie(void *ctx, const os64_url_t *from, bool encrypted, const char *value,
                       size_t len)
{
    way_leg_t *s = ctx;
    way_jar_hear(s->session->jar, from, encrypted, value, len, now_utc());
}

// The same two for a fetch a face makes itself, whose cancellation rides
// in the hooks because libfetch has one context for all of them.
static bool hooks_cancelled(void *ctx)
{
    way_hooks_t *h = ctx;
    return h->cancelled != NULL && h->cancelled(h->cancel_ctx);
}

static bool hooks_headers(void *ctx, const os64_url_t *hop, bool encrypted, char *out, size_t cap)
{
    way_hooks_t *h = ctx;
    (void)way_hop_headers(h->jar, h->referrer, hop, encrypted, now_utc(), out, cap);
    return true;
}

static void hooks_cookie(void *ctx, const os64_url_t *from, bool encrypted, const char *value,
                         size_t len)
{
    way_hooks_t *h = ctx;
    way_jar_hear(h->jar, from, encrypted, value, len, now_utc());
}

void way_fetch_hooks(way_hooks_t *hooks, os64_fetch_options_t *opt)
{
    opt->headers_for = hooks_headers;
    opt->on_set_cookie = hooks_cookie;
    opt->cancelled = hooks_cancelled;
    opt->ctx = hooks;
}

// ── A body read whole, through the cache ────────────────────────────────

// Every hop takes the library's own verdict; what is noted is whether one
// was a redirect the server does not promise to repeat, and what a
// permanent one said about keeping it.
static os64_fetch_verdict_t hooks_hop(void *ctx, const os64_fetch_hop_t *hop)
{
    way_hooks_t *h = ctx;
    if (hop->status != 301 && hop->status != 308)
        h->passing_hop = true;
    way_chain_hop(&h->chain, &hop->keep, now_utc());
    return OS64_FETCH_HOP_DEFAULT;
}

// The body, whole, up to `cap` bytes. NULL on no memory, when the fetch
// did not deliver all of it, or when there is more than `cap`.
static uint8_t *read_whole(os64_fetch_t *f, size_t cap, size_t *len)
{
    size_t at = 0, size = 16u * 1024u < cap ? 16u * 1024u : cap;
    uint8_t *buf = os64_malloc(size > 0 ? size : 1);
    if (buf == NULL)
        return NULL;
    for (;;) {
        if (at == size) {
            if (size >= cap) {
                // ASK FOR THE BYTE THAT WOULD CROSS THE CAP: libfetch refuses
                // a body at max_body on the read that would pass it, not
                // before, and a buffer that stopped at exactly the cap would
                // take the first `cap` bytes of something longer for all of it.
                uint8_t probe;
                (void)os64_fetch_read(f, &probe, 1);
                break;
            }
            size_t want = size * 2 > cap ? cap : size * 2;
            uint8_t *grown = os64_realloc(buf, want);
            if (grown == NULL) {
                os64_free(buf);
                return NULL;
            }
            buf = grown;
            size = want;
        }
        int64_t n = os64_fetch_read(f, buf + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
    }
    if (os64_fetch_status(f) != OS64_FETCH_OK) {
        os64_free(buf);
        return NULL;
    }
    *len = at;
    return buf;
}

// A kept entry as the answer.
static bool serve(way_whole_t *out, const way_entry_t *e, uint8_t *body, size_t len,
                  way_served_t how)
{
    out->bytes = body;
    out->len = len;
    out->status = e->status;
    os64_strcopy(out->content_type, sizeof(out->content_type), e->content_type);
    os64_strcopy(out->charset, sizeof(out->charset), e->charset);
    os64_strcopy(out->url, sizeof(out->url), e->final);
    out->served = how;
    out->fetch = OS64_FETCH_OK;
    return true;
}

bool way_fetch_whole(way_hooks_t *hooks, const char *url, os64_fetch_options_t *opt,
                     size_t cap, way_whole_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    way_fetch_hooks(hooks, opt);
    opt->on_hop = hooks_hop;
    opt->max_body = cap;
    hooks->passing_hop = false;
    way_chain_start(&hooks->chain);
    // Only a plain GET is the same request every time: a caller's own
    // headers could change what comes back.
    way_cache_t *cache = hooks->cache;
    bool plain = opt->method == OS64_FETCH_METHOD_GET && opt->body == NULL &&
                 (opt->extra_headers == NULL || opt->extra_headers[0] == '\0');
    bool caching = plain && way_cache_enabled(cache);
    way_entry_t *e = caching ? os64_malloc(sizeof(*e)) : NULL;
    uint8_t *kept = NULL;
    size_t kept_len = 0;
    bool have = e != NULL && way_cache_find(cache, url, e, &kept, &kept_len, cap);
    int64_t now = now_utc();
    if (have && way_keep_fresh(&e->keep, now)) {
        serve(out, e, kept, kept_len, WAY_SERVED_KEPT);
        way_cache_count(cache, WAY_SERVED_KEPT);
        os64_free(e);
        return true;
    }
    // The question that asks whether it changed goes out on a copy: the
    // caller's options outlive this frame, and its conditions do not. A
    // validator is the kept RESOURCE's, and two resources may share one
    // (RFC 9110 § 8.8.1): it is asked only when the entry came from the
    // address asked for, since libfetch sends a request's headers on every
    // hop. One reached through redirects is fetched whole when stale.
    char conditions[OS64_FETCH_EXTRA_MAX];
    os64_fetch_options_t ask = *opt;
    bool asked = have && os64_streq(e->final, url) &&
                 way_keep_conditions(&e->keep, conditions, sizeof(conditions)) > 0;
    if (asked)
        ask.extra_headers = conditions;

    os64_fetch_t *f = os64_fetch_open(url, &ask);
    const os64_fetch_head_t *head = f != NULL ? os64_fetch_head(f) : NULL;
    // A 304 from another resource — the address redirects now — validates
    // nothing kept here: asked again, without the condition.
    if (asked && head != NULL && head->status == 304 && !os64_streq(head->url_text, e->final)) {
        os64_fetch_close(f);
        hooks->passing_hop = false;
        way_chain_start(&hooks->chain);
        ask.extra_headers = opt->extra_headers;
        asked = false;
        f = os64_fetch_open(url, &ask);
        head = f != NULL ? os64_fetch_head(f) : NULL;
    }
    out->fetch = f != NULL ? os64_fetch_status(f) : OS64_FETCH_NO_MEMORY;
    bool served = false;
    if (head == NULL) {
        // The server could not be reached at all: a stale entry that may be
        // used so is better than no picture (CACHE.md, decision 3). A TLS
        // failure is not this: somebody may be in the way.
        bool unreachable = out->fetch == OS64_FETCH_DIAL_FAILED || out->fetch == OS64_FETCH_SILENT;
        if (have && unreachable && way_keep_stale_ok(&e->keep)) {
            served = serve(out, e, kept, kept_len, WAY_SERVED_STALE);
            way_cache_count(cache, WAY_SERVED_STALE);
            kept = NULL;
        }
    } else if (asked && head->status == 304) {
        // Not modified: the kept body — the 304 is the kept resource's,
        // since it came from the address the entry came from — with what
        // the 304 and any redirects on the way said laid over what was
        // kept, which starts its freshness again. When they forbid keeping
        // it any longer, it goes; this once, it is still the answer.
        bool keep = !hooks->passing_hop && way_keep_confirm(&e->keep, &head->keep, now_utc()) &&
                    way_chain_apply(&hooks->chain, &e->keep, now_utc());
        if (keep)
            (void)way_cache_put(cache, e, kept, kept_len);
        else
            way_cache_drop(cache, url);
        served = serve(out, e, kept, kept_len, WAY_SERVED_CONFIRMED);
        way_cache_count(cache, WAY_SERVED_CONFIRMED);
        kept = NULL;
    } else {
        size_t len = 0;
        uint8_t *bytes = read_whole(f, cap, &len);
        out->fetch = os64_fetch_status(f);
        if (bytes != NULL) {
            out->bytes = bytes;
            out->len = len;
            out->status = head->status;
            os64_strcopy(out->content_type, sizeof(out->content_type), head->content_type);
            os64_strcopy(out->charset, sizeof(out->charset), head->charset);
            os64_strcopy(out->url, sizeof(out->url), head->url_text);
            served = true;
            // Kept when it is a whole 200 reached directly or by permanent
            // redirects, and it and they say it may be (way_keep_read,
            // way_chain_apply).
            if (e != NULL && head->status == 200 && !hooks->passing_hop &&
                way_keep_read(&head->keep, now_utc(), &e->keep) &&
                way_chain_apply(&hooks->chain, &e->keep, now_utc())) {
                os64_strcopy(e->url, sizeof(e->url), url);
                os64_strcopy(e->final, sizeof(e->final), head->url_text);
                e->status = head->status;
                os64_strcopy(e->content_type, sizeof(e->content_type), head->content_type);
                os64_strcopy(e->charset, sizeof(e->charset), head->charset);
                (void)way_cache_put(cache, e, bytes, len);
            } else if (have) {
                // A new answer that is not kept is still newer than the
                // kept one, which must not come back as stale.
                way_cache_drop(cache, url);
            }
        }
    }
    if (f != NULL)
        os64_fetch_close(f);
    os64_free(kept);
    os64_free(e);
    return served;
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
    bool yes = s->face.confirm != NULL &&
               s->face.confirm(s->face.ctx, question, true, " stopped at the unencrypted hop");
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

// ── Judging a head ──────────────────────────────────────────────────────

// A media type from libfetch is already lowercased, so these compare
// verbatim.
static bool ends_with(const char *s, const char *tail)
{
    size_t n = os64_strlen(s), t = os64_strlen(tail);
    return n >= t && os64_streq(s + n - t, tail);
}

// A body a person reads as it is: text/*, and the application/ types that
// are text in all but name — JSON (what an API, and httpbin, answers a
// form with), JavaScript, and XML — which a browser shows rather than
// offering to save.
static bool type_is_text(const char *type)
{
    return (type[0] == 't' && type[1] == 'e' && type[2] == 'x' && type[3] == 't'
            && type[4] == '/')
           || way_type_is_json(type) || os64_streq(type, "application/javascript")
           || os64_streq(type, "application/xml") || ends_with(type, "+xml");
}

// The head, copied out of the fetch (way.h: the fetch's own storage dies
// with it).
static void head_copy(way_head_t *out, const os64_fetch_head_t *head, way_body_t body)
{
    os64_memset(out, 0, sizeof(*out));
    out->status = head->status;
    os64_strcopy(out->reason, sizeof(out->reason), head->reason);
    os64_strcopy(out->content_type, sizeof(out->content_type), head->content_type);
    os64_strcopy(out->charset, sizeof(out->charset), head->charset);
    os64_strcopy(out->url, sizeof(out->url), head->url_text);
    out->posted = head->method == OS64_FETCH_METHOD_POST;
    out->body = body;
}

bool way_open(way_leg_t *s, const char *url, const os64_page_request_t *request,
              way_opening_t *out, os64_fetch_status_t *why)
{
    os64_memset(out, 0, sizeof(*out));
    if (why)
        *why = OS64_FETCH_OK;
    leg_say(s, " fetching %s", url);
    show(s);

    os64_html_options_t limits = os64_html_options_default();
    os64_fetch_options_t opt = {0};
    opt.user_agent = s->agent;
    opt.accept = s->session->accept;
    opt.max_body = limits.max_bytes;     // the same page, the same cap
    opt.cancelled = fetch_cancelled;
    opt.on_hop = hop_ask;
    opt.headers_for = leg_headers;
    opt.on_set_cookie = leg_cookie;
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
    // A PICTURE ASKED FOR BY ITSELF is shown as a browser shows one, on a
    // page of its own, by a face that can; a POST's reply is not, since the
    // face's own fetch of the picture would be a GET of another thing. Only
    // a kind the face says it shows: any other picture is a file to save, as
    // it always was, not a page showing a broken picture.
    bool image = !html && !plain && head->method != OS64_FETCH_METHOD_POST &&
                 way_accept_names(s->pictures, type);
    if (!html && !plain && !image) {
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
    out->fetch = f;
    head_copy(&out->head, head, html ? WAY_BODY_HTML : image ? WAY_BODY_IMAGE : WAY_BODY_TEXT);
    const os64_fetch_detail_t *detail = os64_fetch_detail(f);
    out->head.tls_version = detail->tls_version;
    out->head.tls_fallback = detail->tls_fallback;
    os64_strcopy(out->asked, sizeof(out->asked), url);
    return true;
}

// ── Reading a body ──────────────────────────────────────────────────────

int64_t way_read(way_leg_t *s, way_opening_t *o, void *buf, size_t cap)
{
    int64_t n = os64_fetch_read(o->fetch, buf, cap);
    if (n <= 0)
        return n;
    // A fetch takes as long as the network takes, and a person deserves
    // to see how far it has got. The count is the fetch's own, so a body
    // read in pieces of any size says the same things at the same places.
    const os64_fetch_progress_t *progress = os64_fetch_progress(o->fetch);
    if (progress->produced >= o->shown + 64u * 1024u) {
        o->shown = progress->produced;
        leg_say(s, " reading %lu KB of %s", (unsigned long)(o->shown / 1024), o->asked);
        show(s);
    }
    return n;
}

// Read the body into the tree. Feeding stops at the parser's first refusal —
// which still leaves a document, because a page that was too large or too
// deep is a page you can read the beginning of.
static os64_html_document_t *parse_body(way_leg_t *s, way_opening_t *o)
{
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = o->head.charset[0] ? o->head.charset : NULL;
    opt.scripting = s->scripting;
    os64_html_parser_t *p = os64_html_parser_new(&opt);
    if (!p)
        return NULL;
    char buf[8192];
    for (;;) {
        int64_t n = way_read(s, o, buf, sizeof(buf));
        if (n <= 0)
            break;
        int64_t parsed = os64_html_parser_feed(p, buf, (size_t)n);
        while (parsed == OS64_HTML_SCRIPT)
            parsed = os64_html_parser_resume(p);
        if (parsed != OS64_HTML_OK)
            break;
    }
    return os64_html_parser_finish(p);
}

// Read the body as bytes. The cap is the parser's, because a page is a page
// whichever way it is written.
static char *read_body(way_leg_t *s, way_opening_t *o, size_t cap, size_t *len, bool *whole)
{
    size_t at = 0, size = 16u * 1024u;
    *len = 0;
    *whole = true;
    char *text = os64_malloc(size);
    if (!text)
        return NULL;
    for (;;) {
        if (at == size) {
            if (size >= cap) {
                // ASK FOR THE BYTE THAT WOULD CROSS THE CAP. libfetch refuses
                // a body at `max_body` on the read that would pass it, not
                // before, so stopping at a full buffer leaves the fetch
                // reporting OK and this page claiming to be whole when it is
                // the first 8 MB of something longer.
                char probe;
                (void)way_read(s, o, &probe, 1);
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
        int64_t n = way_read(s, o, text + at, size - at);
        if (n <= 0)
            break;
        at += (size_t)n;
    }
    *len = at;
    return text;
}

// ── A page's script ─────────────────────────────────────────────────────

// The page's address as the jar keeps cookies for it: an http or https
// page only (any other has none), encrypted when https.
static bool script_page(const char *page_url, os64_url_t *url, bool *encrypted)
{
    if (page_url == NULL || os64_url_parse(page_url, url) != OS64_URL_OK)
        return false;
    *encrypted = os64_streq(url->scheme, "https");
    return *encrypted || os64_streq(url->scheme, "http");
}

size_t way_script_cookies(way_jar_t *jar, const char *page_url, char *out, size_t cap,
                          bool *whole)
{
    os64_url_t url;
    bool encrypted;
    *whole = true;
    if (cap > 0)
        out[0] = '\0';
    if (!script_page(page_url, &url, &encrypted))
        return 0;
    int32_t left = 0;
    size_t n = way_jar_script_cookies(jar, &url, encrypted, now_utc(), out, cap, &left);
    *whole = left == 0;
    return n;
}

void way_script_cookie(way_jar_t *jar, const char *page_url, const char *text, size_t len)
{
    os64_url_t url;
    bool encrypted;
    if (script_page(page_url, &url, &encrypted))
        way_jar_script_hear(jar, &url, encrypted, text, len, now_utc());
}

// ── Loading ─────────────────────────────────────────────────────────────

bool way_load(way_leg_t *s, const char *url, const os64_page_request_t *request,
              way_page_t *out, os64_fetch_status_t *why)
{
    way_opening_t o;
    if (!way_open(s, url, request, &o, why))
        return false;
    bool short_of_memory = false;        // a truncation of OURS, not the wire's
    os64_html_options_t limits = os64_html_options_default();

    if (o.head.body == WAY_BODY_HTML) {
        out->doc = parse_body(s, &o);
        if (!out->doc) {
            leg_say(s, " out of memory reading %s", url);
            os64_fetch_close(o.fetch);
            return false;
        }
        // WHAT THE PAGE MEANS, from the tree and the address it came from —
        // `<base href>` included, which libpage reads. A page it cannot build
        // a model of is still a page worth reading, so this is not a failure
        // to return: what it costs is the links and boxes, and the face says
        // so.
        out->model = os64_page_build(out->doc, o.head.url, NULL, NULL);
    } else {
        bool whole = true;
        out->text = read_body(s, &o, limits.max_bytes, &out->textlen, &whole);
        if (!whole)
            short_of_memory = true;
        if (!out->text) {
            leg_say(s, " out of memory reading %s", url);
            os64_fetch_close(o.fetch);
            return false;
        }
        out->text_utf8 = way_text_utf8(&o.head, out->text, out->textlen);
    }

    // Only now is there a page: a false return leaves `out` empty.
    os64_fetch_status_t st = os64_fetch_status(o.fetch);
    if (why)
        *why = st;
    way_note(out, &o.head, short_of_memory, st, os64_fetch_reason(o.fetch));
    os64_fetch_close(o.fetch);
    return true;
}
