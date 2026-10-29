// test_yonder_fetch_job_host.c — yonder's fetch job (yonder/fetch_job.c) on
// the host, under the sanitizers, against libfetch's contract faked
// (tools/test_fetch_fake.inc) with the real origin.c and the real cookie jar:
// a case per rule the job applies in order, asserting WHICH requests went
// out (the preflight or none, the Origin, the cookies) and what the page is
// handed back, then every allocation failed in turn.

#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fetch_job.h"
#include "os64/date.h"
#include "os64/mem.h"
#include "os64/str.h"

// ── The heap, the clocks, the console ───────────────────────────────────

// fail_at: the allocation that fails first; with fail_once, the only one.
static size_t allocations, fail_at, live;
static bool fail_once;

void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && (fail_once ? allocations == fail_at : allocations >= fail_at))
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
    if (fail_at != 0 && (fail_once ? allocations == fail_at : allocations >= fail_at))
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

void os64_yield(void)
{
}

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

int64_t os64_time(os64_time_t *t)
{
    memset(t, 0, sizeof(*t));
    t->epoch = 1792600000;
    return 0;
}

static uint64_t clock_ms = 1000;
uint64_t yonder_now_ms(void)
{
    return clock_ms;
}

#include "test_fetch_fake.inc"

static int checks, failures;

static void expect(const char *name, bool ok, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL %s%s%s\n", name, detail != NULL ? ": " : "", detail != NULL ? detail : "");
    }
}

static void expect_text(const char *name, const char *got, const char *want)
{
    if (got == NULL)
        got = "(null)";
    char detail[1200];
    snprintf(detail, sizeof(detail), "got \"%s\", want \"%s\"", got, want);
    expect(name, strcmp(got, want) == 0, detail);
}

// ── A request ───────────────────────────────────────────────────────────

static way_jar_t *jar;
static way_preflight_t *cache;
static bool cancel_now;

static bool cancel_cb(void *ctx)
{
    (void)ctx;
    return cancel_now;
}

typedef struct {
    const char *page, *url, *method, *headers, *body;
    way_mode_t mode;
    way_credentials_t credentials;
    way_redirect_t redirect;
    way_policy_t policy;
    bool flag;
    uint64_t max_body;
    const way_identity_t *identity;     // the agent's preset lines, NULL for none
} Ask;

static yonder_fetch_job_t *job_of(const Ask *a)
{
    yonder_fetch_job_t *j = os64_calloc(1, sizeof(*j));
    if (j == NULL)
        return NULL;
    j->kind = YONDER_JOB_FETCH;
    j->agent = "yonder-test";
    j->identity = a->identity;
    j->url = os64_malloc(strlen(a->url) + 1);
    const char *h = a->headers != NULL ? a->headers : "";
    j->headers = os64_malloc(strlen(h) + 1);
    if (j->url == NULL || j->headers == NULL) {
        yonder_fetch_release(j, NULL);
        return NULL;
    }
    strcpy(j->url, a->url);
    strcpy(j->headers, h);
    j->headers_len = strlen(h);
    if (a->body != NULL) {
        j->body = os64_malloc(strlen(a->body) + 1);
        if (j->body == NULL) {
            yonder_fetch_release(j, NULL);
            return NULL;
        }
        memcpy(j->body, a->body, strlen(a->body));
        j->body_len = strlen(a->body);
    }
    way_origin_of(a->page, 77, &j->request.origin);
    j->request.mode = a->mode;
    j->request.credentials = a->credentials;
    j->request.redirect = a->redirect;
    j->request.policy = a->policy;
    j->request.preflight_flag = a->flag;
    snprintf(j->request.method, sizeof(j->request.method), "%s", a->method != NULL ? a->method : "GET");
    way_method_normalize(j->request.method);
    snprintf(j->referrer, sizeof(j->referrer), "%s", a->page);
    j->max_body = a->max_body;
    j->jar = jar;
    j->preflight = cache;
    return j;
}

// The job run, as the pool would; the job is released, the product kept.
static yonder_fetched_t *run(const Ask *a)
{
    yonder_fetch_job_t *j = job_of(a);
    if (j == NULL)
        return NULL;
    void *out = NULL;
    yonder_fetch_run(j, cancel_cb, NULL, &out);
    yonder_fetch_release(j, NULL);
    return out;
}

static void fresh(void)
{
    fake_fetch_reset();
    way_jar_free(jar);
    way_preflight_free(cache);
    jar = way_jar_new();
    cache = way_preflight_new();
    cancel_now = false;
    clock_ms = 1000;
}

static void seed_cookie(const char *url, const char *cookie)
{
    os64_url_t u;
    os64_url_parse(url, &u);
    way_jar_hear(jar, &u, strncmp(url, "https", 5) == 0, cookie, strlen(cookie), 1792600000);
}

static const char *cookies_for(const char *url)
{
    static char out[512];
    os64_url_t u;
    os64_url_parse(url, &u);
    out[0] = '\0';
    way_jar_cookies(jar, &u, strncmp(url, "https", 5) == 0, 1792600000, out, sizeof(out), NULL);
    return out;
}

static const char *field(int i, const char *name)
{
    static char out[2048];
    return fake_field(i, name, out, sizeof(out));
}

static bool has_line(const yonder_fetched_t *g, const char *line)
{
    return g->headers != NULL && strstr(g->headers, line) != NULL;
}

// ── The cases ───────────────────────────────────────────────────────────

static void same_origin(void)
{
    fresh();
    seed_cookie("https://a.test/", "sid=1");
    fake_answer(NULL, "https://a.test/api", 200, "Content-Type: text/plain\r\nX-Mine: 1\r\nSet-Cookie: next=2\r\n",
                "hello");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://a.test/api#frag"});
    expect("basic: ok", g != NULL && g->ok && g->type == WAY_TYPE_BASIC && g->status == 200, g ? g->why : NULL);
    expect("basic: one request, no preflight", nfake_requests == 1 && !strcmp(fake_requests[0].method, "GET"), NULL);
    expect_text("basic: a GET carries no Origin", field(0, "Origin"), "");
    expect_text("basic: same-origin credentials send the cookie", field(0, "Cookie"), "sid=1");
    expect_text("basic: the Referer is the page", field(0, "Referer"), "https://a.test/page");
    expect_text("basic: Accept defaults to */*", fake_requests[0].accept, "*/*");
    expect("basic: Set-Cookie heard", strstr(cookies_for("https://a.test/"), "next=2") != NULL, NULL);
    expect("basic: every header but Set-Cookie", has_line(g, "X-Mine: 1\r\n") && !has_line(g, "Set-Cookie"), g->headers);
    expect("basic: the body", g->has_body && g->body_len == 5 && !memcmp(g->body, "hello", 5), NULL);
    expect_text("basic: the URL without its fragment", g->url, "https://a.test/api");
    expect("basic: not redirected", !g->redirected && g->hops == 0, NULL);
    yonder_fetched_free(g);

    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api", "POST", "Content-Type: text/plain\r\n", "body"});
    expect_text("basic: a POST carries Origin", field(0, "Origin"), "https://a.test");
    expect("basic: the body went", fake_requests[0].body_len == 4 && !strcmp(fake_requests[0].body, "body"), NULL);
    expect_text("basic: Content-Type as a body header", fake_requests[0].body_headers, "Content-Type: text/plain\r\n");
    yonder_fetched_free(g);

    fresh();
    seed_cookie("https://a.test/", "sid=1");
    fake_answer(NULL, "https://a.test/api", 200, "Set-Cookie: no=1\r\n", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api", .credentials = WAY_CREDENTIALS_OMIT});
    expect_text("omit: no cookie", field(0, "Cookie"), "");
    expect("omit: Set-Cookie not heard", strstr(cookies_for("https://a.test/"), "no=1") == NULL, NULL);
    yonder_fetched_free(g);
}

static void cross_origin(void)
{
    fresh();
    seed_cookie("https://b.test/", "theirs=1");
    fake_answer(NULL, "https://b.test/api", 200, "Content-Type: text/plain\r\nX-Secret: 1\r\nSet-Cookie: heard=1\r\n", "data");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/api"});
    expect("cors: no ACAO is a network error", g != NULL && !g->ok && g->type == WAY_TYPE_ERROR, NULL);
    expect_text("cors: Origin sent", field(0, "Origin"), "https://a.test");
    expect_text("cors: same-origin credentials send no cookie across", field(0, "Cookie"), "");
    expect("cors: nor hear one", strstr(cookies_for("https://b.test/"), "heard=1") == NULL, NULL);
    expect("cors: the body was never read", fake_reads == 0, NULL);
    yonder_fetched_free(g);

    fresh();
    fake_answer(NULL, "https://b.test/api", 200,
                "Access-Control-Allow-Origin: *\r\nContent-Type: text/plain\r\nX-Secret: 1\r\nX-Shown: 2\r\n"
                "Access-Control-Expose-Headers: X-Shown\r\n", "data");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api"});
    expect("cors: * reads", g != NULL && g->ok && g->type == WAY_TYPE_CORS, g ? g->why : NULL);
    expect("cors: safelisted and exposed only", has_line(g, "Content-Type: text/plain\r\n") &&
           has_line(g, "X-Shown: 2\r\n") && !has_line(g, "X-Secret"), g->headers);
    yonder_fetched_free(g);

    // include: cookies go, the answer must name the origin and say true.
    fresh();
    seed_cookie("https://b.test/", "theirs=1");
    fake_answer(NULL, "https://b.test/api", 200,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Credentials: true\r\n"
                "Set-Cookie: fresh=1\r\n", "ok");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api", .credentials = WAY_CREDENTIALS_INCLUDE});
    expect("include: ok", g != NULL && g->ok && g->type == WAY_TYPE_CORS, g ? g->why : NULL);
    expect_text("include: the cookie went", field(0, "Cookie"), "theirs=1");
    expect("include: the cookie was heard", strstr(cookies_for("https://b.test/"), "fresh=1") != NULL, NULL);
    yonder_fetched_free(g);

    // C: credentials included, the CORS check fails, and the cookie is in
    // the jar nevertheless (§ 4.7 hears before § 4.4 checks).
    fresh();
    fake_answer(NULL, "https://b.test/api", 200, "Access-Control-Allow-Origin: *\r\nSet-Cookie: kept=1\r\n", "no");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api", .credentials = WAY_CREDENTIALS_INCLUDE});
    expect("C: * under include fails", g != NULL && !g->ok, NULL);
    expect("C: and the cookie was heard first", strstr(cookies_for("https://b.test/"), "kept=1") != NULL, NULL);
    yonder_fetched_free(g);
}

// A preflight granted on one hop, a redirect that taints the request, and a
// later hop whose preflight fails: the network error clears the FIRST
// hop's grant, cached under the request origin as it was then
// (https://a.test), though the request origin is "null" by the end.
static void preflight_cleared_across_a_taint(void)
{
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                "Access-Control-Max-Age: 600\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 307,
                "Location: https://c.test/x\r\nAccess-Control-Allow-Origin: https://a.test\r\n", NULL);
    fake_answer("OPTIONS", "https://c.test/x", 204, "X-No-Grant: 1\r\n", NULL);
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT", NULL, "x"});
    expect("taint: the chain ends in a network error", g != NULL && !g->ok, g ? g->why : NULL);
    expect("taint: it went b's OPTIONS, b's PUT, c's OPTIONS", nfake_requests == 3 &&
           !strcmp(fake_requests[2].method, "OPTIONS") && !strcmp(fake_requests[2].url, "https://c.test/x"), NULL);
    expect_text("taint: c's preflight carried the tainted origin", field(2, "Origin"), "null");
    expect("taint: b's grant is cleared too", way_preflight_count(cache) == 0, NULL);
    yonder_fetched_free(g);
}

static void preflights(void)
{
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                "Access-Control-Allow-Headers: x-foo\r\nAccess-Control-Max-Age: 60\r\nSet-Cookie: pre=1\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "done");
    seed_cookie("https://b.test/", "theirs=1");
    Ask put = {"https://a.test/page", "https://b.test/api", "PUT", "X-Foo: 1\r\nContent-Type: application/json\r\n",
               "{}", .credentials = WAY_CREDENTIALS_INCLUDE};
    // include needs Allow-Credentials on the preflight too; not given here.
    yonder_fetched_t *g = run(&put);
    expect("preflight: include without Allow-Credentials fails", g != NULL && !g->ok, NULL);
    expect("preflight: and the PUT never went", nfake_requests == 1 && !strcmp(fake_requests[0].method, "OPTIONS"), NULL);
    yonder_fetched_free(g);

    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                "Access-Control-Allow-Headers: x-foo, content-type\r\nAccess-Control-Max-Age: 60\r\nSet-Cookie: pre=1\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "done");
    seed_cookie("https://b.test/", "theirs=1");
    put.credentials = WAY_CREDENTIALS_SAME_ORIGIN;
    g = run(&put);
    expect("preflight: ok", g != NULL && g->ok, g ? g->why : NULL);
    expect("preflight: OPTIONS then PUT", nfake_requests == 2 && !strcmp(fake_requests[0].method, "OPTIONS") &&
           !strcmp(fake_requests[1].method, "PUT"), NULL);
    expect_text("preflight: Access-Control-Request-Method", field(0, "Access-Control-Request-Method"), "PUT");
    expect_text("preflight: Access-Control-Request-Headers", field(0, "Access-Control-Request-Headers"), "content-type,x-foo");
    expect_text("preflight: its Origin", field(0, "Origin"), "https://a.test");
    expect_text("preflight: no cookie", field(0, "Cookie"), "");
    expect_text("preflight: its Accept", fake_requests[0].accept, "*/*");
    expect("preflight: no body, no X-Foo", fake_requests[0].body_len == 0 && !strcmp(field(0, "X-Foo"), ""), NULL);
    expect("preflight: its Set-Cookie not heard", strstr(cookies_for("https://b.test/"), "pre=1") == NULL, NULL);
    expect_text("preflight: the PUT's Origin", field(1, "Origin"), "https://a.test");
    expect_text("preflight: the PUT's X-Foo", field(1, "X-Foo"), "1");
    yonder_fetched_free(g);
    // A second time, inside Max-Age: the cache answers.
    g = run(&put);
    expect("preflight: cached", g != NULL && g->ok && nfake_requests == 3 && !strcmp(fake_requests[2].method, "PUT"), NULL);
    yonder_fetched_free(g);
    // Past it: asked again.
    clock_ms += 60000;
    g = run(&put);
    expect("preflight: expired", g != NULL && g->ok && nfake_requests == 5 && !strcmp(fake_requests[3].method, "OPTIONS"), NULL);
    yonder_fetched_free(g);
    // The actual request failing its CORS check clears the grants.
    fake_answers[1].headers = "";
    g = run(&put);
    expect("preflight: a failed request", g != NULL && !g->ok, NULL);
    expect("preflight: clears the grants", way_preflight_count(cache) == 0, NULL);
    yonder_fetched_free(g);

    // A preflight answered with a redirect is not ok.
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 302, "Location: https://b.test/x\r\nAccess-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: PUT\r\n", NULL);
    put.headers = "";
    g = run(&put);
    expect("preflight: a redirect fails it", g != NULL && !g->ok && nfake_requests == 1, NULL);
    yonder_fetched_free(g);

    // XHR's upload listener: the flag preflights a simple GET.
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", NULL);
    fake_answer("GET", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "x");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api", .flag = true});
    expect("preflight: the flag", g != NULL && g->ok && nfake_requests == 2 && !strcmp(fake_requests[0].method, "OPTIONS"), g ? g->why : NULL);
    yonder_fetched_free(g);
}

static void redirects(void)
{
    // Same origin to another: the Authorization goes, tainting is cors, the
    // Origin is the page's (the hop came FROM the page's own origin).
    fresh();
    seed_cookie("https://a.test/", "sid=1");
    seed_cookie("https://b.test/", "theirs=1");
    fake_answer(NULL, "https://a.test/r", 302, "Location: https://b.test/final\r\n", NULL);
    fake_answer(NULL, "https://b.test/final", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "end");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://a.test/r", "GET", "Authorization: token\r\n"});
    expect("redirect: ok", g != NULL && g->ok && g->type == WAY_TYPE_CORS && g->redirected && g->hops == 1, g ? g->why : NULL);
    expect_text("redirect: hop 1 had Authorization", field(0, "Authorization"), "token");
    expect_text("redirect: and the cookie", field(0, "Cookie"), "sid=1");
    expect_text("redirect: hop 2 lost Authorization", field(1, "Authorization"), "");
    expect_text("redirect: and sent no cookie", field(1, "Cookie"), "");
    expect_text("redirect: its Origin", field(1, "Origin"), "https://a.test");
    expect_text("redirect: the final URL", g->url, "https://b.test/final");
    yonder_fetched_free(g);

    // Another origin to a third: Origin: null, and the answer must allow null.
    fresh();
    fake_answer(NULL, "https://b.test/r", 302, "Location: https://c.test/final\r\nAccess-Control-Allow-Origin: *\r\n", NULL);
    fake_answer(NULL, "https://c.test/final", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "end");
    g = run(&(Ask){"https://a.test/page", "https://b.test/r"});
    expect_text("redirect: tainted Origin", field(1, "Origin"), "null");
    expect("redirect: the page's origin no longer matches", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    fresh();
    fake_answer(NULL, "https://b.test/r", 302, "Location: https://b.test/final\r\n", NULL);
    fake_answer(NULL, "https://b.test/final", 200, "Access-Control-Allow-Origin: *\r\n", "end");
    g = run(&(Ask){"https://a.test/page", "https://b.test/r"});
    expect("redirect: a cross-origin redirect without ACAO fails", g != NULL && !g->ok && nfake_requests == 1, NULL);
    yonder_fetched_free(g);

    // 303 makes a GET of a POST, without its body or body headers.
    fresh();
    fake_answer(NULL, "https://a.test/form", 303, "Location: /done\r\n", NULL);
    fake_answer(NULL, "https://a.test/done", 200, "", "done");
    g = run(&(Ask){"https://a.test/page", "https://a.test/form", "POST", "Content-Type: text/plain\r\nX-Keep: 1\r\n", "data"});
    expect("303: ok", g != NULL && g->ok, g ? g->why : NULL);
    expect("303: a GET with no body", !strcmp(fake_requests[1].method, "GET") && fake_requests[1].body_len == 0 &&
           fake_requests[1].body_headers[0] == '\0' && !strcmp(field(1, "X-Keep"), "1"), NULL);
    yonder_fetched_free(g);
    // 307 replays it.
    fresh();
    fake_answer(NULL, "https://a.test/form", 307, "Location: /again\r\n", NULL);
    fake_answer(NULL, "https://a.test/again", 200, "", "done");
    g = run(&(Ask){"https://a.test/page", "https://a.test/form", "POST", "Content-Type: text/plain\r\n", "data"});
    expect("307: the POST again", g != NULL && g->ok && !strcmp(fake_requests[1].method, "POST") &&
           fake_requests[1].body_len == 4, NULL);
    yonder_fetched_free(g);

    // Modes.
    fresh();
    fake_answer(NULL, "https://a.test/r", 302, "Location: /x\r\n", "courtesy");
    g = run(&(Ask){"https://a.test/page", "https://a.test/r", .redirect = WAY_REDIRECT_ERROR});
    expect("redirect error", g != NULL && !g->ok && nfake_requests == 1, NULL);
    yonder_fetched_free(g);
    fresh();
    fake_answer(NULL, "https://a.test/r", 302, "Location: /x\r\nX-A: 1\r\n", "courtesy");
    g = run(&(Ask){"https://a.test/page", "https://a.test/r", .redirect = WAY_REDIRECT_MANUAL});
    expect("redirect manual: opaqueredirect", g != NULL && g->ok && g->type == WAY_TYPE_OPAQUEREDIRECT &&
           g->status == 0 && !g->has_body && (g->headers == NULL || g->headers_len == 0) && nfake_requests == 1, NULL);
    expect_text("redirect manual: keeps its URL", g->url, "https://a.test/r");
    yonder_fetched_free(g);

    // The limits.
    fresh();
    static char urls[22][64], locs[22][96];
    for (int i = 0; i <= 21; i++) {
        snprintf(urls[i], sizeof(urls[i]), "https://a.test/%d", i);
        snprintf(locs[i], sizeof(locs[i]), "Location: /%d\r\n", i + 1);
        fake_answer(NULL, urls[i], i == 20 ? 200 : 302, i == 20 ? "" : locs[i], "x");
    }
    g = run(&(Ask){"https://a.test/page", "https://a.test/0"});
    expect("20 redirects", g != NULL && g->ok && g->hops == 20, g ? g->why : NULL);
    yonder_fetched_free(g);
    fake_answers[20].status = 302;
    fake_answers[20].headers = locs[20];
    g = run(&(Ask){"https://a.test/page", "https://a.test/0"});
    expect("the 21st", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    fresh();
    fake_answer(NULL, "https://a.test/r", 302, "Location: http://a.test/plain\r\n", NULL);
    g = run(&(Ask){"http://a.test/page", "https://a.test/r", .mode = WAY_MODE_NO_CORS});
    expect("a downgrade is refused, even from an http page", g != NULL && !g->ok && nfake_requests == 1, NULL);
    yonder_fetched_free(g);
    fresh();
    fake_answer(NULL, "https://a.test/r", 301, "", "moved, nowhere");
    g = run(&(Ask){"https://a.test/page", "https://a.test/r"});
    expect("a redirect with no Location is final", g != NULL && g->ok && g->status == 301 && g->body_len == 14, NULL);
    yonder_fetched_free(g);
}

static void opaque_and_refusals(void)
{
    fresh();
    seed_cookie("https://b.test/", "theirs=1");
    fake_answer(NULL, "https://b.test/pixel", 200, "X-A: 1\r\nSet-Cookie: track=1\r\n", "secret");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/pixel", .mode = WAY_MODE_NO_CORS,
                                     .credentials = WAY_CREDENTIALS_INCLUDE});
    expect("opaque", g != NULL && g->ok && g->type == WAY_TYPE_OPAQUE && g->status == 0 && !g->has_body &&
           g->headers_len == 0 && g->url[0] == '\0' && !g->redirected, NULL);
    expect("opaque: the body never read", fake_reads == 0, NULL);
    expect_text("opaque: include sends the cookie", field(0, "Cookie"), "theirs=1");
    expect_text("opaque: a GET in no-cors has no Origin", field(0, "Origin"), "");
    yonder_fetched_free(g);

    fresh();
    g = run(&(Ask){"https://a.test/page", "http://b.test/api"});
    expect("mixed content", g != NULL && !g->ok && g->mixed && nfake_requests == 0, NULL);
    yonder_fetched_free(g);
    fresh();
    g = run(&(Ask){"https://a.test/page", "https://b.test:25/"});
    expect("a bad port", g != NULL && !g->ok && !g->mixed && nfake_requests == 0, NULL);
    yonder_fetched_free(g);
    fresh();
    g = run(&(Ask){"https://a.test/page", "https://b.test/", .mode = WAY_MODE_SAME_ORIGIN});
    expect("same-origin mode", g != NULL && !g->ok && nfake_requests == 0, NULL);
    yonder_fetched_free(g);
    fresh();
    g = run(&(Ask){"https://a.test/page", "file:///etc/passwd"});
    expect("file:", g != NULL && !g->ok && nfake_requests == 0, NULL);
    yonder_fetched_free(g);
    fresh();
    cancel_now = true;
    fake_answer(NULL, "https://a.test/x", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/x"});
    expect("cancelled", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
}

static void data_and_limits(void)
{
    fresh();
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "data:text/plain;base64,aGVsbG8=#x"});
    expect("data: base64", g != NULL && g->ok && g->type == WAY_TYPE_BASIC && g->status == 200 && g->body_len == 5 &&
           !memcmp(g->body, "hello", 5) && nfake_requests == 0, NULL);
    expect("data: its type", has_line(g, "Content-Type: text/plain\r\n"), g ? g->headers : NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:,a%20b"});
    expect("data: percent-decoded, US-ASCII text", g != NULL && g->ok && g->body_len == 3 && !memcmp(g->body, "a b", 3) &&
           has_line(g, "Content-Type: text/plain;charset=US-ASCII\r\n"), g ? g->headers : NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:;charset=utf-8,x"});
    expect("data: a bare parameter list is text/plain's", g != NULL && g->ok &&
           has_line(g, "Content-Type: text/plain;charset=utf-8\r\n"), g ? g->headers : NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:text/plain;base64,a"});
    expect("data: bad base64", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    // The type is the MIME type serialized whole: every parameter kept.
    g = run(&(Ask){"https://a.test/page", "data:Text/Plain;foo=bar;Q=\"a b\";base64,aGk="});
    expect("data: every parameter kept", g != NULL && g->ok && has_line(g, "Content-Type: text/plain;foo=bar;q=\"a b\"\r\n"),
           g ? g->headers : NULL);
    yonder_fetched_free(g);
    // Fetch's data: URL processor: ";" then zero or more U+0020 SPACE, then
    // "base64" in any case. Spaces only: after a tab the body is not base64.
    g = run(&(Ask){"https://a.test/page", "data:text/plain;  BASE64,aGk="});
    expect("data: spaces before base64", g != NULL && g->ok && g->body_len == 2 && !memcmp(g->body, "hi", 2), NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:text/plain;\tbase64,aGk="});
    expect("data: a tab before base64 is not the marker", g != NULL && g->ok && g->body_len == 4 &&
           !memcmp(g->body, "aGk=", 4), NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:no-comma"});
    expect("data: no comma", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    // A: a data: body past the cap is a network error, never an allocation
    // the size of what the script wrote.
    g = run(&(Ask){"https://a.test/page", "data:,0123456789", .max_body = 9});
    expect("A: data past the cap", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "data:,012345678", .max_body = 9});
    expect("A: data at the cap", g != NULL && g->ok && g->body_len == 9, NULL);
    yonder_fetched_free(g);
    size_t big = YONDER_FETCH_DATA_URL_MAX + 1;
    char *huge = malloc(big + 1);
    memcpy(huge, "data:,", 6);
    memset(huge + 6, 'a', big - 6);
    huge[big] = '\0';
    uint8_t *body;
    size_t len;
    char type[64];
    size_t before = allocations;
    expect("A: data text past its own cap", !yonder_fetch_data_url(huge, YONDER_FETCH_BODY_MAX, &body, &len, type, sizeof(type)) &&
           allocations == before, NULL);
    free(huge);

    // The body's cap on the wire, and the head's.
    fresh();
    fake_answer(NULL, "https://a.test/big", 200, "", "0123456789");
    g = run(&(Ask){"https://a.test/page", "https://a.test/big", .max_body = 9});
    expect("a body past the cap", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "https://a.test/big", .max_body = 10});
    expect("a body at the cap", g != NULL && g->ok && g->body_len == 10, NULL);
    yonder_fetched_free(g);
    static char head[YONDER_FETCH_HEAD_MAX + 64];
    size_t at = 0;
    while (at + 40 < sizeof(head))
        at += (size_t)snprintf(head + at, sizeof(head) - at, "X-Padding-%05zu: 0123456789\r\n", at);
    fresh();
    fake_answer(NULL, "https://a.test/fat", 200, head, "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/fat"});
    expect("a head past 64 KiB", g != NULL && !g->ok && strstr(g->why, "64 KiB") != NULL, g ? g->why : NULL);
    yonder_fetched_free(g);

    // HEAD, 204: no body.
    fresh();
    fake_answer(NULL, "https://a.test/h", 200, "Content-Length: 40\r\n", "would be ignored");
    g = run(&(Ask){"https://a.test/page", "https://a.test/h", "HEAD"});
    expect("HEAD: no body", g != NULL && g->ok && !g->has_body && g->body_len == 0, NULL);
    yonder_fetched_free(g);
    fake_answer(NULL, "https://a.test/n", 204, "", NULL);
    g = run(&(Ask){"https://a.test/page", "https://a.test/n"});
    expect("204: no body", g != NULL && g->ok && !g->has_body, NULL);
    yonder_fetched_free(g);

    // An unreadable ACAO line.
    fresh();
    fake_answer(NULL, "https://b.test/u", 200, "Access-Control-Allow-Origin: *\r\n", "x")->omitted =
        "Access-Control-Allow-Origin\r\n";
    g = run(&(Ask){"https://a.test/page", "https://b.test/u"});
    expect("an unreadable ACAO fails", g != NULL && !g->ok, NULL);
    yonder_fetched_free(g);
}

static void fields(void)
{
    fresh();
    fake_answer(NULL, "https://a.test/x", 206, "", "ab");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://a.test/x", "GET",
                                     "accept: application/json\r\nAccept: text/plain\r\nRange: bytes=0-1\r\n"
                                     "Cookie: forged=1\r\nOrigin: https://evil.test\r\nX-Kept: yes\r\n"});
    expect("fields: ok", g != NULL && g->ok, g ? g->why : NULL);
    expect_text("fields: Accept joined", fake_requests[0].accept, "application/json, text/plain");
    yonder_fetched_free(g);
    // An Accept the page set EMPTY is in the list, so Fetch's */* default
    // does not apply: it goes empty, and joins as "get" joins with another.
    g = run(&(Ask){"https://a.test/page", "https://a.test/x", "GET", "Accept: \r\n"});
    expect("fields: an empty Accept goes empty", g != NULL && g->ok && fake_requests[1].accept_given &&
           fake_requests[1].accept[0] == '\0', fake_requests[1].accept);
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "https://a.test/x", "GET", "Accept: \r\nAccept: a/b\r\n"});
    expect_text("fields: an empty Accept joins", fake_requests[2].accept, ", a/b");
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "https://a.test/x", "GET", "X-Other: 1\r\n"});
    expect_text("fields: no Accept is */*", fake_requests[3].accept, "*/*");
    expect("fields: a Range asks for identity", fake_requests[0].identity, NULL);
    expect("fields: a forbidden header never goes", strstr(fake_requests[0].extra, "forged") == NULL &&
           strstr(fake_requests[0].extra, "evil") == NULL, fake_requests[0].extra);
    expect_text("fields: the rest go", field(0, "X-Kept"), "yes");
    yonder_fetched_free(g);
    // The Referer follows the policy.
    fresh();
    fake_answer(NULL, "https://b.test/x", 200, "Access-Control-Allow-Origin: *\r\n", "x");
    g = run(&(Ask){"https://a.test/page?q=1", "https://b.test/x"});
    expect_text("referer: the origin across origins", field(0, "Referer"), "https://a.test/");
    yonder_fetched_free(g);
    g = run(&(Ask){"https://a.test/page", "https://b.test/x", .policy = WAY_POLICY_NO_REFERRER});
    expect_text("referer: none", field(1, "Referer"), "");
    yonder_fetched_free(g);
}

// ── Every allocation failed in turn ─────────────────────────────────────

static void sweep(void)
{
    size_t n;
    for (n = 1; n < 400; n++) {
        fresh();
        fake_answer("OPTIONS", "https://b.test/api", 204,
                    "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n", NULL);
        fake_answer("PUT", "https://b.test/api", 302, "Location: /final\r\nAccess-Control-Allow-Origin: https://a.test\r\n", NULL);
        fake_answer(NULL, "https://b.test/final", 200, "Access-Control-Allow-Origin: https://a.test\r\nX-A: 1\r\n", "end");
        size_t base = live;
        allocations = 0;
        fail_at = n;
        yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT", NULL, "x"});
        fail_at = 0;
        char name[64];
        snprintf(name, sizeof(name), "sweep %zu: ok or a network error", n);
        expect(name, g == NULL || g->ok || g->type == WAY_TYPE_ERROR, NULL);
        snprintf(name, sizeof(name), "sweep %zu: a whole answer when ok", n);
        expect(name, g == NULL || !g->ok || (g->body_len == 3 && g->url != NULL && g->headers != NULL), NULL);
        // A request that ended in a network error leaves no grant its
        // preflight stored ("clear cache entries using request"), whichever
        // allocation failed: the cache is security state.
        snprintf(name, sizeof(name), "sweep %zu: a failure leaves no preflight grant", n);
        expect(name, (g != NULL && g->ok) || way_preflight_count(cache) == 0, NULL);
        yonder_fetched_free(g);
        snprintf(name, sizeof(name), "sweep %zu: nothing leaked", n);
        expect(name, live == base, NULL);
        if (allocations < n)
            break;
    }
    expect("sweep reached the end", n < 400, NULL);
}

// Each allocation failed ALONE, in a request whose preflight is granted and
// whose answer then fails the CORS check: whichever one failed (the copy of
// a preflighted URL among them), the network error clears every grant the
// preflight stored.
static void sweep_once(void)
{
    size_t n;
    for (n = 1; n < 400; n++) {
        fresh();
        fake_answer("OPTIONS", "https://b.test/api", 204,
                    "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                    "Access-Control-Max-Age: 600\r\n", NULL);
        fake_answer("PUT", "https://b.test/api", 200, "X-No-Grant: 1\r\n", "end");
        size_t base = live;
        allocations = 0;
        fail_at = n;
        fail_once = true;
        yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT", NULL, "x"});
        fail_at = 0;
        fail_once = false;
        char name[80];
        snprintf(name, sizeof(name), "sweep once %zu: a network error", n);
        expect(name, g == NULL || !g->ok, NULL);
        snprintf(name, sizeof(name), "sweep once %zu: no preflight grant left", n);
        expect(name, way_preflight_count(cache) == 0, NULL);
        yonder_fetched_free(g);
        snprintf(name, sizeof(name), "sweep once %zu: nothing leaked", n);
        expect(name, live == base, NULL);
        if (allocations < n)
            break;
    }
    expect("sweep once reached the end", n < 400, NULL);
}

// ── Who is asking ───────────────────────────────────────────────────────

static const way_identity_t kWho = {.hints = "sec-ch-ua: \"Fixture\";v=\"1\"\r\n", .language = "en-US,en;q=0.9",
                                    .priority = {[WAY_DEST_EMPTY] = "u=4"}};

static void who_is_asking(void)
{
    // fetch() to its own origin: cors, empty, the preset's lines with it.
    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://a.test/api", .identity = &kWho});
    expect("who: ok", g != NULL && g->ok, g ? g->why : NULL);
    expect_text("who: same-origin site", field(0, "Sec-Fetch-Site"), "same-origin");
    expect_text("who: cors mode", field(0, "Sec-Fetch-Mode"), "cors");
    expect_text("who: empty dest", field(0, "Sec-Fetch-Dest"), "empty");
    expect_text("who: no user on a script's request", field(0, "Sec-Fetch-User"), "");
    expect_text("who: no upgrade on a script's request", field(0, "Upgrade-Insecure-Requests"), "");
    expect_text("who: the preset's hints", field(0, "sec-ch-ua"), "\"Fixture\";v=\"1\"");
    expect_text("who: the preset's language", field(0, "Accept-Language"), "en-US,en;q=0.9");
    expect_text("who: the preset's priority for an empty destination", field(0, "Priority"), "u=4");
    yonder_fetched_free(g);

    // The modes a page may name.
    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api", .mode = WAY_MODE_SAME_ORIGIN});
    expect_text("who: same-origin mode", field(0, "Sec-Fetch-Mode"), "same-origin");
    yonder_fetched_free(g);
    fake_answer(NULL, "https://b.test/pixel", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://b.test/pixel", .mode = WAY_MODE_NO_CORS});
    expect_text("who: no-cors mode", field(1, "Sec-Fetch-Mode"), "no-cors");
    expect_text("who: and cross-site", field(1, "Sec-Fetch-Site"), "cross-site");
    yonder_fetched_free(g);

    // A preflight carries cors and empty, the request's site, the
    // language, and no client hints; the request after it carries them.
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                "Access-Control-Allow-Headers: x-w1\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "done");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT", "X-W1: 1\r\n", .identity = &kWho});
    expect("who: the preflighted PUT", g != NULL && g->ok && nfake_requests == 2 &&
           !strcmp(fake_requests[0].method, "OPTIONS"), g ? g->why : NULL);
    expect_text("who: preflight mode", field(0, "Sec-Fetch-Mode"), "cors");
    expect_text("who: preflight dest", field(0, "Sec-Fetch-Dest"), "empty");
    expect_text("who: preflight site", field(0, "Sec-Fetch-Site"), "cross-site");
    expect_text("who: preflight language", field(0, "Accept-Language"), "en-US,en;q=0.9");
    expect_text("who: no hints with a preflight", field(0, "sec-ch-ua"), "");
    expect_text("who: a preflight's priority", field(0, "Priority"), "u=4");
    expect_text("who: the request after it has them", field(1, "sec-ch-ua"), "\"Fixture\";v=\"1\"");
    expect_text("who: and its site", field(1, "Sec-Fetch-Site"), "cross-site");
    yonder_fetched_free(g);

    // A redirect to another site: that hop, and the URL list after it.
    fresh();
    fake_answer(NULL, "https://a.test/r", 302, "Location: https://b.test/final\r\n", NULL);
    fake_answer(NULL, "https://b.test/final", 302,
                "Access-Control-Allow-Origin: *\r\nLocation: https://a.test/back\r\n", NULL);
    fake_answer(NULL, "https://a.test/back", 200, "Access-Control-Allow-Origin: *\r\n", "end");
    g = run(&(Ask){"https://a.test/page", "https://a.test/r"});
    expect("who: the redirects followed", g != NULL && g->ok && nfake_requests == 3, g ? g->why : NULL);
    expect_text("who: the first hop is same-origin", field(0, "Sec-Fetch-Site"), "same-origin");
    expect_text("who: the hop that leaves is cross-site", field(1, "Sec-Fetch-Site"), "cross-site");
    expect_text("who: and back on the origin it stays so", field(2, "Sec-Fetch-Site"), "cross-site");
    yonder_fetched_free(g);

    // Plain http hears none of the metadata or the hints.
    fresh();
    fake_answer(NULL, "http://a.test/api", 200, "", "x");
    g = run(&(Ask){"http://a.test/page", "http://a.test/api", .identity = &kWho});
    expect_text("who: plain http, no site", field(0, "Sec-Fetch-Site"), "");
    expect_text("who: plain http, no hints", field(0, "sec-ch-ua"), "");
    expect_text("who: plain http, the language", field(0, "Accept-Language"), "en-US,en;q=0.9");
    yonder_fetched_free(g);

    // A page's own Priority goes alone, as its Accept-Language does: the
    // preset's beside it would be a duplicate, which libfetch refuses.
    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api", "GET", "Priority: u=1\r\n", .identity = &kWho});
    expect("who: a page's Priority", g != NULL && g->ok, g ? g->why : NULL);
    expect_text("who: the page's Priority goes", field(0, "Priority"), "u=1");
    expect("who: and the preset's does not", strstr(fake_requests[0].fields, "Priority") == NULL,
           fake_requests[0].fields);
    yonder_fetched_free(g);

    // With a preflight: it carries none of the page's lines, so the
    // preset's Accept-Language and Priority go with it; the request after
    // it carries the page's alone.
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n"
                "Access-Control-Allow-Headers: priority\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "done");
    g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT",
                   "Accept-Language: fr\r\nPriority: u=1\r\n", .identity = &kWho});
    expect("who: a preflighted request with the page's own lines", g != NULL && g->ok && nfake_requests == 2,
           g ? g->why : NULL);
    expect_text("who: the preflight's language is the preset's", field(0, "Accept-Language"), "en-US,en;q=0.9");
    expect_text("who: the preflight's priority is the preset's", field(0, "Priority"), "u=4");
    expect_text("who: the request's language is the page's", field(1, "Accept-Language"), "fr");
    expect_text("who: the request's priority is the page's", field(1, "Priority"), "u=1");
    expect("who: and the preset's lines are not beside them",
           strstr(fake_requests[1].fields, "Accept-Language") == NULL &&
           strstr(fake_requests[1].fields, "Priority") == NULL, fake_requests[1].fields);
    yonder_fetched_free(g);

    // A page's own Accept-Language goes alone; a page cannot write the
    // browser's lines, nor a Sec- or Proxy- header of any name.
    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api", "GET",
                   "accept-language: fr\r\nSec-Fetch-User: ?1\r\nSec-Fetch-Site: none\r\nsec-ch-ua: forged\r\n"
                   "Upgrade-Insecure-Requests: 1\r\nProxy-Thing: x\r\nSec-Anything: x\r\n",
                   .identity = &kWho});
    expect("who: a page's headers", g != NULL && g->ok, g ? g->why : NULL);
    expect_text("who: the page's language", field(0, "accept-language"), "fr");
    expect("who: and only the page's", strstr(fake_requests[0].fields, "Accept-Language") == NULL,
           fake_requests[0].fields);
    expect("who: no forged Sec- or Proxy- line goes",
           strstr(fake_requests[0].extra, "Sec-") == NULL && strstr(fake_requests[0].extra, "sec-") == NULL &&
           strstr(fake_requests[0].extra, "Proxy-") == NULL, fake_requests[0].extra);
    expect_text("who: the browser's site stands", field(0, "Sec-Fetch-Site"), "same-origin");
    expect_text("who: no user from a script", field(0, "Sec-Fetch-User"), "");
    expect_text("who: the browser's hints stand", field(0, "sec-ch-ua"), "\"Fixture\";v=\"1\"");
    yonder_fetched_free(g);
}

// ── How it asks: the preset's order, Connection line and fetch Accept ───

static const way_identity_t kAsks = {
    .keep_alive = true, .accept = {[WAY_DEST_EMPTY] = "*/*;q=0.9"},
    .order = {[WAY_ORDER_NO_CORS] = "Accept", [WAY_ORDER_CORS] = "Origin, Accept",
              [WAY_ORDER_PREFLIGHT] = "Access-Control-Request-Method, Accept"},
};

static void how_it_asks(void)
{
    // A preflighted request: the preflight in the preset's preflight
    // order, the request in its cors order, both saying keep-alive; the
    // preset's fetch Accept stands in for Fetch's `*/*` on the request.
    fresh();
    fake_answer("OPTIONS", "https://b.test/api", 204,
                "Access-Control-Allow-Origin: https://a.test\r\nAccess-Control-Allow-Methods: PUT\r\n", NULL);
    fake_answer("PUT", "https://b.test/api", 200, "Access-Control-Allow-Origin: https://a.test\r\n", "done");
    yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://b.test/api", "PUT", .identity = &kAsks});
    expect("asks: the preflighted PUT", g != NULL && g->ok && nfake_requests == 2, g ? g->why : NULL);
    expect_text("asks: the preflight's order", fake_requests[0].order, "Access-Control-Request-Method, Accept");
    expect_text("asks: the request's order", fake_requests[1].order, "Origin, Accept");
    expect("asks: both say keep-alive", fake_requests[0].keep_alive && fake_requests[1].keep_alive, NULL);
    expect_text("asks: a preflight's Accept is Fetch's own", fake_requests[0].accept, "*/*");
    expect_text("asks: the request's is the preset's", fake_requests[1].accept, "*/*;q=0.9");
    yonder_fetched_free(g);

    // no-cors takes the no-cors order; a page's own Accept stands.
    fresh();
    fake_answer(NULL, "https://b.test/pixel", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://b.test/pixel", "GET", "Accept: text/x-page\r\n",
                   .mode = WAY_MODE_NO_CORS, .identity = &kAsks});
    expect("asks: a no-cors fetch", g != NULL && g->ok, g ? g->why : NULL);
    expect_text("asks: its order", fake_requests[0].order, "Accept");
    expect_text("asks: the page's Accept stands", fake_requests[0].accept, "text/x-page");
    yonder_fetched_free(g);

    // No preset: no order, no Connection line, Fetch's `*/*`.
    fresh();
    fake_answer(NULL, "https://a.test/api", 200, "", "x");
    g = run(&(Ask){"https://a.test/page", "https://a.test/api"});
    expect("asks: no preset", g != NULL && g->ok && fake_requests[0].order[0] == '\0' &&
                                  !fake_requests[0].keep_alive, g ? g->why : NULL);
    expect_text("asks: and Fetch's own Accept", fake_requests[0].accept, "*/*");
    yonder_fetched_free(g);
}

// A script's request gets the Cookie line a page's own hop gets
// (WAY_COOKIE_LINE_MAX), the newest cookies when they do not all fit, and
// the product says how many were left out, and for which host.
static void cookie_room(void)
{
    static char line[16384];
    char cookie[300], value[241];
    memset(value, 'v', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    for (int many = 0; many < 2; many++) {
        fresh();
        int n = many ? 40 : 12;                 // 10 KB, or 3 KB
        for (int i = 0; i < n; i++) {
            snprintf(cookie, sizeof(cookie), "pre%02d=%s; Path=/", i, value);
            seed_cookie("https://a.test/", cookie);
        }
        seed_cookie("https://a.test/", "user_session=SESSION; Path=/; Secure; HttpOnly");
        fake_answer(NULL, "https://a.test/api", 200, "", "x");
        yonder_fetched_t *g = run(&(Ask){"https://a.test/page", "https://a.test/api"});
        const char *sent = fake_field(0, "Cookie", line, sizeof(line));
        expect(many ? "cookie room: past it, the login goes and the rest is counted"
                    : "cookie room: 3 KB and the login go whole",
               g != NULL && g->ok && strstr(sent, "user_session=SESSION") != NULL &&
                   (strstr(sent, "pre00=") != NULL) == !many && strlen(sent) + 11 <= WAY_COOKIE_LINE_MAX &&
                   g->cookies_short.left == (many ? 8 : 0) &&
                   strcmp(g->cookies_short.host, many ? "a.test" : "") == 0,
               g != NULL ? g->cookies_short.host : NULL);
        yonder_fetched_free(g);
    }
}

static void native_images(void)
{
    for(unsigned cors=0;cors<3;++cors) {
        fresh();
        fake_answer(NULL,"https://b.test/pixel",200,cors==1?"Access-Control-Allow-Origin: https://a.test\r\n":"","pixels");
        yonder_fetch_job_t *j=job_of(&(Ask){"https://a.test/page","https://b.test/pixel",
            .mode=cors?WAY_MODE_CORS:WAY_MODE_NO_CORS});
        void *out=NULL;yonder_fetch_image_run(j,cancel_cb,NULL,&out);yonder_fetched_t *g=out;
        expect("native images: CORS refusal remains a refusal",g&&g->ok==(cors!=2),g?g->why:NULL);
        if(g&&g->ok)expect("native images: retained pixels carry the taint verdict",
            g->has_body&&g->body_len==6&&!memcmp(g->body,"pixels",6)&&
            g->type==(cors?WAY_TYPE_CORS:WAY_TYPE_OPAQUE),NULL);
        yonder_fetch_release(j,g);
    }
    fresh();fake_answer(NULL,"https://a.test/start",302,"Location: https://b.test/pixel\r\n","");
    fake_answer(NULL,"https://b.test/pixel",200,"","pixels");
    yonder_fetch_job_t *j=job_of(&(Ask){"https://a.test/page","https://a.test/start",.mode=WAY_MODE_NO_CORS});
    void *out=NULL;yonder_fetch_image_run(j,cancel_cb,NULL,&out);yonder_fetched_t *g=out;
    expect("native images: redirect taints even a same-origin starting URL",g&&g->ok&&g->type==WAY_TYPE_OPAQUE&&g->body_len==6,NULL);
    yonder_fetch_release(j,g);
}

int main(void)
{
    cookie_room();
    who_is_asking();
    how_it_asks();
    same_origin();
    cross_origin();
    preflights();
    preflight_cleared_across_a_taint();
    redirects();
    opaque_and_refusals();
    native_images();
    data_and_limits();
    fields();
    sweep();
    sweep_once();
    way_jar_free(jar);
    way_preflight_free(cache);
    expect("nothing leaked", live == 0, NULL);
    printf("fetch job: %d checks, %d failed\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
