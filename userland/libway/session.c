// session.c — the pure half of a browsing session: pages, the history, and
// the judgements about what a page asks for. Nothing here touches the
// network, which is why tools/test_way_host.sh can drive all of it.

#include <stdarg.h>

#include "internal.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/url.h"

void way_say(way_session_t *s, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    os64_vsnprintf(s->status, sizeof(s->status), fmt, args);
    va_end(args);
}

way_leg_t way_leg(const way_session_t *s)
{
    way_leg_t leg;
    os64_memset(&leg, 0, sizeof(leg));
    leg.session = s;
    leg.face = s->face;
    return leg;
}

// ── Pages ───────────────────────────────────────────────────────────────

void way_page_clear(way_page_t *p)
{
    // The model points into the tree, so it goes first.
    os64_page_free(p->model);
    if (p->doc)
        os64_html_document_free(p->doc);
    os64_free(p->text);
    os64_free(p->flipped);
    p->flipped = NULL;
    p->nflipped = p->flippedcap = 0;
    p->model = NULL;
    p->doc = NULL;
    p->text = NULL;
    p->textlen = 0;
}

bool way_details_flip(way_page_t *p, const os64_html_node_t *details)
{
    for (int32_t i = 0; i < p->nflipped; i++)
        if (p->flipped[i] == details) {
            p->flipped[i] = p->flipped[--p->nflipped];   // flipped back
            return true;
        }
    if (p->nflipped == p->flippedcap) {
        int32_t cap = p->flippedcap ? p->flippedcap * 2 : 8;
        const os64_html_node_t **grown = os64_realloc(p->flipped, (size_t)cap * sizeof(*grown));
        if (grown == NULL)
            return false;
        p->flipped = grown;
        p->flippedcap = cap;
    }
    p->flipped[p->nflipped++] = details;
    return true;
}

// ── The history ─────────────────────────────────────────────────────────

static void push(way_crumb_t *stack, int32_t *depth, const char *url, const way_position_t *where)
{
    if (*depth >= WAY_HISTORY_MAX) {
        // Drop the OLDEST crumb rather than refuse to go on: somebody
        // sixty-four pages in wants the sixty-fifth more than the first.
        for (int32_t i = 1; i < WAY_HISTORY_MAX; i++)
            stack[i - 1] = stack[i];
        *depth = WAY_HISTORY_MAX - 1;
    }
    os64_strcopy(stack[*depth].url, sizeof(stack[*depth].url), url);
    stack[*depth].position = *where;
    (*depth)++;
}

void way_remember(way_session_t *s, const char *url, const way_position_t *where)
{
    push(s->history, &s->depth, url, where);
    s->ahead = 0;
}

bool way_last(way_session_t *s, way_crumb_t *out)
{
    if (s->depth == 0) {
        way_say(s, " this is where you came in");
        return false;
    }
    *out = s->history[s->depth - 1];
    return true;
}

void way_forget_last(way_session_t *s)
{
    if (s->depth > 0)
        s->depth--;
}

void way_went_back(way_session_t *s, const char *url, const way_position_t *where)
{
    way_forget_last(s);
    push(s->forward, &s->ahead, url, where);
}

bool way_next(way_session_t *s, way_crumb_t *out)
{
    if (s->ahead == 0) {
        way_say(s, " there is nowhere forward to go");
        return false;
    }
    *out = s->forward[s->ahead - 1];
    return true;
}

void way_went_forward(way_session_t *s, const char *url, const way_position_t *where)
{
    if (s->ahead > 0)
        s->ahead--;
    push(s->history, &s->depth, url, where);
}

// ── Judgements ──────────────────────────────────────────────────────────
//
// EVERY FETCH A PAGE ASKS FOR IS JUDGED HERE: a link followed, a form sent,
// a refresh the document declares. (A move within the page is no fetch and
// never comes here.) libpage has already decided where each one goes and
// says what it knows about it; what is left is this browser's own list of
// what it can carry, and the person's decision where one is owed. One door,
// because the same rules written once per road are how a road comes to miss
// one.

// Whether an address can be offered inside single quotes as a command to
// type. Quoted because husk splits a line at `;`; refused when the address
// holds a quote itself, because a page chose it, and a quote in it would
// end the quoting and run whatever the page wrote after it.
bool way_shell_quotable(const char *address)
{
    for (const char *q = address; *q != '\0'; q++)
        if (*q == '\'')
            return false;
    return true;
}

way_judgement_t way_judge(way_session_t *s, const os64_page_request_t *request, way_ask_t ask)
{
    way_judgement_t j = {.kind = WAY_REFUSE};
    // WHICH SCHEMES THIS BROWSER FOLLOWS IS ITS OWN LIST, which is why
    // libpage states the scheme instead of keeping one.
    if (os64_streq(request->scheme, "gopher")) {
        if (way_shell_quotable(request->url))
            way_say(s, " that is gopherspace - read it with:  gopher '%s'", request->url);
        else
            way_say(s, " that is gopherspace - and its address holds a quote,"
                       " so open it in gopher by hand");
        return j;
    }
    if (!os64_streq(request->scheme, "http") && !os64_streq(request->scheme, "https")) {
        way_say(s, " that is a %s: address, which %s does not fetch", request->scheme, s->name);
        return j;
    }
    if (ask != WAY_ASK_NEVER && request->downgrade) {
        os64_url_t target;
        const char *host = os64_url_parse(request->url, &target) == OS64_URL_OK
                               ? target.host : "that address";
        if (ask == WAY_ASK_SEND)
            os64_snprintf(j.question, sizeof(j.question),
                          " %s would get this unencrypted - send it?", host);
        else
            os64_snprintf(j.question, sizeof(j.question),
                          " this encrypted page sends you to %s unencrypted - go?", host);
        j.kind = WAY_QUESTION;
        j.security = true;
        j.refused = ask == WAY_ASK_SEND ? " not sent" : " stayed here";
        return j;
    }
    j.kind = WAY_FETCH;
    return j;
}

bool way_may_go(way_session_t *s, const os64_page_request_t *request, way_ask_t ask)
{
    way_judgement_t j = way_judge(s, request, ask);
    if (j.kind == WAY_QUESTION)
        return s->face.confirm(s->face.ctx, j.question, j.security, j.refused);
    return j.kind == WAY_FETCH;
}

// ── A page that asks to send you somewhere ──────────────────────────────
//
// `<meta http-equiv="refresh">` is a navigation the DOCUMENT declares, and
// following it is what makes a search engine's result links work: the HTML
// endpoints wrap every link in a click logger that answers 200 with no
// redirect, a script for a browser that runs one, and this for a browser
// that does not. libpage says whether one is declared and where to; the
// rules about whether to OBEY are here, because they are about a person
// reading rather than about a page.

way_refresh_t way_refresh_step(way_session_t *s, way_page_t *page, int32_t *chain,
                               os64_page_request_t *request)
{
    os64_memset(request, 0, sizeof(*request));
    request->control = -1;
    if (page->refresh_handled)
        return WAY_REFRESH_NONE;
    const os64_page_refresh_t *refresh = page->model ? os64_page_refresh(page->model) : NULL;
    page->refresh_handled = true;
    if (refresh == NULL)
        return WAY_REFRESH_NONE;          // an ordinary page ends the chain
    if (refresh->url.refused != OS64_PAGE_REASON_OK) {
        way_say(s, " refresh: %s", os64_page_reason_name(refresh->url.refused));
        return WAY_REFRESH_NONE;
    }
    // A DELAY IS A PERSON'S PATIENCE TO SPEND. A page that moves under a
    // reader mid-sentence is hostile, so a delayed refresh is reported and
    // left for them to act on. The "you will be redirected in five seconds"
    // page always carries a link too.
    if (refresh->seconds != 0) {
        if (refresh->names_this_document)
            way_say(s, " this page asks to reload itself every %u seconds",
                    (unsigned)refresh->seconds);
        else
            way_say(s, " this page asks to send you to %s in %u seconds%s", refresh->url.url,
                    (unsigned)refresh->seconds, s->delayed_hint ? s->delayed_hint : "");
        return WAY_REFRESH_NONE;
    }
    // A same-document refresh without a fragment would reload in a loop.
    // A fragment instead moves within this document.
    if (refresh->names_this_document && !refresh->url.has_fragment) {
        way_say(s, " this page asks to reload itself immediately, which %s does not do", s->name);
        return WAY_REFRESH_NONE;
    }
    if (++*chain > WAY_REFRESH_MAX) {
        way_say(s, " this page is the end of a chain of redirects too long to follow");
        return WAY_REFRESH_NONE;
    }
    os64_page_what_t what = {OS64_PAGE_ACTIVATE_REFRESH, 0, 0, 0};
    os64_page_verdict_t verdict = os64_page_activate(page->model, what, request);
    if (verdict == OS64_PAGE_FRAGMENT)
        return WAY_REFRESH_JUMP;
    if (verdict == OS64_PAGE_NAVIGATE)
        return WAY_REFRESH_GO;
    way_say(s, " this page asks to send you somewhere it does not name properly");
    os64_page_request_free(request);
    return WAY_REFRESH_NONE;
}

const char *way_delayed_refresh(const way_page_t *page)
{
    const os64_page_refresh_t *refresh = page->model ? os64_page_refresh(page->model) : NULL;
    if (refresh == NULL || refresh->seconds == 0 || refresh->names_this_document ||
        refresh->url.refused != OS64_PAGE_REASON_OK || refresh->url.url == NULL)
        return NULL;
    return refresh->url.url;
}

// ── What a person types ─────────────────────────────────────────────────

// A bare `host/path` means http, the way a bare host means gopher to the
// gopher client: guessing what a word means belongs to whoever is entitled
// to guess, and here that is the browser.
bool way_typed_address(const char *typed, char *out, size_t cap)
{
    for (const char *p = typed; *p != '\0' && *p != '/'; p++)
        if (p[0] == ':' && p[1] == '/' && p[2] == '/')
            return os64_strcopy(out, cap, typed) < cap;
    return (size_t)os64_snprintf(out, cap, "http://%s", typed) < cap;
}
