// media.c — the sound behind a page's media elements (media.h).
//
// THREADS AND WHAT EACH OWNS.
//
//   window    opens, closes, plays, pauses, seeks; reads facts; reaps;
//             every call into a session's player once it is made
//             (os64/media_player.h): the opener makes it, and closes it
//             itself only when the window's close came first (then the
//             window never calls it, `player_closed`).
//   opener    one per session, for its first steps (the file opened, or
//             the fetch begun and its head waited for), then the player
//             made over the bytes, and done.
//   player    libmedia's threads: its decoding thread owns the bytes kept
//             (`bytes` and its window) from the opener's end, reading them
//             through store_read and store_seek and letting an idle fetch
//             go through store_idle; a <video>'s player converts pictures
//             on a second thread.
//   loader    one at a time per session whose address is not a file (the
//             network, or a data: URL), at most LOADERS_MAX at once: runs
//             the fetch job, whose body goes down the pipe the bytes'
//             reader drains. The reader begins another for a seek far from
//             the bytes kept (a Range request, or a plain one from 0), and
//             to resume, from the window's end, a fetch let go while
//             nothing played or broken part way.
//
// THE SOUND is the player's voice on yonder's output (os64/media_output.h):
// libmedia's one audio thread for the program owns the device and mixes
// every session's voice. An <audio>'s voice WAITs while the device is
// refused (busy, as when another program has the stream: AUDIO_YONDER.md
// § When the stream is busy) and plays on from where it was when the
// device comes back; a <video>'s PLAYs ON, unheard, on the player's clock
// (VIDEO.md § 5).
//
// A session's `lock` guards what more than one thread touches: the asks
// (play, seek, settings, stop), the facts, the loader's product.
// `s_audio.lock` guards the session list and the loader slots taken. No
// thread is joined (thread_start): an opener or a loader says it is done as
// its last touch of its session, and a player says `finished`; only then
// is the session freed. LOCK ORDER: s_audio.lock, then the output's, then
// a player's, then a session's; never the other way, so no call into the
// output or a player is made with a session's lock held. A bell is rung
// only with the lock that guards its owner's right to close it held
// (os64/bell.h): a session's under its lock.

#include "media.h"

#include "fetch_job.h"
#include "fetch_pipe.h"
#include "media_bytes.h"
#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/lock.h"
#include "os64/media.h"
#include "os64/media_output.h"
#include "os64/media_player.h"
#include "os64/os64.h"
#include "os64/thread.h"
#include "os64/url.h"

#include <math.h>

#define RATE OS64_AUDIO_RATE
// The most of a resource kept (media_bytes.h's window): every arrived byte
// is, so a seek back costs nothing, until a longer one slides the window
// forward, keeping KEPT_BEHIND before the reader. The window takes from the
// fetch only while it holds less than KEPT_AHEAD past the reader; the rest
// waits on the pipe's backpressure, so a long sound is read at most that
// and the pipe's window ahead of the ear, not downloaded whole.
#define KEPT_MAX ((uint64_t)256 << 20)
#define KEPT_BEHIND ((uint64_t)16 << 20)
#define KEPT_AHEAD ((uint64_t)32 << 20)
// A session not playing whose fetch is still running lets the fetch go
// after this long, if the server answers ranges: its connection and loader
// slot go to a sound that is wanted. The reader reaching the window's end
// (or a seek past it) asks again from there.
#define HOLD_MS 10000u
// How many sounds are fetched at once; the rest wait their turn. A page may
// hold dozens of players, and a fetch costs a connection, or a bell while
// libfetch queues it, out of the program's sixty-four handles. Fewer than
// libfetch's six to one host (FETCH_POOL_PER_KEY), because a fetch held
// off by the pipe's backpressure keeps its connection while it is held
// (only HOLD_MS when nothing plays and the server answers ranges), and the
// page's other fetches from that host need one left (DEBTS.md § A page's
// sound).
#define LOADERS_MAX 4u
// The size a <video>'s pictures are converted to before any paint
// tells the fitted one: the picture's own, held to this a side.
#define PICTURE_NATURAL_MAX 1920u
// How often yonder's end looks for the threads to have let go.
#define STOP_POLL_MS 10u

// What the bytes' reader has heard of the last request, for a failure's
// record (yonder_media_failure_take): where it began (0: the first, else a
// Range request), its answer's status and Content-Type (0 and "" before
// its head), the bytes it brought, and the first of them, which say what
// the answer is when libmedia could not read it. Each request begins them
// again (loader_start).
typedef struct {
    int status;
    char type[48];
    uint64_t received;
    uint64_t from;
    uint8_t first[YONDER_MEDIA_FIRST_BYTES];
    uint32_t nfirst;
} MediaHeard;

struct yonder_media {
    yonder_media_t *next;           // s_audio.sessions
    os64_lock_t lock;
    os64_bell_t bell;               // the bytes' reader waits on it (store_read), as does its player's decoding thread (lent)
    // Asked, under `lock`.
    bool stop;
    bool play;                      // the element wants sound
    double gain;
    bool loop;
    bool seek_asked;
    double seek_to;
    uint64_t seek_generation;
    bool muted;
    // Its player, made by the opener and driven by the window. `picture`:
    // the element is a <video>. `seek_asked` is a seek waiting for the
    // player.
    bool picture;
    os64_media_player_t *player;    // NULL until made; set once, under `lock`
    bool player_closed;             // os64_media_player_close has been called
    bool player_finished;           // its `finished`: it reads no byte again
    bool player_told;               // the asks made before it existed have been given to it
    bool player_loaded;             // os64_media_player_loaded told
    bool player_unknown;            // its create failed with its thread perhaps running: never freed
    bool size_told;                 // a conversion size was told (yonder_media_size, or the natural one)
    bool probe_told;                // the window has put the probe's length into `facts`
    bool bytes_failed;              // the bytes' failure (`failed`, `why`), copied under `lock` by
    char bytes_why[96];             // the thread that owns them, for the window to read
    bool seek_landing;              // a DOM seek waits for the player's generation below
    uint32_t seek_player_generation;
    bool enough_held;               // HAVE_ENOUGH_DATA reached while it plays: held until it buffers
    uint32_t ready_held;            // the readiness as a DOM seek was asked (bits: current, future,
                                    // enough), told until it lands; the window's alone
    // Told, under `lock`.
    os64_dom_media_facts_t facts;
    // The threads, which hold no handles (thread_start). A session is
    // freed only once each one that may be running has said it is done.
    void *product;                  // the loader's, set once as it ends
    bool loader_started;            // a loader may be running
    bool loader_quit;               // the bytes' reader gave up on it: cancel the fetch
    bool loader_done;               // the loader's last touch of the session
    bool opener_done;               // the opener's last touch of the session
    bool closed;                    // the window let go of it
    // The bytes' reader's own (the opener's, then the player's decoding
    // thread's).
    char *url;
    // A blob: address's entry (BLOB_URLS.md), held for the session: each
    // loader job holds it too, and answers from it.
    struct os64_dom_blob *blob;
    char *page_url;
    const char *agent;              // borrowed (yonder_media_request_t)
    const way_identity_t *identity;
    uint64_t page;
    os64_dom_media_cors_t cors;
    yonder_fetch_job_t *job;
    yonder_fetch_pipe_t *pipe;
    int32_t file;
    // The bytes kept (media_bytes.h): `bytes` holds the window's, `cap`
    // long; `at` is the reader's offset in the resource.
    uint8_t *bytes;
    size_t cap;
    yonder_media_window_t window;
    uint64_t at;
    int64_t size;                   // the resource's length, -1 unknown
    uint64_t asked_from;            // where the running fetch's body begins
    bool headed;                    // the running fetch's head is taken
    bool ranges;                    // the server answers Range requests
    char validator[128];            // the whole answer's version (yonder_media_validator), "" none
    bool held;                      // the fetch was let go on purpose: resume at the window's end
    uint64_t idle_since;            // ms the fetch has been holdable since (store_idle), 0 not
    bool arrived;                   // the resource's end is in the window
    bool failed, opening;
    char why[96];
    MediaHeard heard;               // the reader's; told to `heard_told` under `lock`
    // A failure's record, the window's: the element and its address as
    // the record names them, given at open.
    char element[32];
    char *named;
    // Under `lock`: what the reader had heard when it last told (bytes_arrival,
    // fail), what libmedia said and named of a failed open or decode
    // (session_facts), and whether the failure is on the record.
    MediaHeard heard_told;
    int open_result;
    os64_media_info_t open_info;
    bool failure_told;
};

static struct {
    os64_lock_t lock;
    os64_media_output_t *output;    // made at the first open, ended by yonder_media_stop
    uint32_t loaders;               // loader slots taken (LOADERS_MAX)
    yonder_media_t *sessions;
} s_audio;

static yonder_media_env_t s_env;

static void ring_window(void)
{
    if (s_env.ring != NULL)
        s_env.ring();
}

// Rings whoever reads a session's bytes, its READER (the opener, then its
// player's decoding thread, waiting in store_read or parked); `m->lock` held.
static void ring_reader_locked(yonder_media_t *m)
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

// A loader slot let go: every session's reader is rung, for one may be waiting for
// it. Touches no session's memory but under the list's lock, so a loader
// may call it after its last touch of its own.
static void loader_slot_release(void)
{
    os64_lock_acquire(&s_audio.lock);
    if (s_audio.loaders > 0)
        s_audio.loaders--;
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next) {
        os64_lock_acquire(&m->lock);
        ring_reader_locked(m);
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

// ── The bytes (their reader: the opener, then the player's decoding thread) ──

static bool loader_start(yonder_media_t *m, uint64_t from);

// `n` more bytes at the window's end, the front let go first when they
// would pass KEPT_MAX. Fails with no memory, or when even that leaves no
// room: the reader that far behind a streamed fetch (KEPT_AHEAD keeps it
// from happening), or a whole answer longer than KEPT_MAX.
static bool keep(yonder_media_t *m, const uint8_t *data, size_t n)
{
    uint64_t drop = yonder_media_drop_for(&m->window, n, m->at, KEPT_MAX, KEPT_BEHIND);
    if (drop == UINT64_MAX) {
        os64_strcopy(m->why, sizeof(m->why), "The sound arrived further ahead than yonder keeps");
        return false;
    }
    if (drop > 0) {
        os64_memmove(m->bytes, m->bytes + drop, (size_t)(m->window.have - drop));
        m->window.base += drop;
        m->window.have -= drop;
    }
    size_t have = (size_t)m->window.have;
    if (have + n > m->cap) {
        size_t cap = m->cap != 0 ? m->cap : (size_t)1 << 20;
        while (cap < have + n)
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
    os64_memcpy(m->bytes + have, data, n);
    m->window.have += n;
    m->heard.received += n;
    for (size_t i = 0; i < n && m->heard.nfirst < YONDER_MEDIA_FIRST_BYTES; i++)
        m->heard.first[m->heard.nfirst++] = data[i];
    return true;
}

// The running fetch's head: its status, where its body begins, the
// resource's length, and whether the server answers ranges. A Range
// request answered whole (200) is a server that does not: the window
// begins again at 0, and no seek asks it for a range again. False when
// the head fails the load (`why` says why).
static bool head_taken(yonder_media_t *m, const yonder_fetched_t *head)
{
    m->heard.status = head->status;
    yonder_media_content_type(head->headers, head->headers_len, m->heard.type, sizeof(m->heard.type));
    if (head->status != 0 && (head->status < 200 || head->status > 299)) {
        os64_snprintf(m->why, sizeof(m->why), "The server answered %d", (int)head->status);
        return false;
    }
    uint64_t first = 0;
    int64_t total = -1;
    char validator[sizeof(m->validator)];
    yonder_media_validator(head->headers, head->headers_len, validator, sizeof(validator));
    if (head->status == 206) {
        if (!yonder_media_content_range(head->headers, head->headers_len, &first, &total) ||
            first != m->asked_from) {
            os64_strcopy(m->why, sizeof(m->why), "The server answered with bytes not asked for");
            return false;
        }
        // The bytes must be the same resource's as the ones kept, which the
        // reader has already read: a file replaced on the server since
        // would be spliced into the old one.
        if ((m->validator[0] != '\0' && validator[0] != '\0' && !os64_streq(validator, m->validator)) ||
            (total >= 0 && m->size >= 0 && total != m->size)) {
            os64_strcopy(m->why, sizeof(m->why), "The sound changed on the server while it played");
            return false;
        }
        m->ranges = true;
        if (total >= 0)
            m->size = total;
        return true;
    }
    if (m->asked_from > 0) {
        m->ranges = false;
        m->window = (yonder_media_window_t){0, 0};
    } else {
        m->ranges = yonder_media_accepts_ranges(head->headers, head->headers_len);
    }
    os64_strcopy(m->validator, sizeof(m->validator), validator);
    m->size = yonder_media_content_length(head->headers, head->headers_len);
    return true;
}

// What has come down the pipe, kept, while the window holds less than
// `ahead` past the reader (pump: KEPT_AHEAD); and the loader's whole
// answer, when the cache or a data: URL gave one. True when anything
// changed.
static bool pump_ahead(yonder_media_t *m, uint64_t ahead)
{
    bool moved = false;
    if (m->file >= 0 || m->arrived || m->failed || m->held)
        return false;
    if (m->pipe != NULL) {
        for (;;) {
            yonder_fetch_pipe_state_t state;
            yonder_fetch_pipe_look(m->pipe, &state);
            if (state.headed && !m->headed) {
                m->headed = true;
                moved = true;
                if (!head_taken(m, yonder_fetch_pipe_headed(m->pipe))) {
                    m->failed = true;
                    return true;
                }
            }
            // Past the lead the rest waits in the pipe, which holds the
            // fetch off: the reader's next move takes more.
            if (state.unread > 0 && !yonder_media_take_more(&m->window, m->at, ahead))
                break;
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
            if (state.ended && state.headed) {
                uint64_t end = m->window.base + m->window.have;
                // A part a server bounded (206 for less than asked) ends
                // whole before the resource does: the rest is asked for.
                bool short_part = m->ranges && m->size >= 0 && end < (uint64_t)m->size;
                if (state.ok && !short_part) {
                    m->arrived = true;
                } else if (m->ranges && end > m->asked_from) {
                    // That, or broken part way after bringing something (a
                    // server dropping a connection held off for minutes):
                    // it is asked again from the window's end when the
                    // reader gets there, as a fetch let go on purpose is.
                    m->held = true;
                } else {
                    m->failed = true;
                    os64_strcopy(m->why, sizeof(m->why), "The connection failed part way");
                }
                return true;
            }
            yonder_fetch_pipe_ask(m->pipe);
            break;
        }
    }
    // The job's end, for an answer that never went down the pipe: once a
    // streamed head is taken, the pipe's end says how the body ended,
    // after every byte it holds.
    os64_lock_acquire(&m->lock);
    bool done = m->loader_done;
    yonder_fetched_t *got = m->product;
    os64_lock_release(&m->lock);
    if (done && !m->arrived && !m->failed && !m->headed) {
        if (got != NULL) {
            m->heard.status = got->status;
            yonder_media_content_type(got->headers, got->headers_len, m->heard.type, sizeof(m->heard.type));
        }
        if (got == NULL || !got->ok) {
            if (got != NULL && got->why[0] != '\0')
                os64_snprintf(m->why, sizeof(m->why), "The sound could not be fetched: %s", got->why);
            else
                os64_strcopy(m->why, sizeof(m->why), "The sound could not be fetched");
            m->failed = true;
        } else if (!got->streamed) {
            if (got->status != 0 && (got->status < 200 || got->status > 299)) {
                os64_snprintf(m->why, sizeof(m->why), "The server answered %d", (int)got->status);
                m->failed = true;
            } else if (got->body != NULL && !keep(m, got->body, got->body_len)) {
                m->failed = true;
            } else {
                m->size = (int64_t)(m->window.base + m->window.have);
                m->arrived = true;
            }
        } else {
            // Streamed, but its head was never published: nothing will come.
            os64_strcopy(m->why, sizeof(m->why), "The answer ended before its head came");
            m->failed = true;
        }
        moved = true;
    }
    return moved;
}

static bool pump(yonder_media_t *m)
{
    return pump_ahead(m, KEPT_AHEAD);
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
static bool reader_wait(yonder_media_t *m, uint64_t ms)
{
    (void)os64_bell_wait(&m->bell, ms);
    os64_lock_acquire(&m->lock);
    bool stop = m->stop;
    os64_lock_release(&m->lock);
    return !stop;
}

// A loader runs, or ran and has not been let go.
static bool fetch_running(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    bool running = m->loader_started;
    os64_lock_release(&m->lock);
    return running;
}

// Whether the running fetch may be let go for now: it is still bringing
// bytes, the server answers ranges, so it can be resumed where it stops,
// and nothing wants the sound.
static bool fetch_holdable(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    bool idle = m->loader_started && !m->loader_done && !m->play;
    os64_lock_release(&m->lock);
    return idle && m->ranges && !m->held && !m->arrived && !m->failed;
}

// The running fetch stopped and let go of, once its loader has let go of
// the session. False when the session stops first.
static bool fetch_stop(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    if (m->loader_started && !m->loader_done)
        m->loader_quit = true;
    os64_lock_release(&m->lock);
    for (;;) {
        os64_lock_acquire(&m->lock);
        bool done = !m->loader_started || m->loader_done;
        os64_lock_release(&m->lock);
        if (done)
            break;
        if (!reader_wait(m, OS64_WAIT_FOREVER))
            return false;
    }
    os64_lock_acquire(&m->lock);
    yonder_fetch_job_t *job = m->job;
    void *product = m->product;
    m->job = NULL;
    m->product = NULL;
    m->loader_started = m->loader_done = m->loader_quit = false;
    os64_lock_release(&m->lock);
    yonder_fetch_release(job, product);
    if (m->pipe != NULL)
        yonder_fetch_pipe_release(m->pipe);
    m->pipe = NULL;
    m->headed = false;
    return true;
}

// The fetch begun again at `from` (a Range request past 0): a resume when
// the window ends there, else a new window there. False when it could not
// begin (`why` says why) or the session stopped.
static bool fetch_restart(yonder_media_t *m, uint64_t from)
{
    if (!fetch_stop(m))
        return false;
    if (from != m->window.base + m->window.have)
        m->window = (yonder_media_window_t){from, 0};
    m->arrived = false;
    m->held = false;
    if (!loader_start(m, from)) {
        os64_strcopy(m->why, sizeof(m->why), "The sound could not be fetched");
        m->failed = true;
        return false;
    }
    return true;
}

static void bytes_arrival(yonder_media_t *m);

static int64_t store_read(void *ctx, uint8_t *out, size_t cap)
{
    yonder_media_t *m = ctx;
    for (;;) {
        uint64_t end = m->window.base + m->window.have;
        if (m->at >= m->window.base && m->at < end) {
            uint64_t n = end - m->at;
            if (n > cap)
                n = cap;
            os64_memcpy(out, m->bytes + (m->at - m->window.base), (size_t)n);
            m->at += n;
            bytes_arrival(m);
            return (int64_t)n;
        }
        bytes_arrival(m);
        if (m->failed)
            return -1;
        if (m->arrived)
            return 0;
        // A fetch let go (on purpose, or broken part way) is resumed where
        // it stopped, unless the window already holds the resource's end:
        // a fetch let go just as its last bytes came.
        if (m->held && m->size >= 0 && end >= (uint64_t)m->size) {
            m->held = false;
            m->arrived = true;
            continue;
        }
        if (m->held && !fetch_restart(m, end)) {
            bytes_arrival(m);       // a restart that failed is the bytes' failure, told
            return -1;
        }
        if (!pump(m) && !reader_wait(m, 200))
            return -1;
    }
}

// A seek inside the window is free; one far from it begins a new window
// there when the server answers ranges; one a little past it, or past it
// on a server that does not, moves the reader there and the next read
// waits for the bytes (media_bytes.h; the reader's place is what lets
// pump take them past KEPT_AHEAD). None ever waits or asks while the media
// is being opened: a reader probing the file's end, as the MP3 reader does
// for its tags, carries on without them. Nothing past the resource's known
// length is asked for.
static int64_t store_seek(void *ctx, uint64_t offset)
{
    yonder_media_t *m = ctx;
    if (m->failed)
        return -1;
    // `opening` is ended by the player's first look (player_look), on
    // another thread, so it is read under the lock.
    os64_lock_acquire(&m->lock);
    bool opening = m->opening;
    os64_lock_release(&m->lock);
    yonder_media_seek_state_t s = {opening, m->ranges, fetch_running(m) && !m->held, m->held, m->arrived};
    yonder_media_seek_t plan = yonder_media_seek_plan(&m->window, offset, &s);
    if (plan != YONDER_MEDIA_SEEK_HERE && m->size >= 0 && offset >= (uint64_t)m->size)
        plan = YONDER_MEDIA_SEEK_REFUSE;
    if (plan == YONDER_MEDIA_SEEK_REFUSE)
        return -1;
    if (plan == YONDER_MEDIA_SEEK_JUMP && !fetch_restart(m, offset)) {
        bytes_arrival(m);
        return -1;
    }
    m->at = offset;
    return (int64_t)offset;
}

// The player's decoding thread waits with nothing to read (os64/media.h's
// `idle`, at each wake while it waits, 200 ms apart at most): the window
// takes what the pipe holds, up to its lead (pump), and a fetch still
// running for a session nobody plays is let go after HOLD_MS (store_read
// resumes it), so a paused podcast does not keep a connection and a loader
// slot for the evening.
static void store_idle(void *ctx)
{
    yonder_media_t *m = ctx;
    if (pump(m))
        bytes_arrival(m);
    if (!fetch_holdable(m)) {
        m->idle_since = 0;
        return;
    }
    uint64_t now = (uint64_t)os64_micros() / 1000;
    if (m->idle_since == 0)
        m->idle_since = now;
    if (now - m->idle_since < HOLD_MS)
        return;
    // What the pipe already holds is kept first, past the usual lead: it
    // was paid for.
    (void)pump_ahead(m, UINT64_MAX);
    if (fetch_stop(m))
        m->held = !m->arrived && !m->failed;   // the pipe may have brought the end, or a failure
    m->idle_since = 0;
    bytes_arrival(m);
}

// ── The loader thread ──────────────────────────────────────────────────

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
    ring_reader_locked(m);
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
        if (!reader_wait(m, OS64_WAIT_FOREVER))
            return false;
    }
}

static void pipe_ring(void *ctx)
{
    yonder_media_t *m = ctx;
    os64_lock_acquire(&m->lock);
    ring_reader_locked(m);
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

// The fetch job for a network address, its body from byte `from` on (a
// Range request past 0; the cache answers none) streamed to the reader,
// once a loader slot is free. False when the fetch could not begin; then
// `loader_started` says whether a loader may be running all the same, and
// the job and the pipe stay the session's until it is freed.
static bool loader_start(yonder_media_t *m, uint64_t from)
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
    job->blob = os64_dom_blob_hold(m->blob);
    job->headers = os64_calloc(1, 48);
    if (job->headers != NULL && from > 0) {
        os64_snprintf(job->headers, 48, "Range: bytes=%llu-\r\n", (unsigned long long)from);
        job->headers_len = os64_strlen(job->headers);
    }
    m->asked_from = from;
    m->heard = (MediaHeard){.from = from};
    m->headed = false;
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
    // lets go of the writer's side, and the session's reader.
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

// The fetch's progress told (buffered's range, loaded); `m->lock` held, on
// the thread that owns the bytes.
static void arrival_locked(yonder_media_t *m)
{
    bool loaded = m->file >= 0 || (m->arrived && m->window.base == 0);
    m->facts.loaded = loaded;
    if (m->facts.probed && isfinite(m->facts.duration)) {
        // The window, as a share of the length: it starts past 0 after a
        // seek far ahead, or once a long sound has slid it.
        double length = m->size > 0 ? (double)m->size : 0;
        m->facts.arrived = loaded ? m->facts.duration
                           : length > 0 ? m->facts.duration * (double)(m->window.base + m->window.have) / length : 0;
        m->facts.arrived_from = loaded || !(length > 0) ? 0 : m->facts.duration * (double)m->window.base / length;
    }
}

// A session's bytes moved (store_read, store_seek): the arrival told, for
// the window's facts.
static void bytes_arrival(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    arrival_locked(m);
    m->heard_told = m->heard;
    if (m->failed && !m->bytes_failed) {
        m->bytes_failed = true;
        os64_strcopy(m->bytes_why, sizeof(m->bytes_why), m->why);
    }
    os64_lock_release(&m->lock);
    product_trim(m);
}

// The session failed before it had a player, on the bytes' reader's
// thread: `why` is the page's MediaError.message, and a failure with none
// says so rather than nothing.
static void fail(yonder_media_t *m, int code, const char *why)
{
    os64_lock_acquire(&m->lock);
    m->heard_told = m->heard;
    if (m->facts.error == 0) {
        m->facts.error = code;
        os64_strcopy(m->facts.message, sizeof(m->facts.message),
                     why[0] != '\0' ? why : "The media failed, and nothing said why");
    }
    os64_lock_release(&m->lock);
    ring_window();
}

static int open_error(int result)
{
    return result == OS64_MEDIA_DECODE ? OS64_DOM_MEDIA_ERR_DECODE : OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED;
}

// A session's first steps, on the thread that will own its bytes: the file
// opened, or the fetch begun and its head waited for (or the answer,
// whole), and `source` made over them. False when the session failed
// (`fail` has told why) or was closed before it began.
static bool source_prepare(yonder_media_t *m, os64_media_source_t *out)
{
    os64_media_source_t source = {0};
    bool network = !is_scheme(m->url, "file:");
    os64_lock_acquire(&m->lock);
    bool stopped = m->stop;
    os64_lock_release(&m->lock);
    if (stopped) {
        // Closed before it ran (a start that answered an error): nothing
        // to set up.
    } else if (!network) {
        char path[OS64_URL_REF_MAX];
        int64_t h = os64_open(file_path(m->url, path, sizeof(path)), "r");
        m->file = h >= 0 ? (int32_t)h : -1;
        if (m->file < 0)
            fail(m, OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, "The file would not open");
        else
            os64_media_file_source(m->file, &source);
    } else if (!loader_start(m, 0)) {
        fail(m, OS64_DOM_MEDIA_ERR_NETWORK, "The sound could not be fetched");
    } else {
        // The size is the head's: wait for it (or for the answer, whole).
        while (!m->headed && !m->failed && !m->arrived)
            if (!pump(m) && !reader_wait(m, 200))
                break;
        // Remote while bytes are still to come: an answer whole already (a
        // data: URL, the cache, a short sound) is read in memory, exactly.
        source = (os64_media_source_t){m, store_read, store_seek, m->size, !m->arrived, store_idle};
        if (m->failed)
            fail(m, OS64_DOM_MEDIA_ERR_SRC_NOT_SUPPORTED, m->why);
    }
    *out = source;
    os64_lock_acquire(&m->lock);
    bool go = m->facts.error == 0 && !m->stop;
    os64_lock_release(&m->lock);
    return go;
}

// ── A session's opener and its player ──────────────────────────────────

// The player's facts changed: under the player's lock, so it only rings.
// Its first look is its open's end (the state leaves OPENING, or it
// fails): the bytes' reader no longer opens, so a seek past the window may
// wait for its bytes (store_seek). Under the player's lock, which is taken
// before a session's.
static void player_look(void *ctx)
{
    yonder_media_t *m = ctx;
    os64_lock_acquire(&m->lock);
    m->opening = false;
    os64_lock_release(&m->lock);
    ring_window();
}

// The player will never read the session's bytes again, and is freed.
static void player_finished(void *ctx)
{
    yonder_media_t *m = ctx;
    os64_lock_acquire(&m->lock);
    m->player_finished = true;
    ring_window();
    os64_lock_release(&m->lock);
}

static int64_t opener_main(void *arg)
{
    yonder_media_t *m = arg;
    os64_media_source_t source = {0};
    if (source_prepare(m, &source)) {
        os64_lock_acquire(&m->lock);
        arrival_locked(m);
        m->opening = true;          // until the player's first look (player_look)
        bool loaded = m->facts.loaded;
        os64_lock_release(&m->lock);
        // A <video> wants its picture and plays on unheard when the device
        // is refused; an <audio> wants its sound and waits (VIDEO.md § 5).
        os64_media_player_config_t cfg = {
            .source = source,
            .want = OS64_MEDIA_WANT_SOUND | (m->picture ? OS64_MEDIA_WANT_PICTURE : 0u),
            .output = s_audio.output,
            .refused = m->picture ? OS64_MEDIA_VOICE_PLAY_ON : OS64_MEDIA_VOICE_WAIT,
            .loaded = loaded, .look = player_look, .finished = player_finished, .ctx = m,
            // One bell for the bytes' reader and the player's decoding
            // thread, which are one thread: a page of many players spends
            // one handle on each, as it did before the player (DEBTS.md
            // § A page's sound).
            .bell = &m->bell};
        os64_media_player_t *player = NULL;
        if (os64_media_player_create(&cfg, &player) != 0) {
            // A create that failed may have left its decoding thread running
            // detached (os64_media_player_create), reading these bytes: the
            // session is never freed, the price that start names.
            os64_lock_acquire(&m->lock);
            m->player_unknown = true;
            os64_lock_release(&m->lock);
            fail(m, OS64_DOM_MEDIA_ERR_NETWORK, "No memory for the player");
        } else {
            // Published under the lock the window's close reads it under: a
            // close that came first finds no player, and it is closed here.
            os64_lock_acquire(&m->lock);
            m->player = player;
            bool closed = m->stop;
            m->player_closed = closed;
            os64_lock_release(&m->lock);
            if (closed)
                os64_media_player_close(player);
            ring_window();
        }
    }
    os64_lock_acquire(&m->lock);
    m->opener_done = true;
    ring_window();
    os64_lock_release(&m->lock);
    return 0;
}

// An element's volume, muted and loop, as its player takes them (a muted
// <video>'s voice goes; a muted <audio> is a silent voice: os64_media_player_mute).
static void player_settings(os64_media_player_t *p, double gain, bool muted, bool loop)
{
    os64_media_player_mute(p, muted);
    os64_media_player_gain(p, gain);
    os64_media_player_loop(p, loop);
}

// The asks made before the player existed, given to it once (window).
static void player_tell(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    os64_media_player_t *p = m->player_told || m->player_closed ? NULL : m->player;
    if (p != NULL)
        m->player_told = true;
    bool play = m->play, muted = m->muted, seek = m->seek_asked, loop = m->loop;
    double gain = m->gain, to = m->seek_to;
    if (p != NULL)
        m->seek_asked = false;      // kept for the player until there is one
    os64_lock_release(&m->lock);
    if (p == NULL)
        return;
    player_settings(p, gain, muted, loop);
    if (seek) {
        uint32_t generation = os64_media_player_seek(p, (int64_t)((to > 0 ? to : 0) * RATE + 0.5));
        os64_lock_acquire(&m->lock);
        m->seek_player_generation = generation;
        os64_lock_release(&m->lock);
    }
    if (play)
        os64_media_player_play(p);
}

// The player, when the window may drive it (made, told, not closed).
static os64_media_player_t *player_of(yonder_media_t *m)
{
    player_tell(m);
    os64_lock_acquire(&m->lock);
    os64_media_player_t *p = m->player_told && !m->player_closed ? m->player : NULL;
    os64_lock_release(&m->lock);
    return p;
}

// A session's facts, from its player's and its bytes' (window).
static void session_facts(yonder_media_t *m, os64_dom_media_facts_t *out)
{
    os64_media_player_t *p = player_of(m);
    os64_lock_acquire(&m->lock);
    *out = m->facts;
    os64_lock_release(&m->lock);
    if (p == NULL)
        return;                     // opening, or failed before it had a player (`fail` told)
    os64_media_player_facts_t f;
    os64_media_player_facts(p, &f);
    bool probed = f.probed && f.failure == 0;
    if (f.failure != 0 && out->error == 0) {
        // A failed open leaves no kind playable (the info is the open's):
        // it fails as the open's code says (open_error); a failure
        // after it is a decode's; the bytes' own failure is the network's.
        bool opened = f.info.sound == OS64_MEDIA_PLAYABLE || f.info.picture == OS64_MEDIA_PLAYABLE;
        out->error = opened ? OS64_DOM_MEDIA_ERR_DECODE : open_error(f.failure);
        os64_lock_acquire(&m->lock);
        if (m->facts.error == 0 && m->open_result == 0) {
            m->open_result = f.failure;
            m->open_info = f.info;
        }
        bool network = m->bytes_failed;
        if (network) {
            out->error = OS64_DOM_MEDIA_ERR_NETWORK;
            os64_strcopy(out->message, sizeof(out->message), m->bytes_why);
        } else {
            os64_strcopy(out->message, sizeof(out->message), os64_media_strerror(f.failure));
        }
        os64_lock_release(&m->lock);
    }
    os64_lock_acquire(&m->lock);
    bool tell_size = false;
    if (f.probed && !m->probe_told) {
        m->probe_told = true;
        m->facts.probed = probed;
        m->facts.duration = f.length >= 0 ? (double)f.length / RATE : INFINITY;
        // Buffered's range is a share of the duration: a file's is all of
        // it, which the window may say; a fetch's is told by the thread
        // that owns its bytes, at its next read (bytes_arrival).
        if (m->file >= 0)
            arrival_locked(m);
        // A picture is converted at some size even before a paint tells
        // the fitted one, or one never painted (below the fold, hidden, not
        // in the document) would never have a picture to be ready with.
        tell_size = m->picture && !m->size_told && f.info.picture == OS64_MEDIA_PLAYABLE && f.info.width > 0;
        m->size_told |= tell_size;
    }
    if (m->probe_told && f.length >= 0 && isinf(m->facts.duration)) {
        // A length the probe could not say, learned at the end (libdom
        // fires durationchange; seekable follows).
        m->facts.duration = (double)f.length / RATE;
        if (m->file >= 0)
            arrival_locked(m);
    }
    if (m->seek_landing && (int32_t)(f.seeked - m->seek_player_generation) >= 0) {
        m->seek_landing = false;
        m->facts.seeked = m->seek_generation;
    }
    if (out->error != 0 && m->facts.error == 0) {
        m->facts.error = out->error;
        os64_strcopy(m->facts.message, sizeof(m->facts.message), out->message);
        // Nothing will read another byte of it: the fetch is let go.
        if (m->loader_started && !m->loader_done)
            m->loader_quit = true;
    }
    bool tell_loaded = m->facts.loaded && !m->player_loaded;
    m->player_loaded |= tell_loaded;
    *out = m->facts;
    os64_lock_release(&m->lock);
    if (tell_loaded)
        os64_media_player_loaded(p, true);
    if (tell_size) {
        uint32_t w = f.info.width, h = f.info.height;
        if (w > PICTURE_NATURAL_MAX || h > PICTURE_NATURAL_MAX) {
            double scale = (double)PICTURE_NATURAL_MAX / (w > h ? w : h);
            w = (uint32_t)(w * scale);
            h = (uint32_t)(h * scale);
        }
        os64_media_player_size(p, w > 0 ? w : 1, h > 0 ? h : 1);
    }
    out->probed = probed;
    out->seekable = f.info.seekable;
    // The readiness libdom's readyState is made from (VIDEO.md § 5's table):
    // the queues' while paused; while the player PLAYS at least FUTURE
    // (its queues drain and refill as it plays, which is no wait), and
    // ENOUGH once reached until it buffers; a buffering player is
    // CURRENT, which is libdom's `waiting`.
    bool playing = f.state == OS64_MEDIA_PLAYER_PLAYING;
    if (f.state == OS64_MEDIA_PLAYER_BUFFERING || !playing)
        m->enough_held = false;     // the window's own; read and written here alone
    m->enough_held |= playing && f.enough;
    out->current = f.current || playing;
    out->future = playing || (f.future && f.state != OS64_MEDIA_PLAYER_BUFFERING);
    out->enough = m->enough_held || (f.enough && f.state != OS64_MEDIA_PLAYER_BUFFERING);
    // A seek leaves readyState as it was until it lands (the table's "as
    // is"); then the readiness is the new place's.
    os64_lock_acquire(&m->lock);
    bool landing = m->seek_landing;
    os64_lock_release(&m->lock);
    if (landing) {
        out->current = (m->ready_held & 1u) != 0;
        out->future = (m->ready_held & 2u) != 0;
        out->enough = (m->ready_held & 4u) != 0;
    }
    // The facts kept are what libdom was told (a seek's held readiness
    // is taken from them).
    os64_lock_acquire(&m->lock);
    m->facts.current = out->current;
    m->facts.future = out->future;
    m->facts.enough = out->enough;
    os64_lock_release(&m->lock);
    out->busy = f.busy;
    out->sounding = f.sounding;
    out->running = f.state == OS64_MEDIA_PLAYER_PLAYING;
    // An <audio>'s player never decodes a picture the file may carry.
    out->picture = m->picture && f.info.picture == OS64_MEDIA_PLAYABLE;
    out->width = out->picture ? f.info.width : 0;
    out->height = out->picture ? f.info.height : 0;
    // With loop on the player runs on from its start (os64_media_player_loop)
    // and never ends, so libdom hears no end.
    out->ended = f.state == OS64_MEDIA_PLAYER_ENDED;
    out->position = (double)f.position / RATE;
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
    return s_audio.output != NULL || os64_media_output_create(&s_audio.output) == 0;
}

// Whether a session's threads have all let go of it, its player's
// included. Its lock held.
static bool threads_done_locked(const yonder_media_t *m)
{
    return m->opener_done && (!m->loader_started || m->loader_done) &&
           (m->player == NULL ? !m->player_unknown : m->player_finished);
}

// Frees a session no thread is using any more (threads_done_locked).
static void session_free(yonder_media_t *m)
{
    if (m->file >= 0)
        os64_close(m->file);        // the player never reads it again (player_finished)
    yonder_fetch_release(m->job, m->product);
    if (m->pipe != NULL)
        yonder_fetch_pipe_release(m->pipe);
    os64_bell_close(&m->bell);
    os64_free(m->bytes);
    os64_free(m->url);
    os64_free(m->named);
    os64_dom_blob_release(m->blob);
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
    m->size = -1;
    m->gain = 1;
    m->cors = r->cors;
    m->page = r->page;
    m->agent = r->agent;
    m->identity = r->identity;
    m->facts.duration = NAN;
    m->bell = (os64_bell_t)OS64_BELL_NONE;
    m->url = copy_text(r->url);
    m->blob = os64_dom_blob_hold(r->blob);
    m->page_url = copy_text(r->page_url != NULL ? r->page_url : "about:blank");
    m->picture = r->picture;
    os64_strcopy(m->element, sizeof(m->element), r->element != NULL ? r->element : (r->picture ? "video" : "audio"));
    m->named = copy_text(r->named != NULL ? r->named : r->url);
    if (m->url == NULL || m->page_url == NULL || m->named == NULL || os64_bell_make(&m->bell) != 0) {
        session_free(m);
        return NULL;
    }
    // On the list before the start, so an opener that runs at once is
    // already one a freed loader slot rings.
    os64_lock_acquire(&s_audio.lock);
    m->next = s_audio.sessions;
    s_audio.sessions = m;
    os64_lock_release(&s_audio.lock);
    if (!thread_start(opener_main, m)) {
        // The opener may be running all the same (thread_start): the
        // session is closed and left on the list, for the reap to free
        // once the opener says it is done.
        os64_lock_acquire(&m->lock);
        m->stop = true;
        m->closed = true;
        ring_reader_locked(m);
        os64_lock_release(&m->lock);
        return NULL;
    }
    return m;
}

// A session's player closed, once: its decoding thread's read fails on the
// stop already set (store_read), and it frees itself. Window.
static void player_close(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    os64_media_player_t *p = m->player_closed ? NULL : m->player;
    m->player_closed = true;
    os64_lock_release(&m->lock);
    if (p != NULL)
        os64_media_player_close(p);
}

void yonder_media_close(yonder_media_t *m)
{
    if (m == NULL)
        return;
    os64_lock_acquire(&m->lock);
    m->stop = true;
    m->play = false;
    m->closed = true;
    ring_reader_locked(m);
    os64_lock_release(&m->lock);
    player_close(m);
}

void yonder_media_play(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    m->play = true;
    os64_lock_release(&m->lock);
    os64_media_player_t *p = player_of(m);
    if (p != NULL)
        os64_media_player_play(p);
}

void yonder_media_pause(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    m->play = false;
    os64_lock_release(&m->lock);
    os64_media_player_t *p = player_of(m);
    if (p != NULL)
        os64_media_player_pause(p);
}

void yonder_media_seek(yonder_media_t *m, double seconds, uint64_t generation)
{
    // Held to a time the frame count can hold: a stream's duration is
    // infinite, so libdom clamps nothing (the player holds it to the end).
    if (!(seconds > 0))
        seconds = 0;
    if (seconds > 1e9)
        seconds = 1e9;
    os64_media_player_t *p = player_of(m);
    os64_lock_acquire(&m->lock);
    if (!m->seek_landing)
        m->ready_held = (m->facts.current ? 1u : 0u) | (m->facts.future ? 2u : 0u) | (m->facts.enough ? 4u : 0u);
    m->seek_landing = true;
    m->seek_generation = generation;
    m->seek_to = seconds;
    m->seek_asked = p == NULL;      // for the player, once it exists (player_tell)
    os64_lock_release(&m->lock);
    if (p != NULL) {
        uint32_t g = os64_media_player_seek(p, (int64_t)(seconds * RATE + 0.5));
        os64_lock_acquire(&m->lock);
        m->seek_player_generation = g;
        bool play = m->play;
        os64_lock_release(&m->lock);
        // A player sought from its end plays on (os64/media_player.h); an
        // element that is paused stays paused (libdom paused it there).
        if (!play)
            os64_media_player_pause(p);
    }
}

void yonder_media_settings(yonder_media_t *m, double gain, bool muted, bool loop)
{
    os64_lock_acquire(&m->lock);
    m->gain = gain < 0 ? 0 : gain > 1 ? 1 : gain;
    m->muted = muted;
    m->loop = loop;
    os64_lock_release(&m->lock);
    os64_media_player_t *p = player_of(m);
    if (p != NULL)
        player_settings(p, gain, muted, loop);
}

void yonder_media_facts(yonder_media_t *m, os64_dom_media_facts_t *out)
{
    session_facts(m, out);
}

bool yonder_media_failure_take(yonder_media_t *m, int code, char *out, size_t cap)
{
    os64_lock_acquire(&m->lock);
    bool take = m->facts.error != 0 && !m->failure_told;
    m->failure_told |= take;
    MediaHeard heard = m->heard_told;
    int error = code != 0 ? code : m->facts.error, result = m->open_result;
    os64_media_info_t info = m->open_info;
    char reason[sizeof(m->facts.message)];
    os64_strcopy(reason, sizeof(reason), m->facts.message);
    os64_lock_release(&m->lock);
    if (!take)
        return false;
    yonder_media_failure_t f = {
        .element = m->element, .error = error, .reason = reason, .fetched = !is_scheme(m->named, "file:"),
        .status = heard.status, .type = heard.type, .received = heard.received, .range_from = heard.from,
        .said = result != 0 ? os64_media_strerror(result) : "", .format = info.format, .sound = info.codec,
        .picture = info.picture_codec, .sound_unplayable = info.sound == OS64_MEDIA_UNPLAYABLE,
        .picture_unplayable = info.picture == OS64_MEDIA_UNPLAYABLE, .first = heard.first, .nfirst = heard.nfirst};
    char text[512];
    yonder_media_failure_text(&f, text, sizeof(text));
    os64_snprintf(out, cap, "%s: %s", m->named, text);
    return true;
}

bool yonder_media_frame(yonder_media_t *m, int64_t blank, int64_t period, os64_media_player_frame_t *out)
{
    os64_media_player_t *p = m->picture ? player_of(m) : NULL;
    if (p == NULL) {
        *out = (os64_media_player_frame_t){0};
        return false;
    }
    return os64_media_player_frame(p, blank, period, out);
}

bool yonder_media_player_facts(yonder_media_t *m, os64_media_player_facts_t *out)
{
    os64_media_player_t *p = player_of(m);
    if (p == NULL)
        return false;
    os64_media_player_facts(p, out);
    return true;
}

void yonder_media_size(yonder_media_t *m, uint32_t width, uint32_t height)
{
    os64_lock_acquire(&m->lock);
    m->size_told = true;
    os64_lock_release(&m->lock);
    os64_media_player_t *p = m->picture ? player_of(m) : NULL;
    if (p != NULL)
        os64_media_player_size(p, width, height);
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
        bool gone = m->closed && threads_done_locked(m);
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
        m->play = false;
        ring_reader_locked(m);
        os64_lock_release(&m->lock);
    }
    os64_lock_release(&s_audio.lock);
    // A player made after its session's close is closed by its opener;
    // the window closes the rest (each is a window's call).
    for (;;) {
        yonder_media_t *open = NULL;
        os64_lock_acquire(&s_audio.lock);
        for (yonder_media_t *m = s_audio.sessions; m != NULL && open == NULL; m = m->next) {
            os64_lock_acquire(&m->lock);
            if (m->player != NULL && !m->player_closed)
                open = m;
            os64_lock_release(&m->lock);
        }
        os64_lock_release(&s_audio.lock);
        if (open == NULL)
            break;
        player_close(open);
    }
    // No thread has a handle to join (thread_start): this waits for every
    // session to be freed, as its threads say they are done and its player
    // says `finished` (the output has let go of its voice), because the
    // environment they use is freed after it; then the output ends. A
    // fetch notices the stop at its next
    // look (a hop, the pipe's room, libfetch's queue); a start that truly
    // failed would wait forever, which is the price thread_start names.
    for (;;) {
        yonder_media_reap();
        os64_lock_acquire(&s_audio.lock);
        bool left = s_audio.sessions != NULL;
        os64_lock_release(&s_audio.lock);
        if (!left)
            break;
        (void)os64_sleep(STOP_POLL_MS);
    }
    if (s_audio.output != NULL)
        (void)os64_media_output_destroy(s_audio.output);
    s_audio.output = NULL;
}
