// test_way_host.c — libway's pure half on the host: real pages through
// libhtml and libpage, then through the judgements, the refresh rules, the
// history and the typed address, asserting each DECISION and each sentence
// exactly as a face would show it (docs/completed/06-navigator.md's evidence).
// The I/O half, way_load, needs a network; the guest walk proves it.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html/html.h"
#include "way/way.h"
#include "page/page.h"

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// ── Allocation, and failing it on purpose ───────────────────────────────

static size_t allocations, fail_at, live;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
        return NULL;
    void *at = malloc(size != 0 ? size : 1);
    if (at != NULL)
        live++;
    return at;
}

void *os64_calloc(size_t count, size_t size)
{
    void *at = os64_malloc(count * size);
    if (at != NULL)
        memset(at, 0, count * size);
    return at;
}

void *os64_realloc(void *ptr, size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations >= fail_at)
        return NULL;
    void *at = realloc(ptr, size != 0 ? size : 1);
    if (at != NULL && ptr == NULL)
        live++;
    return at;
}

void os64_free(void *ptr)
{
    if (ptr != NULL)
        live--;
    free(ptr);
}

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s%s%s\n", name, detail != NULL ? ": " : "",
                detail != NULL ? detail : "");
    }
}

static void expect_said(const char *name, const way_session_t *s, const char *want)
{
    expect(name, strcmp(s->status, want) == 0, s->status);
}

// ── A face that answers from a script ───────────────────────────────────

static struct {
    bool answer;
    int asked;
    char question[WAY_SENTENCE_MAX];
    bool security;
    const char *refused;
} s_face;

static bool face_confirm(void *ctx, const char *question, bool security, const char *refused)
{
    (void)ctx;
    s_face.asked++;
    snprintf(s_face.question, sizeof(s_face.question), "%s", question);
    s_face.security = security;
    s_face.refused = refused;
    return s_face.answer;
}

static way_session_t session(void)
{
    way_session_t s;
    memset(&s, 0, sizeof(s));
    s.name = "wend";
    s.delayed_hint = " - press g to go";
    s.face.confirm = face_confirm;
    return s;
}

// ── Pages ───────────────────────────────────────────────────────────────

static way_page_t page_of(const char *html, const char *url)
{
    way_page_t p;
    memset(&p, 0, sizeof(p));
    os64_html_options_t opt = os64_html_options_default();
    opt.charset = "utf-8";
    os64_html_parser_t *parser = os64_html_parser_new(&opt);
    if (parser == NULL)
        return p;
    os64_html_parser_feed(parser, html, strlen(html));
    p.doc = os64_html_parser_finish(parser);
    if (p.doc != NULL)
        p.model = os64_page_build(p.doc, url, NULL);
    snprintf(p.url, sizeof(p.url), "%s", url);
    return p;
}

// Activates link `i` (or control `i` pressed) and returns the verdict.
static os64_page_verdict_t activate(const way_page_t *p, os64_page_activation_t how, int32_t i,
                                    os64_page_request_t *request)
{
    os64_page_what_t what = {how, i, 0, 0};
    return os64_page_activate(p->model, what, request);
}

// ── The judgements ──────────────────────────────────────────────────────

static void judge_case(const char *name, const char *html, const char *url,
                       os64_page_activation_t how, way_ask_t ask, way_judgement_kind_t kind,
                       const char *said, const char *question)
{
    way_session_t s = session();
    way_page_t p = page_of(html, url);
    os64_page_request_t request;
    os64_page_verdict_t v = activate(&p, how, 0, &request);
    expect(name, v == OS64_PAGE_NAVIGATE, "the page did not ask to navigate");
    if (v == OS64_PAGE_NAVIGATE) {
        way_judgement_t j = way_judge(&s, &request, ask);
        expect(name, j.kind == kind, NULL);
        if (said != NULL)
            expect_said(name, &s, said);
        if (question != NULL)
            expect(name, strcmp(j.question, question) == 0, j.question);
    }
    os64_page_request_free(&request);
    way_page_clear(&p);
}

static void judgements(void)
{
    judge_case("judge: an http link is fetched", "<a href=/next>n</a>", "http://h/p",
               OS64_PAGE_ACTIVATE_LINK, WAY_ASK_NEVER, WAY_FETCH, "", NULL);
    judge_case("judge: gopher is handed to the gopher client",
               "<a href=gopher://g.org/1/x>g</a>", "http://h/p", OS64_PAGE_ACTIVATE_LINK,
               WAY_ASK_NEVER, WAY_REFUSE,
               " that is gopherspace - read it with:  gopher 'gopher://g.org/1/x'", NULL);
    judge_case("judge: a gopher address holding a quote is not offered as a command",
               "<a href=\"gopher://g.org/1/it's\">g</a>", "http://h/p", OS64_PAGE_ACTIVATE_LINK,
               WAY_ASK_NEVER, WAY_REFUSE,
               " that is gopherspace - and its address holds a quote, so open it in gopher by"
               " hand", NULL);
    judge_case("judge: a scheme this browser does not fetch is named",
               "<a href=mailto:a@b.c>m</a>", "http://h/p", OS64_PAGE_ACTIVATE_LINK,
               WAY_ASK_NEVER, WAY_REFUSE, " that is a mailto: address, which wend does not fetch",
               NULL);
    judge_case("judge: a POST form is sent (packet 05)",
               "<form method=post action=/p><input type=submit></form>", "http://h/p",
               OS64_PAGE_ACTIVATE_CONTROL, WAY_ASK_SEND, WAY_FETCH, "", NULL);
    judge_case("judge: a POST leaving https for http asks first",
               "<form method=post action=http://e.org/p><input type=submit></form>",
               "https://h/p", OS64_PAGE_ACTIVATE_CONTROL, WAY_ASK_SEND, WAY_QUESTION, "",
               " e.org would get this unencrypted - send it?");
    judge_case("judge: a form leaving https for http asks first",
               "<form action=http://e.org/s><input type=submit></form>", "https://h/p",
               OS64_PAGE_ACTIVATE_CONTROL, WAY_ASK_SEND, WAY_QUESTION, "",
               " e.org would get this unencrypted - send it?");
    judge_case("judge: a link leaving https for http does not ask",
               "<a href=http://e.org/>e</a>", "https://h/p", OS64_PAGE_ACTIVATE_LINK,
               WAY_ASK_NEVER, WAY_FETCH, "", NULL);

    // The question put through the face: its words, its security flag and
    // the sentence for a no; the answer decides.
    way_page_t p = page_of("<form action=http://e.org/s><input type=submit></form>",
                           "https://h/p");
    os64_page_request_t request;
    activate(&p, OS64_PAGE_ACTIVATE_CONTROL, 0, &request);
    way_session_t s = session();
    memset(&s_face, 0, sizeof(s_face));
    s_face.answer = true;
    bool yes = way_may_go(&s, &request, WAY_ASK_SEND);
    expect("may go: yes goes", yes && s_face.asked == 1 && s_face.security &&
                                   strcmp(s_face.refused, " not sent") == 0, NULL);
    s_face.answer = false;
    expect("may go: no stays", !way_may_go(&s, &request, WAY_ASK_SEND), NULL);
    s_face.asked = 0;
    expect("may go: a link is not asked",
           way_may_go(&s, &request, WAY_ASK_NEVER) && s_face.asked == 0, NULL);
    os64_page_request_free(&request);
    way_page_clear(&p);
}

// ── The refresh rules ───────────────────────────────────────────────────

static void refresh_case(const char *name, const char *meta, const char *url, int32_t chain_before,
                         way_refresh_t want, const char *said, const char *goes_to)
{
    char html[512];
    snprintf(html, sizeof(html), "<meta http-equiv=refresh content=\"%s\"><p id=here>x", meta);
    way_session_t s = session();
    way_page_t p = page_of(html, url);
    int32_t chain = chain_before;
    os64_page_request_t request;
    way_refresh_t got = way_refresh_step(&s, &p, &chain, &request);
    expect(name, got == want, NULL);
    if (said != NULL)
        expect_said(name, &s, said);
    if (goes_to != NULL)
        expect(name, request.url != NULL && strcmp(request.url, goes_to) == 0, request.url);
    if (want == WAY_REFRESH_JUMP)
        expect(name, request.anchor != NULL && request.anchor->kind == OS64_HTML_ELEMENT, NULL);
    os64_page_request_free(&request);
    // Judged once per loaded page, however it was answered.
    got = way_refresh_step(&s, &p, &chain, &request);
    expect(name, got == WAY_REFRESH_NONE, "a second look acted again");
    os64_page_request_free(&request);
    way_page_clear(&p);
}

static void refreshes(void)
{
    refresh_case("refresh: an immediate one goes", "0; url=/next", "http://h/p", 0,
                 WAY_REFRESH_GO, "", "http://h/next");
    refresh_case("refresh: a delayed one is offered, not followed", "30; url=/next",
                 "http://h/p", 0, WAY_REFRESH_NONE,
                 " this page asks to send you to http://h/next in 30 seconds - press g to go",
                 NULL);
    refresh_case("refresh: a delayed reload of itself is said", "10", "http://h/p", 0,
                 WAY_REFRESH_NONE, " this page asks to reload itself every 10 seconds", NULL);
    refresh_case("refresh: an immediate reload of itself is refused", "0", "http://h/p", 0,
                 WAY_REFRESH_NONE,
                 " this page asks to reload itself immediately, which wend does not do", NULL);
    refresh_case("refresh: the sixth hop of a chain is refused", "0; url=/next", "http://h/p",
                 WAY_REFRESH_MAX, WAY_REFRESH_NONE,
                 " this page is the end of a chain of redirects too long to follow", NULL);
    refresh_case("refresh: the fifth hop still goes", "0; url=/next", "http://h/p",
                 WAY_REFRESH_MAX - 1, WAY_REFRESH_GO, "", "http://h/next");
    refresh_case("refresh: a fragment of this page is a move", "0; url=#here", "http://h/p", 0,
                 WAY_REFRESH_JUMP, "", NULL);

    way_page_t p = page_of("<meta http-equiv=refresh content=\"30; url=/later\">", "http://h/p");
    const char *later = way_delayed_refresh(&p);
    expect("delayed: the address offered", later != NULL && strcmp(later, "http://h/later") == 0,
           later);
    way_page_clear(&p);
    p = page_of("<meta http-equiv=refresh content=\"0; url=/now\">", "http://h/p");
    expect("delayed: an immediate one is not offered", way_delayed_refresh(&p) == NULL, NULL);
    way_page_clear(&p);
    p = page_of("<p>plain", "http://h/p");
    expect("delayed: a plain page offers nothing", way_delayed_refresh(&p) == NULL, NULL);
    way_page_clear(&p);
}

// ── The history ─────────────────────────────────────────────────────────

static void history(void)
{
    way_session_t s = session();
    way_crumb_t crumb;
    expect("history: Back with nowhere to go says so",
           !way_last(&s, &crumb) && strcmp(s.status, " this is where you came in") == 0,
           s.status);
    for (int i = 0; i <= WAY_HISTORY_MAX; i++) {
        char url[32];
        snprintf(url, sizeof(url), "http://h/%d", i);
        way_position_t where;
        memset(&where, 0, sizeof(where));
        where.bytes[0] = (uint8_t)i;
        where.bytes[WAY_POSITION_MAX - 1] = 0xA5;
        way_remember(&s, url, &where);
    }
    expect("history: the sixty-fifth crumb drops the first",
           s.depth == WAY_HISTORY_MAX && strcmp(s.history[0].url, "http://h/1") == 0, NULL);
    expect("history: the last crumb, and its position untouched",
           way_last(&s, &crumb) && strcmp(crumb.url, "http://h/64") == 0 &&
               crumb.position.bytes[0] == 64 && crumb.position.bytes[WAY_POSITION_MAX - 1] == 0xA5,
           NULL);
    expect("history: looking does not spend", s.depth == WAY_HISTORY_MAX, NULL);
    way_forget_last(&s);
    expect("history: forgetting spends one",
           s.depth == WAY_HISTORY_MAX - 1 && way_last(&s, &crumb) &&
               strcmp(crumb.url, "http://h/63") == 0, NULL);
}

// Back and Forward: the page Back leaves is where Forward goes, Forward's
// arrival puts the page it left back on the history, and following
// anything new empties what was ahead.
static void forward(void)
{
    way_session_t s = session();
    way_position_t at;
    memset(&at, 0, sizeof(at));
    way_crumb_t crumb;
    expect("forward: nowhere to go at first",
           !way_next(&s, &crumb) && strcmp(s.status, " there is nowhere forward to go") == 0,
           s.status);
    way_remember(&s, "http://h/a", &at);    // a -> b
    way_remember(&s, "http://h/b", &at);    // b -> c
    at.bytes[0] = 7;
    way_went_back(&s, "http://h/c", &at);   // back from c to b
    expect("forward: Back leaves the page ahead",
           s.depth == 1 && way_next(&s, &crumb) && strcmp(crumb.url, "http://h/c") == 0 &&
               crumb.position.bytes[0] == 7, NULL);
    way_went_back(&s, "http://h/b", &at);   // back from b to a
    expect("forward: two back, two ahead, newest last",
           s.depth == 0 && s.ahead == 2 && way_next(&s, &crumb) &&
               strcmp(crumb.url, "http://h/b") == 0, NULL);
    way_went_forward(&s, "http://h/a", &at);  // forward from a to b
    expect("forward: Forward puts the page left back on the history",
           s.depth == 1 && strcmp(s.history[0].url, "http://h/a") == 0 && s.ahead == 1 &&
               way_next(&s, &crumb) && strcmp(crumb.url, "http://h/c") == 0, NULL);
    way_remember(&s, "http://h/b", &at);      // b -> somewhere new
    expect("forward: a new way empties what was ahead", s.ahead == 0 && s.depth == 2, NULL);

    // The delayed refresh's sentence carries the face's own offer.
    way_page_t p = page_of("<meta http-equiv=refresh content=\"5; url=/x\">", "http://h/p");
    s.delayed_hint = NULL;
    int32_t chain = 0;
    os64_page_request_t request;
    way_refresh_step(&s, &p, &chain, &request);
    expect_said("refresh: a face with nothing to offer offers nothing", &s,
                " this page asks to send you to http://h/x in 5 seconds");
    way_page_clear(&p);

    way_leg_t leg = way_leg(&s);
    expect("leg: the session's identity and face, an empty sentence",
           leg.session == &s && leg.face.confirm == face_confirm && leg.status[0] == '\0', NULL);
}

// ── Typed addresses and details ─────────────────────────────────────────

static void typed(void)
{
    char out[64];
    expect("typed: a bare host means http",
           way_typed_address("example.com", out, sizeof(out)) &&
               strcmp(out, "http://example.com") == 0, out);
    expect("typed: a scheme is kept",
           way_typed_address("https://a.b/c", out, sizeof(out)) && strcmp(out, "https://a.b/c") == 0,
           out);
    expect("typed: a :// after the first slash is not a scheme",
           way_typed_address("a.b/x://y", out, sizeof(out)) && strcmp(out, "http://a.b/x://y") == 0,
           out);
    // 18 bytes hold "http://" and ten more, with the terminator.
    char tiny[18];
    expect("typed: one byte too long is refused, not trimmed",
           !way_typed_address("a.bc/defghi", tiny, sizeof(tiny)), NULL);
    expect("typed: one that just fits is taken",
           way_typed_address("a.bc/defgh", tiny, sizeof(tiny)) &&
               strcmp(tiny, "http://a.bc/defgh") == 0, tiny);

    way_page_t p = page_of("<details id=a></details><details id=b></details>", "http://h/p");
    const os64_html_node_t *a = p.doc->body->first_child, *b = a->next;
    bool ok = way_details_flip(&p, a) && way_details_flip(&p, b) && p.nflipped == 2 &&
              way_details_flip(&p, a) && p.nflipped == 1 && p.flipped[0] == b;
    expect("details: a flip flips back", ok, NULL);
    way_page_clear(&p);
}

// ── Out of memory at every allocation ───────────────────────────────────

// Everything above that allocates, as one walk: build, judge a form, a
// refresh step, the details flips. At each failure the answer may be a
// refusal; it may never be a crash or a leak.
static void walk(void)
{
    way_session_t s = session();
    way_page_t p = page_of("<meta http-equiv=refresh content=\"0; url=/next\">"
                           "<form action=http://e.org/s><input name=q value=x><input type=submit>"
                           "</form><details></details><details></details>",
                           "https://h/p");
    if (p.model != NULL) {
        os64_page_request_t request;
        if (activate(&p, OS64_PAGE_ACTIVATE_CONTROL, 1, &request) == OS64_PAGE_NAVIGATE)
            (void)way_judge(&s, &request, WAY_ASK_SEND);
        os64_page_request_free(&request);
        int32_t chain = 0;
        (void)way_refresh_step(&s, &p, &chain, &request);
        os64_page_request_free(&request);
        const os64_html_node_t *d = p.doc->body->last_child;
        (void)way_details_flip(&p, d);
        (void)way_details_flip(&p, d->prev);
    }
    way_page_clear(&p);
}

static void sweep(void)
{
    size_t base = live;
    allocations = 0;
    walk();
    size_t count = allocations;
    for (size_t at = 1; at <= count; at++) {
        allocations = 0;
        fail_at = at;
        walk();
        fail_at = 0;
        if (live != base) {
            char detail[64];
            snprintf(detail, sizeof(detail), "at allocation %zu", at);
            expect("sweep: nothing leaked", false, detail);
            return;
        }
    }
    printf("libway sweep: each of %zu allocations failed in turn, nothing leaked\n", count);
}

int main(void)
{
    judgements();
    refreshes();
    history();
    forward();
    typed();
    sweep();
    expect("nothing leaked", live == 0, NULL);
    printf("libway: %d checks, %d failed\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
