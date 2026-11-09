// test_webfont_host.c — yonder's font job (webfont.c) on the host, under
// the sanitizers: a file read or fetched, judged by its signature, made a
// plain SFNT or refused with its reason, and every byte let go. The fetch
// and the file read are stubbed: the cases say what each one answers.
// Usage: test_webfont_host FONT.ttf FONT.woff

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webfont.h"
#include "os64/slurp.h"

static int failures, checks;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s: %s\n", what, #x); } } while (0)

static size_t allocations, fail_at, live;
void *os64_malloc(size_t size)
{
    allocations++;
    if (fail_at != 0 && allocations == fail_at)
        return NULL;
    void *p = malloc(size + 16);
    if (p == NULL)
        return NULL;
    live++;
    return (char *)p + 16;
}
void *os64_calloc(size_t n, size_t size)
{
    void *p = os64_malloc(n * size);
    if (p != NULL)
        memset(p, 0, n * size);
    return p;
}
void os64_free(void *p)
{
    if (p != NULL) {
        live--;
        free((char *)p - 16);
    }
}

// libos64's formatter prints through this; nothing here prints with it.
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle;
    (void)buf;
    return (int64_t)len;
}

// What the stubs answer.
static const uint8_t *served;
static size_t served_len;
static int32_t served_status;
static bool fetch_fails;
static bool cancel_now;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)len);
    if (b == NULL || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return b;
}

os64_slurp_status_t os64_slurp(const char *path, size_t cap, uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    size_t n;
    uint8_t *b = slurp(path, &n);
    if (b == NULL)
        return OS64_SLURP_NO_FILE;
    if (n > cap) {
        free(b);
        return OS64_SLURP_TOO_BIG;
    }
    *out = os64_malloc(n);
    if (*out == NULL) {
        free(b);
        return OS64_SLURP_NO_MEMORY;
    }
    memcpy(*out, b, n);
    free(b);
    *len = n;
    return OS64_SLURP_OK;
}

bool way_fetch_whole(way_hooks_t *hooks, const char *url, os64_fetch_options_t *opt, size_t cap,
                     way_whole_t *out)
{
    (void)hooks;
    (void)url;
    (void)opt;
    memset(out, 0, sizeof(*out));
    if (fetch_fails || served_len > cap) {
        out->fetch = OS64_FETCH_REQUEST_FAILED;
        return false;
    }
    out->status = served_status;
    if (served_len > 0) {
        out->bytes = os64_malloc(served_len);
        if (out->bytes == NULL) {
            out->fetch = OS64_FETCH_NO_MEMORY;
            return false;
        }
        memcpy(out->bytes, served, served_len);
    }
    out->len = served_len;
    return true;
}

const char *os64_fetch_status_name(os64_fetch_status_t status)
{
    return status == OS64_FETCH_REQUEST_FAILED ? "request-failed" : "other";
}

static bool cancelled(void *ctx)
{
    (void)ctx;
    return cancel_now;
}

// The job run once for `url`, the answer checked against what is wanted,
// and both let go.
static void run(const char *what, const char *url, bool ok, const char *why, yonder_font_kind_t container,
                const uint8_t *sfnt, size_t sfnt_len)
{
    yonder_webfont_job_t *job = os64_calloc(1, sizeof(*job));
    job->kind = YONDER_JOB_FONT;
    snprintf(job->url, sizeof(job->url), "%s", url);
    void *out = NULL;
    int64_t verdict = yonder_webfont_run(job, cancelled, NULL, &out);
    yonder_webfont_t *f = out;
    CHECK(f != NULL && verdict == (ok ? 1 : 0), what);
    if (f != NULL) {
        CHECK(f->ok == ok && (why == NULL ? f->why[0] == '\0' : strcmp(f->why, why) == 0), what);
        if (why != NULL && strcmp(f->why, why) != 0)
            fprintf(stderr, "  %s: why \"%s\"\n", what, f->why);
        CHECK(!ok || f->container == container, what);
        if (sfnt != NULL)
            CHECK(f->len == sfnt_len && memcmp(f->sfnt, sfnt, sfnt_len) == 0, what);
        CHECK(ok == (f->sfnt != NULL), what);
    }
    yonder_webfont_release(job, out);
    CHECK(live == 0, what);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    size_t ttf_len, woff_len;
    uint8_t *ttf = slurp(argv[1], &ttf_len), *woff = slurp(argv[2], &woff_len);
    if (ttf == NULL || woff == NULL)
        return 2;
    char file[4096];
    snprintf(file, sizeof(file), "file://%s", argv[1]);
    run("a file read beside the page", file, true, NULL, YONDER_FONT_SFNT, ttf, ttf_len);
    run("a file that is not there", "file:///nowhere/at/all.ttf", false, "could not be read", 0, NULL, 0);
    served = ttf, served_len = ttf_len, served_status = 200;
    run("an SFNT fetched", "https://fonts.test/a.ttf", true, NULL, YONDER_FONT_SFNT, ttf, ttf_len);
    served = woff, served_len = woff_len;
    run("a WOFF fetched and unwrapped", "https://fonts.test/a.woff", true, NULL, YONDER_FONT_WOFF, NULL, 0);
    // The unwrapped SFNT holds the font's own tables: its signature, and its
    // first table's tag where the original's is.
    {
        yonder_webfont_job_t *job = os64_calloc(1, sizeof(*job));
        snprintf(job->url, sizeof(job->url), "https://fonts.test/a.woff");
        void *out = NULL;
        yonder_webfont_run(job, cancelled, NULL, &out);
        yonder_webfont_t *f = out;
        CHECK(f->ok && f->len >= 28 && yonder_font_sniff(f->sfnt, f->len) == YONDER_FONT_SFNT &&
              memcmp(f->sfnt + 12, ttf + 12, 4) == 0, "the WOFF's SFNT is the font's");
        yonder_webfont_release(job, out);
    }
    uint8_t broken[64];
    memcpy(broken, woff, sizeof(broken));
    broken[14] = 1;                 // reserved, which must be zero
    served = broken, served_len = sizeof(broken);
    run("a broken WOFF", "https://fonts.test/b.woff", false, "a broken WOFF (malformed container)", 0, NULL, 0);
    static const uint8_t woff2[] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    served = woff2, served_len = sizeof(woff2);
    run("a WOFF2", "https://fonts.test/a.woff2", false, "a WOFF2, not read yet", 0, NULL, 0);
    static const uint8_t html[] = "<!doctype html><title>404</title>";
    served = html, served_len = sizeof(html) - 1;
    run("not a font", "https://fonts.test/a.ttf", false, "not a font", 0, NULL, 0);
    served_status = 404;
    run("an error's body", "https://fonts.test/a.ttf", false, "HTTP 404", 0, NULL, 0);
    served_status = 200, served_len = 0;
    run("an empty reply", "https://fonts.test/a.ttf", false, "an empty reply", 0, NULL, 0);
    fetch_fails = true;
    run("a fetch that failed", "https://fonts.test/a.ttf", false, "did not fetch (request-failed)", 0, NULL, 0);
    fetch_fails = false;
    served = ttf, served_len = ttf_len, cancel_now = true;
    run("a fetch cancelled", "https://fonts.test/a.ttf", false, "cancelled", 0, NULL, 0);
    cancel_now = false;
    // Every allocation refused in turn: the job ends, answering whole or
    // with a reason, and nothing leaks.
    for (int k = 0; k < 2; k++) {
        served = k ? woff : ttf, served_len = k ? woff_len : ttf_len;
        for (size_t at = 1; at < 40; at++) {
            yonder_webfont_job_t *job = os64_calloc(1, sizeof(*job));
            snprintf(job->url, sizeof(job->url), "https://fonts.test/x");
            allocations = 0;
            fail_at = at;
            void *out = NULL;
            int64_t verdict = yonder_webfont_run(job, cancelled, NULL, &out);
            fail_at = 0;
            yonder_webfont_t *f = out;
            CHECK(verdict == -1 ? f == NULL : f != NULL && (f->ok || f->why[0] != '\0'),
                  "an allocation refused ends the job with a reason");
            yonder_webfont_release(job, out);
            CHECK(live == 0, "an allocation refused leaks nothing");
        }
    }
    free(ttf);
    free(woff);
    printf("webfont: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
