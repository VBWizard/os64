// media.c — the sound behind a page's media elements (media.h).
//
// THREADS AND WHAT EACH OWNS.
//
//   window    opens, closes, plays, pauses, seeks; reads facts; reaps.
//   decoder   one per session: the bytes kept so far (`bytes`, its alone),
//             libmedia's object, and the pushes into the PCM queue.
//   loader    one per session whose address is not a file: (the network,
//             or a data: URL), at most LOADERS_MAX at once: runs the fetch
//             job, whose body goes down the pipe the decoder drains.
//   audio     one for yonder: /dev/audio, /dev/audiopos and /dev/audioctl,
//             and the takes from the sounding session's queue.
//
// A session's `lock` guards what more than one of them touch: the asks
// (play, seek, settings, stop), the queue, the facts, the loader's
// product. `s_audio.lock` guards the session list, which session the
// device carries (`owner`) and the loader slots taken. No thread is
// joined (thread_start): a decoder or a loader says it is done as its last
// touch of its session, and only then is the session freed; the audio
// thread says so as its last touch of anything (`s_audio.done`). LOCK ORDER:
// s_audio.lock, then a session's; never the other way. A bell is rung only
// with the lock that guards its owner's right to close it held
// (os64/bell.h): a session's under its lock, the audio thread's under
// s_audio.lock.
//
// THE CLOCK (AUDIO_YONDER.md § The audio thread and the mixer). The audio
// thread counts the frames it has written since the device's counters
// last began (an open or a flush), and keeps ANCHORS: the stream frame at
// which each run of the media begins (a loop starts a new one). What the
// ear hears is /dev/audiopos's `heard`, extrapolated while the stream
// plays, and the anchor below it says which media frame that is.
//
// ONE SESSION SOUNDS AT A TIME until the mixer (Y.3): the first that asks
// to play owns the device while it wants to, and another that asks waits
// as a busy device makes it wait (§ When the stream is busy).

#include "media.h"

#include "fetch_job.h"
#include "fetch_pipe.h"
#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/lock.h"
#include "os64/media.h"
#include "os64/media_queue.h"
#include "os64/os64.h"
#include "os64/thread.h"
#include "os64/url.h"

#include <math.h>

#define RATE OS64_AUDIO_RATE
// The queue between decoder and device: two seconds.
#define QUEUE_FRAMES (2u * RATE)
// What the decoder asks of libmedia at once, and what the audio thread
// writes at once (a write blocks while the device's ring is full, so a
// pause waits for at most one of these to be taken).
#define DECODE_FRAMES 4096u
#define WRITE_FRAMES 1024u
// HAVE_FUTURE_DATA: a quarter of a second queued.
#define FUTURE_FRAMES (RATE / 4u)
// The most of a resource kept: every arrived byte is, so a seek back costs
// nothing (a seek past what has arrived waits for it). A longer one fails
// as a network error does (AUDIO_YONDER.md § As built (Y.2.1)).
#define KEPT_MAX ((size_t)256 << 20)
// The audio thread: how often it tries a busy device, how long the device
// stays open with nothing to play, and how long a pause keeps it.
#define BUSY_RETRY_MS 500u
#define IDLE_CLOSE_MS 3000u
#define PAUSE_KEEP_MS 30000u
// While a session sounds, the window is rung this often, for timeupdate.
#define RING_SOUNDING_MS 250u
#define ANCHORS 64u
// How many sounds are fetched at once; the rest wait their turn. A page may
// hold dozens of players, and a fetch costs a connection, or a bell while
// libfetch queues it, out of the program's sixty-four handles. Fewer than
// libfetch's six to one host (FETCH_POOL_PER_KEY), because a sound not
// playing holds its connection on the pipe's backpressure,
// and the page's other fetches from that host need one left (DEBTS.md
// § A page's sound).
#define LOADERS_MAX 4u
// How often yonder's end looks for the threads to have let go.
#define STOP_POLL_MS 10u

struct yonder_media {
    yonder_media_t *next;           // s_audio.sessions
    os64_lock_t lock;
    os64_bell_t bell;               // the decoder waits on it
    // Asked, under `lock`.
    bool stop;
    bool play;                      // the element wants sound
    double gain;
    bool loop;
    bool seek_asked;
    double seek_to;
    uint64_t seek_generation;
    bool resync;                    // its device tail was lost: seek to where the ear was
    // Told, under `lock`.
    os64_dom_media_facts_t facts;
    os64_media_queue_t queue;
    int16_t *storage;
    // The threads, which hold no handles (thread_start). A session is
    // freed only once each one that may be running has said it is done.
    void *product;                  // the loader's, set once as it ends
    bool loader_started;            // a loader may be running
    bool loader_quit;               // the decoder gave up on it: cancel the fetch
    bool loader_done;               // the loader's last touch of the session
    bool decoder_done;              // the decoder's last touch of the session
    bool closed;                    // the window let go of it
    // The decoder's own.
    char *url;
    char *page_url;
    const char *agent;              // borrowed (yonder_media_request_t)
    const way_identity_t *identity;
    uint64_t page;
    os64_dom_media_cors_t cors;
    yonder_fetch_job_t *job;
    yonder_fetch_pipe_t *pipe;
    int32_t file;
    uint8_t *bytes;
    size_t have, cap, at;
    int64_t size;
    bool arrived, failed, opening;
    char why[96];
};

static struct {
    os64_lock_t lock;
    os64_bell_t bell;
    bool started;                   // an audio thread may be running
    bool quit;
    bool done;                      // the audio thread's last touch of anything
    uint32_t loaders;               // loader slots taken (LOADERS_MAX)
    yonder_media_t *sessions;
    yonder_media_t *owner;
} s_audio = {.bell = OS64_BELL_NONE};   // a zeroed bell would be handle 0, someone's file

static yonder_media_env_t s_env;

static void ring_window(void)
{
    if (s_env.ring != NULL)
        s_env.ring();
}

static void ring_audio(void)
{
    os64_lock_acquire(&s_audio.lock);
    (void)os64_bell_ring(&s_audio.bell);
    os64_lock_release(&s_audio.lock);
}

// Rings a session's decoder; `m->lock` held.
static void ring_decoder_locked(yonder_media_t *m)
{
    (void)os64_bell_ring(&m->bell);
}

// Starts a thread that is never joined: its handle is let go at once, so a
// page with many players does not spend the program's handles on them. A
// failure does NOT mean the thread is not running: out of handles, the
// kernel starts it detached and still answers an error. So the caller of a
// failed start keeps alive everything the thread may touch, and waits for
// its done flag as it would for a started one.
static bool thread_start(int64_t (*fn)(void *), void *arg)
{
    int64_t handle = os64_thread(fn, arg);
    if (handle < 0)
        return false;
    os64_close((int32_t)handle);
    return true;
}

// A loader slot let go: every decoder is rung, for one may be waiting for
// it. Touches no session's memory but under the list's lock, so a loader
// may call it after its last touch of its own.
static void loader_slot_release(void)
{
    os64_lock_acquire(&s_audio.lock);
    if (s_audio.loaders > 0)
        s_audio.loaders--;
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next) {
        os64_lock_acquire(&m->lock);
        ring_decoder_locked(m);
        os64_lock_release(&m->lock);
    }
    os64_lock_release(&s_audio.lock);
}

// Whether `text` begins with the `n` letters of `word`, ignoring ASCII case.
static bool starts_nocase(const char *text, const char *word, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char a = text[i], b = word[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b || a == '\0')
            return false;
    }
    return true;
}

static char *copy_text(const char *text)
{
    size_t n = os64_strlen(text);
    char *out = os64_malloc(n + 1);
    if (out != NULL)
        os64_memcpy(out, text, n + 1);
    return out;
}

// ── The bytes (decoder thread) ─────────────────────────────────────────

// The Content-Length of a response's header block, or -1: the size a lossy
// file's end is found by (os64/media.h). A coded body (Content-Encoding)
// has no size here, since what libfetch hands over is decoded.
static int64_t content_length(const char *headers, size_t length)
{
    int64_t size = -1;
    const char *p = headers, *end = headers + length;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n')
            p++;
        const char *colon = line;
        while (colon < p && *colon != ':')
            colon++;
        size_t name = (size_t)(colon - line);
        const char *value = colon < p ? colon + 1 : p;
        while (value < p && (*value == ' ' || *value == '\t'))
            value++;
        if (name == 16 && starts_nocase(line, "content-encoding", 16))
            return -1;
        if (name == 14 && starts_nocase(line, "content-length", 14)) {
            int64_t n = 0;
            bool digits = false;
            while (value < p && *value >= '0' && *value <= '9' && n < INT64_MAX / 10) {
                n = n * 10 + (*value++ - '0');
                digits = true;
            }
            size = digits ? n : -1;
        }
        if (p < end)
            p++;
    }
    return size;
}

static bool keep(yonder_media_t *m, const uint8_t *data, size_t n)
{
    if (m->have + n > KEPT_MAX) {
        os64_strcopy(m->why, sizeof(m->why), "The sound is longer than yonder keeps");
        return false;
    }
    if (m->have + n > m->cap) {
        size_t cap = m->cap != 0 ? m->cap : (size_t)1 << 20;
        while (cap < m->have + n)
            cap *= 2;
        if (cap > KEPT_MAX)
            cap = KEPT_MAX;
        uint8_t *grown = os64_realloc(m->bytes, cap);
        if (grown == NULL) {
            os64_strcopy(m->why, sizeof(m->why), "No memory for the sound");
            return false;
        }
        m->bytes = grown;
        m->cap = cap;
    }
    os64_memcpy(m->bytes + m->have, data, n);
    m->have += n;
    return true;
}

// What has come down the pipe, kept; and the loader's whole answer, when
// the cache or a data: URL gave one. True when anything changed.
static bool pump(yonder_media_t *m)
{
    bool moved = false;
    if (m->file >= 0 || m->arrived || m->failed)
        return false;
    if (m->pipe != NULL) {
        for (;;) {
            yonder_fetch_pipe_state_t state;
            yonder_fetch_pipe_look(m->pipe, &state);
            if (state.headed && m->size == -2) {
                const yonder_fetched_t *head = yonder_fetch_pipe_headed(m->pipe);
                if (head->status != 0 && (head->status < 200 || head->status > 299)) {
                    os64_snprintf(m->why, sizeof(m->why), "The server answered %d", (int)head->status);
                    m->failed = true;
                    return true;
                }
                m->size = content_length(head->headers, head->headers_len);
                moved = true;
            }
            if (state.unread > 0) {
                uint8_t *piece = NULL;
                size_t n = yonder_fetch_pipe_take(m->pipe, YONDER_FETCH_PIPE_TAKE_MAX, &piece);
                if (n == 0)
                    break;
                bool kept = keep(m, piece, n);
                os64_free(piece);
                if (!kept) {
                    m->failed = true;
                    return true;
                }
                moved = true;
                continue;
            }
            if (state.ended) {
                if (state.headed) {
                    m->arrived = state.ok;
                    m->failed = !state.ok;
                    if (!state.ok)
                        os64_strcopy(m->why, sizeof(m->why), "The connection failed part way");
                    return true;
                }
            }
            yonder_fetch_pipe_ask(m->pipe);
            break;
        }
    }
    // The job's end, for an answer that never went down the pipe.
    os64_lock_acquire(&m->lock);
    bool done = m->loader_done;
    yonder_fetched_t *got = m->product;
    os64_lock_release(&m->lock);
    if (done && !m->arrived && !m->failed) {
        if (got == NULL || !got->ok) {
            os64_strcopy(m->why, sizeof(m->why), "The sound could not be fetched");
            m->failed = true;
        } else if (!got->streamed) {
            if (got->status != 0 && (got->status < 200 || got->status > 299)) {
                os64_snprintf(m->why, sizeof(m->why), "The server answered %d", (int)got->status);
                m->failed = true;
            } else if (got->body != NULL && !keep(m, got->body, got->body_len)) {
                m->failed = true;
            } else {
                m->size = (int64_t)m->have;
                m->arrived = true;
            }
        } else if (m->size == -2) {
            // Streamed, but its head was never taken: nothing will come.
            m->failed = true;
        }
        moved = true;
    }
    return moved;
}

// The loader's answer holds the body whole (an answer the cache or a data:
// URL gave, or the copy a streamed one kept for the cache, up to the cap):
// once the loader is done and the bytes are kept here, that copy goes.
static void product_trim(yonder_media_t *m)
{
    if (!m->arrived && !m->failed)
        return;
    os64_lock_acquire(&m->lock);
    yonder_fetched_t *got = m->loader_done ? m->product : NULL;
    if (got != NULL && got->body != NULL) {
        os64_free(got->body);
        got->body = NULL;
    }
    os64_lock_release(&m->lock);
}

// Wait for news from the pipe or an ask; false when told to stop.
static bool decoder_wait(yonder_media_t *m, uint64_t ms)
{
    (void)os64_bell_wait(&m->bell, ms);
    os64_lock_acquire(&m->lock);
    bool stop = m->stop;
    os64_lock_release(&m->lock);
    return !stop;
}

static int64_t store_read(void *ctx, uint8_t *out, size_t cap)
{
    yonder_media_t *m = ctx;
    for (;;) {
        if (m->at < m->have) {
            size_t n = m->have - m->at;
            if (n > cap)
                n = cap;
            os64_memcpy(out, m->bytes + m->at, n);
            m->at += n;
            return (int64_t)n;
        }
        if (m->arrived)
            return 0;
        if (m->failed)
            return -1;
        if (!pump(m) && !decoder_wait(m, 200))
            return -1;
    }
}

// A seek inside what has arrived is free. One past it fails at once while
// the media is being opened (a reader probing the file's end, as the MP3
// reader does for its tags, carries on without them rather than wait for
// the whole file), and afterwards waits for the bytes to come.
static int64_t store_seek(void *ctx, uint64_t offset)
{
    yonder_media_t *m = ctx;
    for (;;) {
        if (offset <= m->have) {
            m->at = (size_t)offset;
            return (int64_t)offset;
        }
        if (m->opening || m->arrived || m->failed)
            return -1;
        if (!pump(m) && !decoder_wait(m, 200))
            return -1;
    }
}

// ── The decoder thread ─────────────────────────────────────────────────

static bool loader_cancelled(void *ctx)
{
    yonder_media_t *m = ctx;
    os64_lock_acquire(&m->lock);
    bool stop = m->stop || m->loader_quit;
    os64_lock_release(&m->lock);
    return stop;
}

static int64_t loader_main(void *arg)
{
    yonder_media_t *m = arg;
    void *product = NULL;
    (void)yonder_fetch_media_run(m->job, loader_cancelled, m, &product);
    os64_lock_acquire(&m->lock);
    m->product = product;
    m->loader_done = true;
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
    // `m` may be freed from here on.
    loader_slot_release();
    return 0;
}

// Waits for a loader slot. False when the session stops first.
static bool loader_slot_take(yonder_media_t *m)
{
    for (;;) {
        os64_lock_acquire(&s_audio.lock);
        bool got = s_audio.loaders < LOADERS_MAX;
        if (got)
            s_audio.loaders++;
        os64_lock_release(&s_audio.lock);
        if (got)
            return true;
        if (!decoder_wait(m, OS64_WAIT_FOREVER))
            return false;
    }
}

static void pipe_ring(void *ctx)
{
    yonder_media_t *m = ctx;
    os64_lock_acquire(&m->lock);
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
}

// The path a file: address names (as yonder.c's file_path reads one).
static const char *file_path(const char *url, char *out, size_t cap)
{
    const char *after = url + 7;
    if (os64_strlen(after) >= 10 && os64_memcmp(after, "localhost/", 10) == 0)
        after += 9;
    if (after[0] == '/')
        return after;
    os64_snprintf(out, cap, "/%s", after);
    return out;
}

static bool is_scheme(const char *url, const char *scheme)
{
    return starts_nocase(url, scheme, os64_strlen(scheme));
}

// The fetch job for a network address, its body streamed to the decoder,
// once a loader slot is free. False when the fetch could not begin; then
// `loader_started` says whether a loader may be running all the same, and
// the job and the pipe stay the session's until it is freed.
static bool loader_start(yonder_media_t *m)
{
    if (!loader_slot_take(m))
        return false;
    yonder_fetch_job_t *job = os64_calloc(1, sizeof(*job));
    if (job == NULL) {
        loader_slot_release();
        return false;
    }
    job->kind = YONDER_JOB_FETCH;
    job->page = m->page;
    job->agent = m->agent;
    job->identity = m->identity;
    job->default_accept = "*/*";
    job->url = copy_text(m->url);
    job->headers = os64_calloc(1, 1);
    way_origin_of(m->page_url, m->page != 0 ? m->page : 1, &job->request.origin);
    // HTML's CORS settings attribute: none is no-cors with credentials,
    // anonymous is cors same-origin, use-credentials is cors with them.
    job->request.mode = m->cors == OS64_DOM_MEDIA_NO_CORS ? WAY_MODE_NO_CORS : WAY_MODE_CORS;
    job->request.credentials = m->cors == OS64_DOM_MEDIA_CORS_ANONYMOUS ? WAY_CREDENTIALS_SAME_ORIGIN
                                                                        : WAY_CREDENTIALS_INCLUDE;
    job->request.redirect = WAY_REDIRECT_FOLLOW;
    job->request.policy = WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN;
    os64_strcopy(job->request.method, sizeof(job->request.method), "GET");
    os64_url_t to;
    if (os64_url_parse(m->url, &to) != OS64_URL_OK || !way_referrer(m->page_url, &to, job->referrer, sizeof(job->referrer)))
        job->referrer[0] = '\0';
    job->jar = s_env.jar;
    job->preflight = s_env.preflight;
    job->cache = s_env.cache;
    job->cache_mode = WAY_CACHE_DEFAULT;
    // A new pipe has two holders (fetch_pipe.h): the job, whose release
    // lets go of the writer's side, and the decoder, the reader.
    job->pipe = yonder_fetch_pipe_new(pipe_ring, m);
    if (job->url == NULL || job->headers == NULL || job->pipe == NULL) {
        if (job->pipe != NULL)
            yonder_fetch_pipe_release(job->pipe);
        yonder_fetch_release(job, NULL);
        loader_slot_release();
        return false;
    }
    m->pipe = job->pipe;
    m->job = job;
    job->submitted_us = os64_micros();
    // Before the start: the loader may finish before os64_thread returns,
    // and the session must not look free of it in between. Its slot is the
    // loader's to let go from here on, whatever the start answers; a start
    // that truly failed (the kernel out of memory) keeps the slot and the
    // session for the rest of the program, and yonder's end waits on it,
    // the price of not guessing.
    os64_lock_acquire(&m->lock);
    m->loader_started = true;
    os64_lock_release(&m->lock);
    if (!thread_start(loader_main, m)) {
        os64_lock_acquire(&m->lock);
        m->loader_quit = true;
        os64_lock_release(&m->lock);
        return false;
    }
    return true;
}

// Whether the queue will take no more for now: no room for a decode, or
// every segment in use (a short clip on loop uses one per run, and runs
// out of them before it runs out of frames). `m->lock` held.
static bool queue_full_locked(const yonder_media_t *m)
{
    return os64_media_queue_room(&m->queue) < DECODE_FRAMES || m->queue.segments == OS64_MEDIA_QUEUE_SEGMENTS;
}

// The readiness the queue makes, and the fetch's progress, told; `m->lock`
// held. True when anything libdom reacts to changed. A media decoded to
// its end is as ready as it will ever be, however little of it is still
// queued: the rest has gone to the device, and nothing can starve it.
static bool readiness_locked(yonder_media_t *m, bool decoded_all)
{
    os64_dom_media_facts_t was = m->facts;
    uint32_t count = os64_media_queue_count(&m->queue);
    m->facts.current = count > 0 || decoded_all;
    m->facts.future = count >= FUTURE_FRAMES || decoded_all;
    m->facts.enough = queue_full_locked(m) || decoded_all;
    bool loaded = m->file >= 0 || m->arrived;
    m->facts.loaded = loaded;
    if (m->facts.probed && isfinite(m->facts.duration))
        m->facts.arrived = loaded ? m->facts.duration
                                  : m->size > 0 ? m->facts.duration * (double)m->have / (double)m->size : 0;
    return was.current != m->facts.current || was.future != m->facts.future || was.enough != m->facts.enough ||
           was.loaded != m->facts.loaded;
}

static void fail(yonder_media_t *m, int code, const char *why)
{
    os64_lock_acquire(&m->lock);
    if (m->facts.error == 0) {
        m->facts.error = code;
        os64_strcopy(m->facts.message, sizeof(m->facts.message), why);
    }
    os64_media_queue_end(&m->queue);
    os64_lock_release(&m->lock);
    ring_window();
    ring_audio();
}

static int open_error(int result)
{
    return result == OS64_MEDIA_DECODE ? OS64_DOM_MEDIA_ERR_DECODE : OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
}

static int64_t decoder_main(void *arg)
{
    yonder_media_t *m = arg;
    os64_media_t *media = NULL;
    os64_media_source_t source = {0};
    int16_t *pcm = os64_malloc(sizeof(int16_t) * 2 * DECODE_FRAMES);
    bool network = !is_scheme(m->url, "file:");
    os64_lock_acquire(&m->lock);
    bool stopped = m->stop;
    os64_lock_release(&m->lock);
    if (stopped) {
        // Closed before it ran (a start that answered an error): nothing
        // to set up, and the wait below ends at once.
    } else if (pcm == NULL) {
        fail(m, OS64_DOM_MEDIA_ERR_NETWORK, "No memory for the sound");
    } else if (!network) {
        char path[OS64_URL_REF_MAX];
        int64_t h = os64_open(file_path(m->url, path, sizeof(path)), "r");
        m->file = h >= 0 ? (int32_t)h : -1;
        if (m->file < 0)
            fail(m, OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, "The file would not open");
        else
            os64_media_file_source(m->file, &source);
    } else if (!loader_start(m)) {
        fail(m, OS64_DOM_MEDIA_ERR_NETWORK, "The sound could not be fetched");
    } else {
        // The size is the head's: wait for it (or for the answer, whole).
        m->size = -2;
        while (m->size == -2 && !m->failed && !m->arrived)
            if (!pump(m) && !decoder_wait(m, 200))
                break;
        if (m->size == -2)
            m->size = -1;
        source = (os64_media_source_t){m, store_read, store_seek, m->size};
        if (m->failed)
            fail(m, OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, m->why);
    }
    os64_lock_acquire(&m->lock);
    bool go = m->facts.error == 0 && !m->stop;
    os64_lock_release(&m->lock);
    os64_media_info_t info = {0};
    if (go) {
        m->opening = true;
        int result = os64_media_open(&source, &media, &info);
        m->opening = false;
        if (result != 0) {
            media = NULL;
            fail(m, m->failed ? OS64_DOM_MEDIA_ERR_NETWORK : open_error(result),
                 m->failed ? m->why : os64_media_strerror(result));
            go = false;
        }
    }
    if (go) {
        os64_lock_acquire(&m->lock);
        m->facts.probed = true;
        m->facts.duration = info.frames >= 0 ? (double)info.frames / RATE : INFINITY;
        m->facts.seekable = info.seekable;
        (void)readiness_locked(m, false);
        os64_lock_release(&m->lock);
        ring_window();
    }
    uint32_t generation = 0;
    bool decoded_all = false;
    bool landing = false;           // a seek whose first frames are not queued yet
    uint64_t landing_generation = 0;
    uint64_t run_frames = 0;        // frames queued since the last seek (a loop's run)
    while (go) {
        os64_lock_acquire(&m->lock);
        if (m->stop) {
            os64_lock_release(&m->lock);
            break;
        }
        bool dom_seek = m->seek_asked, resync = m->resync;
        double to = dom_seek ? m->seek_to : m->facts.position;
        uint64_t asked = m->seek_generation;
        m->seek_asked = false;
        m->resync = false;
        bool full = queue_full_locked(m);
        bool loop = m->loop;
        os64_lock_release(&m->lock);
        if (dom_seek || resync) {
            // A time past any media's length (a stream's duration is
            // infinite, so libdom clamps nothing) is held to one the frame
            // count can hold; libmedia answers a frame past the end with
            // the end.
            if (!(to > 0))
                to = 0;
            if (to > 1e9)
                to = 1e9;
            int sought = os64_media_seek(media, (int64_t)(to * RATE + 0.5));
            if (sought != 0) {
                // Refused: nothing moved, so what is queued stays, and the
                // seek lands at once where the media still is.
                os64_lock_acquire(&m->lock);
                if (dom_seek)
                    m->facts.seeked = asked;
                os64_lock_release(&m->lock);
                ring_window();
                continue;
            }
            os64_lock_acquire(&m->lock);
            generation = os64_media_queue_flush(&m->queue);
            decoded_all = false;
            run_frames = 0;
            m->facts.ended = false;
            m->facts.position = (double)os64_media_position(media) / RATE;
            (void)readiness_locked(m, false);
            os64_lock_release(&m->lock);
            // The seek has landed once sound from the new place is queued
            // (below), so the element's seeked comes with data to play.
            if (dom_seek) {
                landing = true;
                landing_generation = asked;
            }
            ring_audio();
            continue;
        }
        // Played or not, the queue is filled and no more: an element
        // reaches HAVE_ENOUGH_DATA before play, as in other browsers,
        // whatever its preload (HTML's hint), because pages start their
        // sound from canplay. Once it is full the fetch waits on the
        // pipe's backpressure until the audio thread takes from it.
        bool moved = pump(m);
        if (full || decoded_all) {
            os64_lock_acquire(&m->lock);
            moved = readiness_locked(m, decoded_all) || moved;
            os64_lock_release(&m->lock);
            if (moved)
                ring_window();
            product_trim(m);
            // Parked until rung: every reason to go on rings this bell (a
            // play, a seek, the settings, a stop, the audio thread taking
            // from the queue), so a finished or idle element costs nothing.
            if (!decoder_wait(m, OS64_WAIT_FOREVER))
                break;
            continue;
        }
        int64_t at = os64_media_position(media);
        int64_t n = os64_media_decode(media, pcm, DECODE_FRAMES);
        if (n > 0) {
            // All of it, waiting for room when the queue will not take it
            // whole (a short sound on loop can use up the queue's runs
            // before its frames).
            uint32_t pushed = 0;
            bool changed = false;
            while (pushed < (uint32_t)n) {
                os64_lock_acquire(&m->lock);
                bool was_empty = os64_media_queue_count(&m->queue) == 0;
                pushed += os64_media_queue_push(&m->queue, generation, pcm + 2 * pushed, (uint32_t)n - pushed,
                                                at + pushed);
                changed = readiness_locked(m, false) || changed;
                if (landing && pushed > 0) {
                    m->facts.seeked = landing_generation;
                    landing = false;
                    changed = true;
                }
                // A seek asked meanwhile flushes what is left anyway, and
                // the audio thread may be waiting on that seek (a resync)
                // rather than taking: give way to it.
                bool giving_way = m->seek_asked || m->resync;
                os64_lock_release(&m->lock);
                if (was_empty)
                    ring_audio();
                if (pushed < (uint32_t)n && (giving_way || !decoder_wait(m, OS64_WAIT_FOREVER)))
                    break;
            }
            run_frames += pushed;
            if (changed)
                ring_window();
        } else if (n == 0) {
            // The next frames begin a new run at 0, which the queue keeps
            // apart and the clock follows; a run that gave nothing, or a
            // seek back that fails, ends the media as with no loop.
            if (loop && run_frames > 0 && os64_media_seek(media, 0) == 0) {
                run_frames = 0;
                continue;
            }
            os64_lock_acquire(&m->lock);
            os64_media_queue_end(&m->queue);
            decoded_all = true;
            // A length the probe could not tell is known now: where the
            // decode ended (libdom fires durationchange for it).
            if (!isfinite(m->facts.duration))
                m->facts.duration = (double)os64_media_position(media) / RATE;
            (void)readiness_locked(m, true);
            if (landing) {
                m->facts.seeked = landing_generation;
                landing = false;
            }
            os64_lock_release(&m->lock);
            ring_window();
            ring_audio();
        } else {
            os64_lock_acquire(&m->lock);
            if (landing) {
                m->facts.seeked = landing_generation;
                landing = false;
            }
            os64_lock_release(&m->lock);
            fail(m, m->failed ? OS64_DOM_MEDIA_ERR_NETWORK : OS64_DOM_MEDIA_ERR_DECODE,
                 m->failed ? m->why : os64_media_strerror((int)n));
            decoded_all = true;
        }
    }
    // Nothing more to do until stopped (a failed session waits for its
    // close like any other). The stop is looked at BEFORE each wait: the
    // loop above may have left on the very ring that said stop, and a wait
    // first would wait for a ring that never comes.
    for (;;) {
        os64_lock_acquire(&m->lock);
        bool stop = m->stop;
        os64_lock_release(&m->lock);
        if (stop)
            break;
        (void)os64_bell_wait(&m->bell, OS64_WAIT_FOREVER);
    }
    os64_media_close(media);
    if (m->file >= 0)
        os64_close(m->file);
    m->file = -1;
    os64_free(pcm);
    // The window rung under the lock that publishes the flag: the reap
    // reads the flag under it, so once the session is seen done this
    // thread touches nothing of the session's or the window's again.
    os64_lock_acquire(&m->lock);
    m->decoder_done = true;
    ring_window();
    os64_lock_release(&m->lock);
    return 0;
}

// ── The audio thread ───────────────────────────────────────────────────

typedef struct {
    int64_t audio, position;        // the stream and its position, -1 when shut
    uint32_t generation;            // the position's, once known
    bool generation_known;
    bool paused;                    // the stream's pause is on
    uint64_t written;               // frames written since the counters began
    struct { uint64_t stream; int64_t media; } anchor[ANCHORS];
    uint32_t anchors;
    int64_t media_end;              // the media frame after the last written
    uint32_t queue_generation;      // the owner's queue's, as last written from
    bool refused;                   // the last open found the stream another program's
    uint64_t idle_since, paused_since, busy_tried, rang;
    uint64_t drained_since;         // micros when the ring was first seen dry with all of it taken
} Device;

static uint64_t now_ms(void)
{
    return (uint64_t)os64_micros() / 1000;
}

static void control(const char *verb)
{
    int64_t h = os64_open("/dev/audioctl", "w");
    if (h >= 0) {
        (void)os64_write((int32_t)h, verb, os64_strlen(verb));
        os64_close((int32_t)h);
    }
}

static void counters_restart(Device *d)
{
    d->written = 0;
    d->anchors = 0;
    d->media_end = -1;
    d->generation_known = false;
    d->drained_since = 0;
}

static void device_close(Device *d)
{
    if (d->audio >= 0)
        os64_close((int32_t)d->audio);
    if (d->position >= 0)
        os64_close((int32_t)d->position);
    d->audio = d->position = -1;
    d->paused = false;
    counters_restart(d);
}

// The media frame the ear is at, or -1 when the position cannot say.
// `*drained` (when the position can say) is true once the sink has taken
// every frame written and the ring has run dry, with `*delay_us` what the
// sink says lies beyond its pull: the last of it reaches the ear that long
// after (a real sink's `heard` need not catch up with `taken` by itself).
// `*lost` is true when the stream began a generation the audio thread did
// not start: a flush from elsewhere (a sink attached or detached, a
// `flush` written to /dev/audioctl) threw away what it held of ours.
static int64_t heard_frame(Device *d, bool *playing, bool *drained, uint32_t *delay_us, bool *lost)
{
    *playing = *drained = *lost = false;
    *delay_us = 0;
    if (d->position < 0 || d->anchors == 0)
        return -1;
    os64_audio_position_t pos;
    if (os64_read((int32_t)d->position, &pos, sizeof(pos)) < (int64_t)sizeof(pos))
        return -1;
    if (!d->generation_known) {
        d->generation = pos.generation;
        d->generation_known = true;
    }
    if (pos.generation != d->generation) {
        *lost = true;
        return -1;
    }
    if (pos.accepted > d->written)
        return -1;
    *drained = pos.taken >= d->written && pos.state == OS64_AUDIO_STARVED;
    *delay_us = pos.delay_us;
    uint64_t heard = pos.heard;
    if (pos.state == OS64_AUDIO_PLAYING) {
        *playing = true;
        uint64_t now = (uint64_t)os64_micros();
        if (now > pos.micros)
            heard += (now - pos.micros) * pos.rate / 1000000u;
        if (heard > pos.taken)
            heard = pos.taken;
    }
    // Anchors the ear has gone past (its successor is heard too) are done
    // with; dropping them here is what frees room for more (the take waits
    // while there is none). Once the ring has run dry with all of it taken,
    // `heard` stops short of `taken` by the sink's delay and moves no more
    // until something is written, so the prune goes by `taken` then, or a
    // take waiting for room would wait forever.
    uint64_t passed = *drained ? pos.taken : heard;
    while (d->anchors > 1 && d->anchor[1].stream <= passed) {
        for (uint32_t k = 1; k < d->anchors; k++)
            d->anchor[k - 1] = d->anchor[k];
        d->anchors--;
    }
    uint32_t i = d->anchors;
    while (i > 1 && d->anchor[i - 1].stream > heard)
        i--;
    // Pruned by `taken`, the ear can be short of the first anchor kept: it
    // is at that anchor's start as near as the clock can say.
    if (heard < d->anchor[i - 1].stream)
        return d->anchor[i - 1].media;
    return d->anchor[i - 1].media + (int64_t)(heard - d->anchor[i - 1].stream);
}

static void apply_gain(int16_t *pcm, uint32_t frames, double gain)
{
    if (gain >= 1)
        return;
    int32_t g = (int32_t)(gain * 65536.0 + 0.5);
    for (uint32_t i = 0; i < frames * 2; i++)
        pcm[i] = (int16_t)(((int32_t)pcm[i] * g) >> 16);
}

// Write all of `frames`; false when the device failed.
static bool device_write(Device *d, const int16_t *pcm, uint32_t frames)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t left = (size_t)frames * OS64_AUDIO_FRAME_BYTES;
    while (left > 0) {
        int64_t n = os64_write((int32_t)d->audio, bytes, left);
        if (n == OS64_INTERRUPTED)
            continue;
        if (n == OS64_AUDIO_FLUSHED)
            return true;
        if (n < 0)
            return false;
        bytes += n;
        left -= (size_t)n;
    }
    return true;
}

// Which session should sound now (s_audio.lock held): the owner while it
// wants to, else the first that asks. The others that ask wait, told busy.
static bool asks(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    bool asking = m->play && !m->stop && !m->facts.ended;
    os64_lock_release(&m->lock);
    return asking;
}

static yonder_media_t *choose_locked(void)
{
    yonder_media_t *o = s_audio.owner;
    if (o != NULL && asks(o))
        return o;
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next)
        if (asks(m))
            return m;
    return NULL;
}

// An owner that can no longer sound: closed, or played to its end.
static bool owner_gone(yonder_media_t *o)
{
    os64_lock_acquire(&o->lock);
    bool gone = o->stop || o->facts.ended;
    os64_lock_release(&o->lock);
    return gone;
}

// Each session that asks to play and cannot is told busy: one another
// session owns the device for, and the owner when another program has
// the stream (`refused`). s_audio.lock held.
static void tell_busy_locked(bool refused)
{
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next) {
        os64_lock_acquire(&m->lock);
        bool busy = m->play && !m->stop && !m->facts.ended && (m != s_audio.owner || refused);
        bool changed = busy != m->facts.busy;
        m->facts.busy = busy;
        if (busy)
            m->facts.sounding = false;
        os64_lock_release(&m->lock);
        if (changed)
            ring_window();
    }
}

static int64_t audio_main(void *arg)
{
    (void)arg;
    Device d = {.audio = -1, .position = -1, .media_end = -1};
    int16_t *buf = os64_malloc(sizeof(int16_t) * 2 * WRITE_FRAMES);
    d.idle_since = now_ms();
    for (;;) {
        os64_lock_acquire(&s_audio.lock);
        if (s_audio.quit) {
            os64_lock_release(&s_audio.lock);
            break;
        }
        yonder_media_t *want = choose_locked(), *owner = s_audio.owner;
        if (owner != NULL && want != owner && (want != NULL || owner_gone(owner))) {
            // Another takes the device, or the owner went: what the device
            // still holds of the owner is not to be heard. One that comes
            // back starts again from where its ear was.
            os64_lock_acquire(&owner->lock);
            owner->facts.sounding = false;
            if (!owner->stop && !owner->facts.ended)
                owner->resync = true;
            os64_lock_release(&owner->lock);
            if (d.audio >= 0) {
                control("flush\n");
                if (d.paused)
                    control("resume\n");
                d.paused = false;
                counters_restart(&d);
            }
            s_audio.owner = owner = want;
            if (owner != NULL) {
                os64_lock_acquire(&owner->lock);
                d.queue_generation = owner->queue.generation;
                os64_lock_release(&owner->lock);
            }
        } else if (owner == NULL && want != NULL) {
            // The device may still be open from one that played out: its
            // counters start again for this one (an empty ring loses
            // nothing to the flush).
            if (d.audio >= 0) {
                control("flush\n");
                if (d.paused)
                    control("resume\n");
                d.paused = false;
                counters_restart(&d);
            }
            s_audio.owner = owner = want;
            os64_lock_acquire(&owner->lock);
            d.queue_generation = owner->queue.generation;
            os64_lock_release(&owner->lock);
        }
        tell_busy_locked(d.refused);
        os64_lock_release(&s_audio.lock);
        uint64_t now = now_ms();

        if (owner == NULL) {
            if (d.audio >= 0 && now - d.idle_since >= IDLE_CLOSE_MS)
                device_close(&d);
            (void)os64_bell_wait(&s_audio.bell, 250);
            continue;
        }
        d.idle_since = now;
        os64_lock_acquire(&owner->lock);
        bool play = owner->play && !owner->stop, resync = owner->resync;
        os64_lock_release(&owner->lock);

        if (!play) {
            // Paused, and nobody else asks: the stream's own pause keeps
            // what it holds and stops the ear where it is. A long pause
            // gives the device back (AUDIO_YONDER.md § Pause).
            if (d.audio >= 0 && !d.paused) {
                control("pause\n");
                d.paused = true;
                d.paused_since = now;
            }
            os64_lock_acquire(&owner->lock);
            owner->facts.sounding = false;
            os64_lock_release(&owner->lock);
            if (d.audio >= 0 && now - d.paused_since >= PAUSE_KEEP_MS) {
                device_close(&d);
                // Let go of it last: once it is no owner the window may
                // reap it, so nothing here touches it after.
                os64_lock_acquire(&s_audio.lock);
                if (s_audio.owner == owner) {
                    os64_lock_acquire(&owner->lock);
                    owner->resync = true;
                    os64_lock_release(&owner->lock);
                    s_audio.owner = NULL;
                }
                os64_lock_release(&s_audio.lock);
            }
            (void)os64_bell_wait(&s_audio.bell, 250);
            continue;
        }
        if (resync) {
            // The decoder seeks to where the ear was and flushes the
            // queue, which the take below sees.
            os64_lock_acquire(&owner->lock);
            ring_decoder_locked(owner);
            os64_lock_release(&owner->lock);
            (void)os64_bell_wait(&s_audio.bell, 20);
            continue;
        }
        if (d.audio < 0) {
            if (now - d.busy_tried < BUSY_RETRY_MS && d.busy_tried != 0) {
                (void)os64_bell_wait(&s_audio.bell, BUSY_RETRY_MS);
                continue;
            }
            d.busy_tried = now;
            int64_t h = os64_open("/dev/audio", "w");
            if (h < 0) {
                // Another program has the stream: the owner waits (busy),
                // and the open is tried again twice a second.
                d.refused = true;
                os64_lock_acquire(&s_audio.lock);
                tell_busy_locked(true);
                os64_lock_release(&s_audio.lock);
                continue;
            }
            d.audio = h;
            int64_t p = os64_open("/dev/audiopos", "r");
            d.position = p >= 0 ? p : -1;
            d.busy_tried = 0;
            d.refused = false;
            counters_restart(&d);
            os64_lock_acquire(&s_audio.lock);
            tell_busy_locked(false);
            os64_lock_release(&s_audio.lock);
        }
        if (d.paused) {
            control("resume\n");
            d.paused = false;
        }
        os64_lock_acquire(&owner->lock);
        if (owner->queue.generation != d.queue_generation) {
            // A seek: what the device holds is from before it.
            d.queue_generation = owner->queue.generation;
            os64_lock_release(&owner->lock);
            control("flush\n");
            counters_restart(&d);
            os64_lock_acquire(&owner->lock);
        }
        int64_t at = 0;
        // No take while every anchor is in use (a clip a few milliseconds
        // long on loop starts a run per write): the device holds runs not
        // yet heard, and their anchors are what the clock reads them by.
        uint32_t n = buf != NULL && (d.anchors < ANCHORS || d.position < 0)
            ? os64_media_queue_take(&owner->queue, buf, WRITE_FRAMES, &at) : 0;
        bool finished = os64_media_queue_finished(&owner->queue);
        double gain = owner->gain;
        if (n > 0)
            ring_decoder_locked(owner);
        os64_lock_release(&owner->lock);

        if (n > 0) {
            if (d.anchors == 0 || at != d.media_end) {
                // Full only with no position to read (nothing prunes, and
                // nothing reads them): the oldest goes.
                if (d.anchors == ANCHORS) {
                    for (uint32_t i = 1; i < ANCHORS; i++)
                        d.anchor[i - 1] = d.anchor[i];
                    d.anchors--;
                }
                d.anchor[d.anchors].stream = d.written;
                d.anchor[d.anchors].media = at;
                d.anchors++;
            }
            d.media_end = at + n;
            apply_gain(buf, n, gain);
            if (!device_write(&d, buf, n)) {
                device_close(&d);
                fail(owner, OS64_DOM_MEDIA_ERR_DECODE, "The sound device failed");
                continue;
            }
            d.written += n;
        }
        bool playing = false, drained = false, lost = false;
        uint32_t delay_us = 0;
        int64_t frame = heard_frame(&d, &playing, &drained, &delay_us, &lost);
        if (lost) {
            // As when another session takes the device: the counters start
            // again, and the owner goes on from where its ear was.
            os64_lock_acquire(&owner->lock);
            owner->resync = true;
            os64_lock_release(&owner->lock);
            counters_restart(&d);
            continue;
        }
        if (!drained)
            d.drained_since = 0;
        else if (d.drained_since == 0)
            d.drained_since = (uint64_t)os64_micros();
        bool all_heard = finished && frame >= 0 &&
                         (frame >= d.media_end ||
                          (drained && (uint64_t)os64_micros() - d.drained_since >= delay_us));
        os64_lock_acquire(&owner->lock);
        bool synced = owner->queue.generation == d.queue_generation;
        if (synced && frame >= 0)
            owner->facts.position = (double)frame / RATE;
        owner->facts.sounding = playing && synced;
        bool ended_now = false;
        // `finished` and what was heard describe the generation the take
        // saw; a seek that flushed the queue since makes them another
        // playback's, which must not be called ended.
        if (synced && finished && (all_heard || d.written == 0) && !owner->facts.ended) {
            owner->facts.ended = true;
            owner->facts.sounding = false;
            ended_now = true;
        }
        os64_lock_release(&owner->lock);
        if (ended_now || (playing && now - d.rang >= RING_SOUNDING_MS)) {
            d.rang = now;
            ring_window();
        }
        if (ended_now) {
            // Played out: the device is nobody's (choose_locked passes over
            // a session that has ended), and closes once idle.
            os64_lock_acquire(&s_audio.lock);
            if (s_audio.owner == owner)
                s_audio.owner = NULL;
            os64_lock_release(&s_audio.lock);
            counters_restart(&d);
            continue;
        }
        if (n == 0)
            (void)os64_bell_wait(&s_audio.bell, finished ? 20 : 10);
    }
    device_close(&d);
    os64_free(buf);
    os64_lock_acquire(&s_audio.lock);
    s_audio.done = true;
    os64_lock_release(&s_audio.lock);
    return 0;
}

// ── The window's calls ─────────────────────────────────────────────────

static char s_sounds[512];

void yonder_media_start(const yonder_media_env_t *env)
{
    s_env = *env;
    (void)os64_media_type_list(s_sounds, sizeof(s_sounds));
}

const char *yonder_media_sounds(void)
{
    return s_sounds;
}

static bool audio_running(void)
{
    if (s_audio.started)
        return true;
    if (os64_bell_make(&s_audio.bell) != 0)
        return false;
    // Started or not as thread_start answers, it may be running: it keeps
    // its bell, and no second one is ever started beside it. The open
    // that saw the error fails.
    s_audio.started = true;
    return thread_start(audio_main, NULL);
}

// Whether a session's threads have all let go of it. Its lock held.
static bool threads_done_locked(const yonder_media_t *m)
{
    return m->decoder_done && (!m->loader_started || m->loader_done);
}

// Frees a session no thread is using any more (threads_done_locked).
static void session_free(yonder_media_t *m)
{
    yonder_fetch_release(m->job, m->product);
    if (m->pipe != NULL)
        yonder_fetch_pipe_release(m->pipe);
    os64_bell_close(&m->bell);
    os64_free(m->storage);
    os64_free(m->bytes);
    os64_free(m->url);
    os64_free(m->page_url);
    os64_free(m);
}

yonder_media_t *yonder_media_open(const yonder_media_request_t *r)
{
    if (!audio_running())
        return NULL;
    yonder_media_t *m = os64_calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    m->file = -1;
    m->gain = 1;
    m->cors = r->cors;
    m->page = r->page;
    m->agent = r->agent;
    m->identity = r->identity;
    m->facts.duration = NAN;
    m->bell = (os64_bell_t)OS64_BELL_NONE;
    m->url = copy_text(r->url);
    m->page_url = copy_text(r->page_url != NULL ? r->page_url : "about:blank");
    m->storage = os64_malloc(sizeof(int16_t) * 2 * QUEUE_FRAMES);
    if (m->url == NULL || m->page_url == NULL || m->storage == NULL || os64_bell_make(&m->bell) != 0) {
        session_free(m);
        return NULL;
    }
    os64_media_queue_init(&m->queue, m->storage, QUEUE_FRAMES);
    // On the list before the start, so a decoder that runs at once is
    // already one a freed loader slot rings.
    os64_lock_acquire(&s_audio.lock);
    m->next = s_audio.sessions;
    s_audio.sessions = m;
    os64_lock_release(&s_audio.lock);
    if (!thread_start(decoder_main, m)) {
        // The decoder may be running all the same (thread_start): the
        // session is closed and left on the list, for the reap to free
        // once the decoder says it is done.
        os64_lock_acquire(&m->lock);
        m->stop = true;
        m->closed = true;
        ring_decoder_locked(m);
        os64_lock_release(&m->lock);
        return NULL;
    }
    return m;
}

void yonder_media_close(yonder_media_t *m)
{
    if (m == NULL)
        return;
    os64_lock_acquire(&m->lock);
    m->stop = true;
    m->play = false;
    m->closed = true;
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
    ring_audio();
}

void yonder_media_play(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    m->play = true;
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
    ring_audio();
}

void yonder_media_pause(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    m->play = false;
    os64_lock_release(&m->lock);
    ring_audio();
}

void yonder_media_seek(yonder_media_t *m, double seconds, uint64_t generation)
{
    os64_lock_acquire(&m->lock);
    m->seek_asked = true;
    m->seek_to = seconds;
    m->seek_generation = generation;
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
}

void yonder_media_settings(yonder_media_t *m, double gain, bool loop)
{
    os64_lock_acquire(&m->lock);
    m->gain = gain < 0 ? 0 : gain > 1 ? 1 : gain;
    m->loop = loop;
    ring_decoder_locked(m);
    os64_lock_release(&m->lock);
}

void yonder_media_facts(yonder_media_t *m, os64_dom_media_facts_t *out)
{
    os64_lock_acquire(&m->lock);
    *out = m->facts;
    os64_lock_release(&m->lock);
}

int yonder_media_can_play(const char *type)
{
    int answer = os64_media_type_supported(type);
    return answer == OS64_MEDIA_TYPE_PROBABLY ? 2 : answer == OS64_MEDIA_TYPE_MAYBE ? 1 : 0;
}

void yonder_media_reap(void)
{
    os64_lock_acquire(&s_audio.lock);
    yonder_media_t **link = &s_audio.sessions, *done = NULL;
    while (*link != NULL) {
        yonder_media_t *m = *link;
        os64_lock_acquire(&m->lock);
        bool gone = m->closed && threads_done_locked(m) && s_audio.owner != m;
        os64_lock_release(&m->lock);
        if (gone) {
            *link = m->next;
            m->next = done;
            done = m;
        } else {
            link = &m->next;
        }
    }
    os64_lock_release(&s_audio.lock);
    while (done != NULL) {
        yonder_media_t *next = done->next;
        session_free(done);
        done = next;
    }
}

void yonder_media_stop(void)
{
    os64_lock_acquire(&s_audio.lock);
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next) {
        os64_lock_acquire(&m->lock);
        m->stop = true;
        m->closed = true;
        ring_decoder_locked(m);
        os64_lock_release(&m->lock);
    }
    s_audio.quit = true;
    if (s_audio.started)
        (void)os64_bell_ring(&s_audio.bell);
    os64_lock_release(&s_audio.lock);
    // No thread has a handle to join (thread_start): this waits for the
    // audio thread's done and for every session to be freed as its threads
    // say they are done, because the environment they use is freed after
    // it. The audio thread lets go of the device first, so the owner is
    // cleared once it has, and the reap may take the last session. A fetch
    // notices the stop at its next look (a hop, the pipe's room, libfetch's
    // queue); a start that truly failed would wait forever, which is the
    // price thread_start names.
    for (bool audio_done = !s_audio.started;;) {
        os64_lock_acquire(&s_audio.lock);
        if (!audio_done && s_audio.done) {
            audio_done = true;
            s_audio.owner = NULL;
        }
        os64_lock_release(&s_audio.lock);
        if (audio_done)
            yonder_media_reap();
        os64_lock_acquire(&s_audio.lock);
        bool left = !audio_done || s_audio.sessions != NULL;
        os64_lock_release(&s_audio.lock);
        if (!left)
            break;
        (void)os64_sleep(STOP_POLL_MS);
    }
    if (s_audio.started)
        os64_bell_close(&s_audio.bell);
    s_audio.started = false;
    s_audio.done = false;
}
