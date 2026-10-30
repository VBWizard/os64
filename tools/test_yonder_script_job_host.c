/* test_yonder_script_job_host.c — a `src` script's worker (script_job.c) on
 * the host, under the sanitizers, against a fake fetch and a fake cache:
 * the scripted-page suites stand a fake in for this worker, so its own
 * rules are held here (BYTECODE_CACHE.md § 5, § 6). A body the cache holds
 * has its compiled form looked for under the address asked for and the
 * source's key, and gets a record a compile's form is kept under; one it
 * does not hold gets neither. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_job.h"
#include "os64/slurp.h"

static int checks, failures;
static size_t live;

static void expect(const char *name, bool ok)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

void *os64_malloc(size_t size)
{
    void *at = malloc(size != 0 ? size : 1);
    live += at != NULL;
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
    void *at = realloc(ptr, size != 0 ? size : 1);
    live += at != NULL && ptr == NULL;
    return at;
}
void os64_free(void *ptr)
{
    live -= ptr != NULL;
    free(ptr);
}

os64_slurp_status_t os64_slurp(const char *path, size_t max, uint8_t **out, size_t *len)
{
    (void)path; (void)max; (void)out; (void)len;
    return OS64_SLURP_NO_FILE;
}

/* Decoding is not what is held here (script_decode.c is the window's suites'
 * too): every label is UTF-8's, and nothing is printed. */
const char *os64_html_encoding_for_label(const char *label, size_t len)
{
    (void)label; (void)len;
    return "UTF-8";
}
int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    (void)handle; (void)buf;
    return (int64_t)len;
}

/* The fetch: this body, the cache holding it or not. */
static const char *s_body;
static bool s_kept;
bool way_fetch_whole(way_hooks_t *hooks, const char *url, os64_fetch_options_t *opt, size_t cap,
                     way_whole_t *out)
{
    (void)hooks; (void)opt; (void)cap;
    memset(out, 0, sizeof(*out));
    size_t n = strlen(s_body);
    out->bytes = os64_malloc(n);
    memcpy(out->bytes, s_body, n);
    out->len = n;
    out->status = 200;
    snprintf(out->url, sizeof(out->url), "%s", url);
    snprintf(out->charset, sizeof(out->charset), "utf-8");
    out->kept = s_kept;
    return true;
}

/* The cache's companions: what was asked for, and what is there. */
static int s_gets;
static char s_got_url[OS64_FETCH_URL_MAX], s_got_key[WAY_COMPANION_KEY_MAX];
static const char *s_companion;
bool way_cache_companion_get(way_cache_t *cache, const char *url, const char *key, uint8_t **bytes,
                             size_t *len, size_t cap)
{
    (void)cache;
    s_gets++;
    snprintf(s_got_url, sizeof(s_got_url), "%s", url);
    snprintf(s_got_key, sizeof(s_got_key), "%s", key);
    *bytes = NULL;
    *len = 0;
    if (s_companion == NULL || strlen(s_companion) > cap)
        return false;
    *len = strlen(s_companion);
    *bytes = os64_malloc(*len);
    memcpy(*bytes, s_companion, *len);
    return true;
}
static int s_puts;
bool way_cache_companion_put(way_cache_t *cache, const char *url, const char *key, const uint8_t *bytes,
                             size_t len)
{
    (void)cache; (void)url; (void)key; (void)bytes; (void)len;
    s_puts++;
    return true;
}

static bool never(void *ctx)
{
    (void)ctx;
    return false;
}

static yonder_script_t *run(const char *url)
{
    yonder_script_job_t *job = os64_calloc(1, sizeof(*job));
    job->kind = YONDER_JOB_SCRIPT;
    snprintf(job->url, sizeof(job->url), "%s", url);
    job->hooks.cache = (way_cache_t *)&s_gets;
    void *out = NULL;
    yonder_script_run(job, never, NULL, &out);
    os64_free(job);
    return out;
}

static void release(yonder_script_t *got)
{
    yonder_script_release(os64_calloc(1, sizeof(yonder_script_job_t)), got);
}

int main(void)
{
    // The key names the text: the same text one key, another text or
    // another length another.
    char a[WAY_COMPANION_KEY_MAX], b[WAY_COMPANION_KEY_MAX], c[WAY_COMPANION_KEY_MAX];
    yonder_script_bytecode_key("var a = 1;", 10, a, sizeof(a));
    yonder_script_bytecode_key("var a = 1;", 10, b, sizeof(b));
    yonder_script_bytecode_key("var a = 2;", 10, c, sizeof(c));
    expect("key: the same text has one key", strcmp(a, b) == 0);
    expect("key: another text another", strcmp(a, c) != 0);
    yonder_script_bytecode_key("var a = 1;", 9, c, sizeof(c));
    expect("key: another length another", strcmp(a, c) != 0 && strncmp(a, "classic ", 8) == 0);

    // A body the cache holds: its companion looked for under the address
    // asked for and the decoded text's key, and found.
    s_body = "document.title = 'x';";
    s_kept = true;
    s_companion = "a blob";
    yonder_script_t *got = run("http://x.test/app.js");
    char key[WAY_COMPANION_KEY_MAX];
    yonder_script_bytecode_key(s_body, strlen(s_body), key, sizeof(key));
    expect("kept: the script came", got != NULL && got->ok && strcmp(got->source, s_body) == 0);
    expect("kept: its companion was looked for under the address and the text's key",
           s_gets == 1 && strcmp(s_got_url, "http://x.test/app.js") == 0 && strcmp(s_got_key, key) == 0);
    expect("kept: and carried, with where a compile's is kept",
           got != NULL && got->bytecode != NULL && got->bytecode->length == 6 &&
               memcmp(got->bytecode->blob, "a blob", 6) == 0 &&
               strcmp(got->bytecode->url, "http://x.test/app.js") == 0 && strcmp(got->bytecode->key, key) == 0);
    release(got);

    // None beside it: the record is made all the same, for the compile's.
    s_companion = NULL;
    got = run("http://x.test/app.js");
    expect("kept, none beside it: a record with no blob",
           got != NULL && got->bytecode != NULL && got->bytecode->blob == NULL && s_gets == 2);
    release(got);

    // A body the cache does not hold: nothing looked for, nothing carried.
    s_kept = false;
    s_companion = "a blob";
    got = run("http://x.test/app.js");
    expect("not kept: no companion is looked for, and no record is made",
           got != NULL && got->ok && got->bytecode == NULL && s_gets == 2);
    release(got);

    // The write job writes what it holds, and its release frees it.
    yonder_bytecode_job_t *job = os64_calloc(1, sizeof(*job));
    job->kind = YONDER_JOB_BYTECODE;
    job->blob = os64_malloc(4);
    job->length = 4;
    void *none = NULL;
    expect("write: the job keeps its blob", yonder_bytecode_keep_run(job, never, NULL, &none) == 1 && s_puts == 1);
    yonder_bytecode_keep_release(job, NULL);

    expect("everything let go", live == 0);
    printf("yonder script job: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
