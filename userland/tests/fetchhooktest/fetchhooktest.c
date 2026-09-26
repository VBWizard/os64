// Explicit network fixture: run against tools/httptestd.py, not the public
// internet. This is a fixed test cookie handoff, not a cookie jar.
#include "os64/os64.h"
#include "fetch/fetch.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { os64_printf("fetchhooktest: FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
typedef struct { unsigned received, asked, hops; bool session; } state_t;

static void got_cookie(void *ctx, const os64_url_t *url, bool encrypted, const char *value, size_t len)
{
    state_t *state = ctx;
    CHECK(os64_streq(url->path, "/set-cookie"));
    CHECK(!encrypted);
    const char *want = state->received == 0 ? "session=donuts; Path=/; HttpOnly" : "taste=chocolate; Path=/";
    CHECK(len == os64_strlen(want) && os64_memcmp(value, want, len) == 0);
    state->received++;
    state->session = true;
}
static bool headers(void *ctx, const os64_url_t *url, bool encrypted, char *out, size_t cap)
{
    state_t *state = ctx;
    state->asked++;
    CHECK(!encrypted);
    if (os64_streq(url->path, "/needs-cookie")) {
        CHECK(state->received == 2 && state->hops == 1 && state->session);
        return os64_strcopy(out, cap, "Cookie: session=donuts\r\n") < cap;
    }
    return os64_strcopy(out, cap, "Referer: https://private.test/account\r\n") < cap;
}
static os64_fetch_verdict_t hop(void *ctx, const os64_fetch_hop_t *h)
{
    state_t *state = ctx;
    CHECK(h->status == 302 && state->received == 2);
    state->hops++;
    return OS64_FETCH_HOP_DEFAULT;
}
static void fetch(const char *base, const char *path, const os64_fetch_options_t *opt,
                  int status, const char *want)
{
    char url[OS64_FETCH_URL_MAX];
    int32_t n = os64_snprintf(url, sizeof(url), "%s%s", base, path);
    CHECK(n > 0 && (size_t)n < sizeof(url));
    if (n <= 0 || (size_t)n >= sizeof(url)) return;
    os64_fetch_t *f = os64_fetch_open(url, opt);
    CHECK(f != NULL);
    if (!f) return;
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    const os64_fetch_head_t *head = os64_fetch_head(f);
    CHECK(head && head->status == status);
    char buf[512]; size_t used = 0;
    while (used < sizeof(buf)) {
        int64_t got = os64_fetch_read(f, buf + used, sizeof(buf) - used);
        if (got <= 0) break;
        used += (size_t)got;
    }
    CHECK(os64_fetch_status(f) == OS64_FETCH_OK);
    CHECK(used == os64_strlen(want) && os64_memcmp(buf, want, used) == 0);
    os64_fetch_close(f);
}
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && !os64_streq(argv[2], "--proxy"))) {
        os64_printf("usage: fetchhooktest URL [--proxy] (tools/httptestd.py; plain proxy only)\n");
        return 2;
    }
    state_t state = {0};
    os64_fetch_options_t opt = { .headers_for = headers, .on_set_cookie = got_cookie,
        .on_hop = hop, .ctx = &state, .no_proxy = argc == 2, .idle_ms = 3000 };
    fetch(argv[1], "/set-cookie", &opt, 200, "cookie accepted\n");
    CHECK(state.asked == 2 && state.received == 2 && state.hops == 1);
    // The two source forms of Referer are both stripped on this plain wire.
    opt.on_set_cookie = NULL; opt.on_hop = NULL;
    fetch(argv[1], "/referer", &opt, 200, "\n");
    opt.headers_for = NULL;
    opt.extra_headers = "Referer: https://private.test/account\r\n";
    fetch(argv[1], "/referer", &opt, 200, "\n");
    opt.extra_headers = NULL;
    fetch(argv[1], "/needs-cookie", &opt, 403, "cookie missing\n");
    opt.method = OS64_FETCH_METHOD_POST; opt.body = "a=1"; opt.body_len = 3;
    opt.content_type = "application/x-www-form-urlencoded";
    fetch(argv[1], "/303-after-post", &opt, 200, "method=GET\ncontent-type=\nbytes=0\n");
    fetch(argv[1], "/307-after-post", &opt, 200,
          "method=POST\ncontent-type=application/x-www-form-urlencoded\nbytes=3\na=1");
    os64_printf("fetchhooktest: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
