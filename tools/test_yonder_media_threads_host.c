/* A REFUSED THREAD START IN YONDER'S MEDIA HOST (apps/yonder/media.c),
 * on the host: the session's own code, with the threads, the player, the
 * fetch and libos64 beneath it stubbed, so a start can be refused at will.
 *
 * A refused start makes no thread (the kernel reserves a thread's handle
 * before it makes the thread), so each one is undone at once and the
 * session is freed by the reap that follows its close, never held until
 * yonder's end:
 *   1. the player's own threads refused (os64_media_player_create fails):
 *      the open answers NULL and the next reap frees the session;
 *   2. the loader refused (a network source's prepare): the session fails
 *      at once with a network error, its loader slot is given back, and the
 *      reap after its close frees it;
 *   3. the plane's helper refused: the ask answers false, and the reap
 *      after the close frees the session; and, for contrast, a helper that
 *      started and has not said it is done keeps its session until it does.
 *
 *   tools/test_yonder_media_threads_host.sh */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../userland/apps/yonder/media.c"

static int checks, failures;
static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* ── libos64, beneath ─────────────────────────────────────────────────── */

void *os64_malloc(size_t n) { return malloc(n); }
void *os64_calloc(size_t n, size_t size) { return calloc(n, size); }
void *os64_realloc(void *p, size_t n) { return realloc(p, n); }
void os64_free(void *p) { free(p); }
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *os64_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t os64_strlen(const char *s) { return strlen(s); }
bool os64_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
size_t os64_strcopy(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (cap > 0) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, k);
        d[k] = '\0';
    }
    return n;
}
int os64_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
int64_t os64_micros(void) { static int64_t now = 1000; return now += 1000; }
int64_t os64_sleep(uint64_t ms) { (void)ms; return 0; }
void os64_yield(void) {}
int64_t os64_open(const char *path, const char *mode) { (void)path; (void)mode; return -1; }
int64_t os64_close(int32_t handle) { (void)handle; return 0; }
int64_t os64_bell_make(os64_bell_t *bell) { bell->handle = 7; return 0; }
void os64_bell_close(os64_bell_t *bell) { bell->handle = -1; }
int64_t os64_bell_ring(os64_bell_t *bell) { (void)bell; return 0; }
int64_t os64_bell_wait(os64_bell_t *bell, uint64_t ms) { (void)bell; (void)ms; return 0; }

/* The thread start: refused, or "started" (the function is kept, not run,
 * so the test says when it has run). */
static bool s_refuse_threads;
static int s_started;
static int64_t (*s_kept_fn)(void *);
static void *s_kept_arg;
int64_t os64_thread_named(int64_t (*fn)(void *), void *arg, const char *name)
{
    (void)name;
    if (s_refuse_threads)
        return -1;
    s_started++;
    s_kept_fn = fn;
    s_kept_arg = arg;
    return 100 + s_started;
}

/* ── the player and the output ────────────────────────────────────────── */

static bool s_refuse_player;
static os64_media_player_config_t s_cfg;
static int s_players;
static int s_dummy_output, s_dummy_player;
int os64_media_output_create(os64_media_output_t **out) { *out = (os64_media_output_t *)&s_dummy_output; return 0; }
int os64_media_output_destroy(os64_media_output_t *o) { (void)o; return 0; }
int os64_media_player_create(const os64_media_player_config_t *cfg, os64_media_player_t **out)
{
    if (s_refuse_player)
        return OS64_MEDIA_NO_MEMORY;
    s_cfg = *cfg;
    s_players++;
    *out = (os64_media_player_t *)&s_dummy_player;
    return 0;
}
/* A close the player carries out at once: it will never read again. */
void os64_media_player_close(os64_media_player_t *p)
{
    (void)p;
    s_players--;
    if (s_cfg.finished != NULL)
        s_cfg.finished(s_cfg.ctx);
}
void os64_media_player_facts(os64_media_player_t *p, os64_media_player_facts_t *out) { (void)p; memset(out, 0, sizeof(*out)); }
bool os64_media_player_frame(os64_media_player_t *p, int64_t blank, int64_t period, os64_media_player_frame_t *out)
{ (void)p; (void)blank; (void)period; memset(out, 0, sizeof(*out)); return false; }
void os64_media_player_gain(os64_media_player_t *p, double gain) { (void)p; (void)gain; }
void os64_media_player_loaded(os64_media_player_t *p, bool loaded) { (void)p; (void)loaded; }
void os64_media_player_loop(os64_media_player_t *p, bool loop) { (void)p; (void)loop; }
void os64_media_player_mute(os64_media_player_t *p, bool muted) { (void)p; (void)muted; }
void os64_media_player_pause(os64_media_player_t *p) { (void)p; }
void os64_media_player_play(os64_media_player_t *p) { (void)p; }
int os64_media_player_plane(os64_media_player_t *p, int64_t win) { (void)p; (void)win; return 0; }
int64_t os64_media_player_plane_gap_take(os64_media_player_t *p) { (void)p; return 0; }
void os64_media_player_plane_want(os64_media_player_t *p, bool want) { (void)p; (void)want; }
uint32_t os64_media_player_seek(os64_media_player_t *p, int64_t frame) { (void)p; (void)frame; return 0; }
int os64_media_player_shown(os64_media_player_t *p, os64_media_player_view_t *out) { (void)p; memset(out, 0, sizeof(*out)); return -1; }
void os64_media_player_shown_done(os64_media_player_t *p, os64_media_player_view_t *view) { (void)p; (void)view; }
void os64_media_player_size(os64_media_player_t *p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; }
void os64_media_file_source(int32_t file, os64_media_source_t *out) { (void)file; memset(out, 0, sizeof(*out)); }
const char *os64_media_strerror(int code) { (void)code; return "stub"; }
size_t os64_media_type_list(char *out, size_t cap) { if (cap > 0) out[0] = '\0'; return 0; }
int os64_media_type_supported(const char *type) { (void)type; return 0; }

/* ── the fetch, the blob and the address ──────────────────────────────── */

/* A new pipe has two holders, the job and the session's reader
 * (fetch_pipe.h): `s_pipes` counts the holds not yet let go. */
static int s_pipes, s_jobs_released;
static int s_dummy_pipe;
yonder_fetch_pipe_t *yonder_fetch_pipe_new(void (*ring)(void *), void *ctx)
{ (void)ring; (void)ctx; s_pipes += 2; return (yonder_fetch_pipe_t *)&s_dummy_pipe; }
void yonder_fetch_pipe_release(yonder_fetch_pipe_t *p) { (void)p; s_pipes--; }
void yonder_fetch_release(void *raw, void *product)
{
    yonder_fetch_job_t *job = raw;
    (void)product;
    if (job == NULL)
        return;
    s_jobs_released++;
    if (job->pipe != NULL)
        yonder_fetch_pipe_release(job->pipe);
    free(job->url);
    free(job->headers);
    free(job);
}
void yonder_fetch_pipe_look(yonder_fetch_pipe_t *pipe, yonder_fetch_pipe_state_t *out) { (void)pipe; memset(out, 0, sizeof(*out)); }
const yonder_fetched_t *yonder_fetch_pipe_headed(yonder_fetch_pipe_t *pipe) { (void)pipe; return NULL; }
void yonder_fetch_pipe_ask(yonder_fetch_pipe_t *pipe) { (void)pipe; }
size_t yonder_fetch_pipe_take(yonder_fetch_pipe_t *pipe, size_t max, uint8_t **out) { (void)pipe; (void)max; *out = NULL; return 0; }
int64_t yonder_fetch_media_run(void *job, bool (*cancelled)(void *), void *ctx, void **out)
{ (void)job; (void)cancelled; (void)ctx; *out = NULL; return 0; }
os64_dom_blob_t *os64_dom_blob_hold(os64_dom_blob_t *blob) { return blob; }
void os64_dom_blob_release(os64_dom_blob_t *blob) { (void)blob; }
os64_url_result_t os64_url_parse(const char *text, os64_url_t *out) { (void)text; memset(out, 0, sizeof(*out)); return OS64_URL_OK; }
void way_origin_of(const char *url, uint64_t opaque_id, way_origin_t *out) { (void)url; (void)opaque_id; memset(out, 0, sizeof(*out)); }
bool way_referrer(const char *page, const os64_url_t *to, char *out, size_t cap) { (void)page; (void)to; if (cap > 0) out[0] = '\0'; return true; }

/* ── the cases ────────────────────────────────────────────────────────── */

static int session_count(void)
{
    int n = 0;
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next)
        n++;
    return n;
}

static yonder_media_request_t request(const char *url, bool picture)
{
    yonder_media_request_t r;
    memset(&r, 0, sizeof(r));
    r.url = url;
    r.page_url = "http://page.test/";
    r.page = 1;
    r.picture = picture;
    return r;
}

static void refused_player(void)
{
    yonder_media_request_t r = request("http://media.test/a.mp4", true);
    s_refuse_player = true;
    yonder_media_t *m = yonder_media_open(&r);
    s_refuse_player = false;
    check(m == NULL && session_count() == 1, "player refused: the open answers NULL, the session left for the reap");
    yonder_media_reap();
    check(session_count() == 0, "player refused: the next reap frees the session");
}

static void refused_loader(void)
{
    yonder_media_request_t r = request("http://media.test/a.mp4", true);
    yonder_media_t *m = yonder_media_open(&r);
    check(m != NULL && s_players == 1, "loader refused: the session opens with its player");
    if (m == NULL)
        return;
    s_refuse_threads = true;
    os64_media_source_t source;
    bool go = s_cfg.prepare(s_cfg.ctx, &source);
    s_refuse_threads = false;
    check(!go && m->facts.error == OS64_DOM_MEDIA_ERR_NETWORK, "loader refused: the prepare fails, a network error");
    check(!m->loader_started && m->job == NULL && s_audio.loaders == 0,
          "loader refused: nothing waits for it, and its slot is given back");
    check(s_jobs_released == 1 && s_pipes == 1, "loader refused: the job and its hold on the pipe are let go at once");
    yonder_media_close(m);
    yonder_media_reap();
    check(session_count() == 0, "loader refused: the reap after the close frees the session");
    check(s_pipes == 0, "loader refused: and the session's hold on the pipe with it");
}

static void refused_helper(void)
{
    yonder_media_request_t r = request("file:///tests/pages/a.mp4", true);
    yonder_media_t *m = yonder_media_open(&r);
    check(m != NULL, "helper refused: the session opens");
    if (m == NULL)
        return;
    s_refuse_threads = true;
    bool asked = yonder_media_plane_ask(m, 5);
    s_refuse_threads = false;
    check(!asked && m->plane_helper_done, "helper refused: the ask answers false, and is over");
    yonder_media_close(m);
    yonder_media_reap();
    check(session_count() == 0, "helper refused: the reap after the close frees the session");

    // For contrast: a helper that started keeps its session until it says
    // it is done.
    m = yonder_media_open(&r);
    check(m != NULL, "helper started: the session opens");
    if (m == NULL)
        return;
    int before = s_started;
    check(yonder_media_plane_ask(m, 5) && s_started == before + 1 && s_kept_fn == plane_helper,
          "helper started: the ask starts it");
    yonder_media_close(m);
    yonder_media_reap();
    check(session_count() == 1, "helper started: the reap keeps the session while the helper may touch it");
    s_kept_fn(s_kept_arg);      // the helper runs, and says it is done
    yonder_media_reap();
    check(session_count() == 0, "helper started: once it is done, the reap frees the session");
}

int main(void)
{
    yonder_media_env_t env;
    memset(&env, 0, sizeof(env));
    yonder_media_start(&env);
    refused_player();
    refused_loader();
    refused_helper();
    printf("yonder media threads host: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
