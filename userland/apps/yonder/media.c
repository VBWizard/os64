// media.c — the sound behind a page's media elements (media.h).
//
// THREADS AND WHAT EACH OWNS.
//
//   window    opens, closes, plays, pauses, seeks; reads facts; reaps.
//   decoder   one per session: the bytes kept (`bytes` and its window, its
//             alone), libmedia's object, and the pushes into the PCM queue.
//   loader    one at a time per session whose address is not a file (the
//             network, or a data: URL), at most LOADERS_MAX at once: runs
//             the fetch job, whose body goes down the pipe the decoder
//             drains. The decoder begins another for a seek far from the
//             bytes kept (a Range request, or a plain one from 0), and to
//             resume, from the window's end, a fetch let go while nothing
//             played or broken part way.
//   audio     one for yonder: /dev/audio, /dev/audiopos and /dev/audioctl,
//             the takes from every sounding session's queue, the mix, and
//             each session's voice clock (`voice`, `voice_generation`).
//
// A session's `lock` guards what more than one of them touch: the asks
// (play, seek, settings, stop), the queue, the facts, the loader's
// product. `s_audio.lock` guards the session list, which sessions are in
// the mix (`voiced`, which keeps the reap off them) and the loader slots
// taken. No thread is
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
// last began (an open or a flush), and each voice keeps ANCHORS
// (os64/media_mix.h): where in the stream each run of its frames went.
// What the ear hears is /dev/audiopos's `heard`, extrapolated while the
// stream plays, read through a voice's anchors.
//
// EVERY SESSION THAT PLAYS SOUNDS (the mix, up to MIX_VOICES; one past it
// waits, told busy, as when another program has the stream: § When the
// stream is busy).

#include "media.h"

#include "fetch_job.h"
#include "fetch_pipe.h"
#include "media_bytes.h"
#include "os64/audio.h"
#include "os64/bell.h"
#include "os64/lock.h"
#include "os64/media.h"
#include "os64/media_mix.h"
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
// The audio thread: how often it tries a busy device, how long the device
// stays open with nothing to play, and how long a pause keeps it.
#define BUSY_RETRY_MS 500u
#define IDLE_CLOSE_MS 3000u
#define PAUSE_KEEP_MS 30000u
// While a session sounds, the window is rung this often, for timeupdate.
#define RING_SOUNDING_MS 250u
// The sessions sounding at once. One past it waits, told busy: a page of
// dozens of players starting together is not a page anyone listens to.
#define MIX_VOICES 16u
// How many sounds are fetched at once; the rest wait their turn. A page may
// hold dozens of players, and a fetch costs a connection, or a bell while
// libfetch queues it, out of the program's sixty-four handles. Fewer than
// libfetch's six to one host (FETCH_POOL_PER_KEY), because a fetch held
// off by the pipe's backpressure keeps its connection while it is held
// (only HOLD_MS when nothing plays and the server answers ranges), and the
// page's other fetches from that host need one left (DEBTS.md § A page's
// sound).
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
    // The audio thread's: in the mix (asks to sound, has frames in the
    // device still to be heard, or is paused while the whole stream is,
    // its tail held there), written under s_audio.lock, which the reap
    // reads it under; and the voice's clock and the queue generation
    // it was written from, which only the audio thread touches.
    bool voiced;
    os64_media_voice_t voice;
    uint32_t voice_generation;
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
    bool arrived;                   // the resource's end is in the window
    bool failed, opening;
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
    return true;
}

// The running fetch's head: its status, where its body begins, the
// resource's length, and whether the server answers ranges. A Range
// request answered whole (200) is a server that does not: the window
// begins again at 0, and no seek asks it for a range again. False when
// the head fails the load (`why` says why).
static bool head_taken(yonder_media_t *m, const yonder_fetched_t *head)
{
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
        // decoder has already read: a file replaced on the server since
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
                m->size = (int64_t)(m->window.base + m->window.have);
                m->arrived = true;
            }
        } else {
            // Streamed, but its head was never published: nothing will come.
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
static bool decoder_wait(yonder_media_t *m, uint64_t ms)
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
        if (!decoder_wait(m, OS64_WAIT_FOREVER))
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
            return (int64_t)n;
        }
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
        if (m->held && !fetch_restart(m, end))
            return -1;
        if (!pump(m) && !decoder_wait(m, 200))
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
    yonder_media_seek_state_t s = {m->opening, m->ranges, fetch_running(m) && !m->held, m->held, m->arrived};
    yonder_media_seek_t plan = yonder_media_seek_plan(&m->window, offset, &s);
    if (plan != YONDER_MEDIA_SEEK_HERE && m->size >= 0 && offset >= (uint64_t)m->size)
        plan = YONDER_MEDIA_SEEK_REFUSE;
    if (plan == YONDER_MEDIA_SEEK_REFUSE)
        return -1;
    if (plan == YONDER_MEDIA_SEEK_JUMP && !fetch_restart(m, offset))
        return -1;
    m->at = offset;
    return (int64_t)offset;
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

// The fetch job for a network address, its body from byte `from` on (a
// Range request past 0; the cache answers none) streamed to the decoder,
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
    job->headers = os64_calloc(1, 48);
    if (job->headers != NULL && from > 0) {
        os64_snprintf(job->headers, 48, "Range: bytes=%llu-\r\n", (unsigned long long)from);
        job->headers_len = os64_strlen(job->headers);
    }
    m->asked_from = from;
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
    } else if (!loader_start(m, 0)) {
        fail(m, OS64_DOM_MEDIA_ERR_NETWORK, "The sound could not be fetched");
    } else {
        // The size is the head's: wait for it (or for the answer, whole).
        while (!m->headed && !m->failed && !m->arrived)
            if (!pump(m) && !decoder_wait(m, 200))
                break;
        // Remote while bytes are still to come: an answer whole already (a
        // data: URL, the cache, a short sound) is read in memory, exactly.
        source = (os64_media_source_t){m, store_read, store_seek, m->size, !m->arrived};
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
    uint64_t idle_since = 0;        // ms the fetch has been holdable since (fetch_holdable), 0 not
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
        // sound from canplay. Once it is full the fetch runs on into the
        // window and the pipe until their limits hold it off (pump).
        bool moved = pump(m);
        if (full || decoded_all) {
            os64_lock_acquire(&m->lock);
            moved = readiness_locked(m, decoded_all) || moved;
            os64_lock_release(&m->lock);
            if (moved)
                ring_window();
            product_trim(m);
            // A fetch still running for a session nobody plays is let go
            // after HOLD_MS (store_read resumes it): a paused podcast does
            // not keep a connection and a loader slot for the evening.
            uint64_t wait = OS64_WAIT_FOREVER;
            if (fetch_holdable(m)) {
                uint64_t now = (uint64_t)os64_micros() / 1000;
                if (idle_since == 0)
                    idle_since = now;
                if (now - idle_since >= HOLD_MS) {
                    // What the pipe already holds is kept first, past the
                    // usual lead: it was paid for.
                    (void)pump_ahead(m, UINT64_MAX);
                    if (!fetch_stop(m))
                        break;
                    // The pipe may have brought the end, or a failure.
                    m->held = !m->arrived && !m->failed;
                    idle_since = 0;
                } else {
                    wait = HOLD_MS - (now - idle_since);
                }
            } else {
                idle_since = 0;
            }
            // Parked until rung: every reason to go on rings this bell (a
            // play, a seek, the settings, a stop, the audio thread taking
            // from the queue), so a finished or idle element costs nothing.
            if (!decoder_wait(m, wait))
                break;
            continue;
        }
        idle_since = 0;
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
    bool refused;                   // the last open found the stream another program's
    uint64_t idle_since, paused_since, busy_tried, rang;
    uint64_t drained_since;         // micros when the ring was first seen dry with all of it taken
} Device;

// What the stream says of the ear, read once a round (device_heard).
typedef struct {
    bool known;                     // the position could say
    uint64_t heard;                 // the stream frame the ear is at
    uint64_t passed;                // one it has certainly passed (what prunes anchors)
    bool playing;                   // the stream plays
    bool drained;                   // the sink has every frame written, and the ring ran dry
    uint32_t delay_us;              // what the sink holds past its pull
} Ear;

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

// Where the ear is in the stream; `known` false when the position cannot
// say, and `*lost` when the stream began a generation the audio thread did
// not start: a flush from elsewhere (a
// sink attached or detached, a `flush` written to /dev/audioctl) threw
// away what it held. `drained` (once the sink has taken every frame
// written and the ring has run dry) comes with `delay_us`, what the sink
// says lies beyond its pull: the last of it reaches the ear that long
// after (a real sink's `heard` need not catch up with `taken` by itself).
static void device_heard(Device *d, Ear *ear, bool *lost)
{
    *ear = (Ear){0};
    *lost = false;
    if (d->position < 0)
        return;
    os64_audio_position_t pos;
    if (os64_read((int32_t)d->position, &pos, sizeof(pos)) < (int64_t)sizeof(pos))
        return;
    if (!d->generation_known) {
        d->generation = pos.generation;
        d->generation_known = true;
    }
    if (pos.generation != d->generation) {
        *lost = true;
        return;
    }
    if (pos.accepted > d->written)
        return;
    ear->known = true;
    ear->drained = pos.taken >= d->written && pos.state == OS64_AUDIO_STARVED;
    ear->delay_us = pos.delay_us;
    uint64_t heard = pos.heard;
    if (pos.state == OS64_AUDIO_PLAYING) {
        ear->playing = true;
        uint64_t now = (uint64_t)os64_micros();
        if (now > pos.micros)
            heard += (now - pos.micros) * pos.rate / 1000000u;
        if (heard > pos.taken)
            heard = pos.taken;
    }
    ear->heard = heard;
    // Once the ring has run dry with all of it taken, `heard` stops short of
    // `taken` by the sink's delay and moves no more until something is
    // written, so the anchors are pruned by `taken` then, or a voice whose
    // anchors are all in use would wait for room forever.
    ear->passed = ear->drained ? pos.taken : heard;
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

static bool asks(yonder_media_t *m)
{
    os64_lock_acquire(&m->lock);
    bool asking = m->play && !m->stop && !m->facts.ended;
    os64_lock_release(&m->lock);
    return asking;
}

// Each session that asks to play and cannot is told busy: every one when
// another program has the stream (`refused`), and one past the mix's
// MIX_VOICES. s_audio.lock held.
static void tell_busy_locked(bool refused)
{
    for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next) {
        os64_lock_acquire(&m->lock);
        bool busy = m->play && !m->stop && !m->facts.ended && (refused || !m->voiced);
        bool changed = busy != m->facts.busy;
        m->facts.busy = busy;
        if (busy)
            m->facts.sounding = false;
        os64_lock_release(&m->lock);
        if (changed)
            ring_window();
    }
}

// The runs of a queue one block may take from a voice (a short clip on
// loop wraps more than once in a block), and the least a block is cut to
// for a voice held back by that or its anchors (the block's comment in
// audio_main).
#define VOICE_PIECES 8u
#define BLOCK_FLOOR 64u

// One take mixed into a block: where in the block it lies, the media frame
// it starts at, and its length.
typedef struct {
    uint32_t offset;
    int64_t at;
    uint32_t frames;
} Piece;

// What the audio thread knows of a voice for one round.
typedef struct {
    yonder_media_t *m;
    bool wants;                     // play, not stopped, not ended
    bool resync;
    bool stopped;
    bool finished;                  // its queue has given its last frame
    double gain;
    Piece piece[VOICE_PIECES];      // what this round's block took from it
    uint32_t pieces;
} Voice;

// Whether a voice has frames in the stream the ear has not reached.
static bool voice_unheard(const yonder_media_t *m, const Ear *ear)
{
    return m->voice.anchors > 0 && (!ear->known || ear->heard < os64_media_voice_stream_end(&m->voice));
}

// The stream's counters start again (a flush, ours or another's, or the
// device shut): no frame of any voice is in it, so every clock starts
// again. With `resync`, a voice whose frames were thrown away unheard goes
// on from where its ear was (its decoder seeks there and flushes its
// queue); one closed, ended or failed does not. Without it (a flush of
// what only a voice's own seek made stale) nothing heard was lost.
static void voices_restart(Device *d, Voice *v, uint32_t n, bool resync)
{
    counters_restart(d);
    for (uint32_t i = 0; i < n; i++) {
        yonder_media_t *m = v[i].m;
        if (m->voice.anchors > 0) {
            os64_lock_acquire(&m->lock);
            if (resync && !m->stop && !m->facts.ended && m->facts.error == 0)
                m->resync = true;
            m->facts.sounding = false;
            os64_lock_release(&m->lock);
        }
        os64_media_voice_reset(&m->voice);
    }
}

// A voice leaves the mix: the window may reap it from then on, so nothing
// here touches it after.
static void voice_release(yonder_media_t *m)
{
    os64_lock_acquire(&s_audio.lock);
    os64_lock_acquire(&m->lock);
    m->facts.sounding = false;
    os64_lock_release(&m->lock);
    m->voiced = false;
    os64_lock_release(&s_audio.lock);
}

// THE MIX (AUDIO_YONDER.md § The audio thread and the mixer). Every session
// that asks to play is a voice; each block is what every voice had for it,
// times its gain, summed (os64/media_mix.h), and each voice's clock says
// which of its frames the stream's position is. A voice that pauses while
// others play stops being taken from and its last frames in the device
// play out; when nobody plays, the stream's own pause holds what it has.
static int64_t audio_main(void *arg)
{
    (void)arg;
    Device d = {.audio = -1, .position = -1};
    int16_t *buf = os64_malloc(sizeof(int16_t) * 2 * WRITE_FRAMES);
    int32_t *acc = os64_malloc(sizeof(int32_t) * 2 * WRITE_FRAMES);
    Voice v[MIX_VOICES];
    d.idle_since = now_ms();
    for (;;) {
        // Who is in the mix: every session that asks, and every one with
        // frames in the device still to be heard (its clock is read until
        // they are). `voiced` keeps the reap off them meanwhile.
        os64_lock_acquire(&s_audio.lock);
        if (s_audio.quit) {
            os64_lock_release(&s_audio.lock);
            break;
        }
        // The voices already in it first (never more than MIX_VOICES), so
        // none is left unvisited, then those that ask, while there is room.
        uint32_t n = 0, wanting = 0, waiting = 0;
        for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next)
            if (m->voiced && n < MIX_VOICES)
                v[n++] = (Voice){.m = m};
            else if (!m->voiced && asks(m))
                waiting++;
        // A full mix with a session waiting gives up the voices that do not
        // play (paused, their tails held): a paused voice must not keep a
        // page's next sound waiting. One with frames the ear has not heard
        // goes on from where its ear was when it plays again.
        if (n == MIX_VOICES && waiting > 0) {
            uint32_t kept = 0;
            for (uint32_t i = 0; i < n; i++) {
                yonder_media_t *m = v[i].m;
                if (asks(m)) {
                    v[kept++] = v[i];
                    continue;
                }
                os64_lock_acquire(&m->lock);
                if (m->voice.anchors > 0 && !m->stop && !m->facts.ended && m->facts.error == 0)
                    m->resync = true;
                m->facts.sounding = false;
                os64_lock_release(&m->lock);
                os64_media_voice_reset(&m->voice);
                m->voiced = false;
            }
            n = kept;
        }
        for (yonder_media_t *m = s_audio.sessions; m != NULL && n < MIX_VOICES; m = m->next) {
            if (m->voiced || !asks(m))
                continue;
            m->voiced = true;
            os64_media_voice_reset(&m->voice);
            os64_lock_acquire(&m->lock);
            m->voice_generation = m->queue.generation;
            os64_lock_release(&m->lock);
            v[n++] = (Voice){.m = m};
        }
        tell_busy_locked(d.refused);
        os64_lock_release(&s_audio.lock);
        uint64_t now = now_ms();
        for (uint32_t i = 0; i < n; i++) {
            yonder_media_t *m = v[i].m;
            os64_lock_acquire(&m->lock);
            v[i].stopped = m->stop;
            v[i].wants = m->play && !m->stop && !m->facts.ended;
            v[i].resync = m->resync;
            v[i].gain = m->gain;
            os64_lock_release(&m->lock);
            if (v[i].wants)
                wanting++;
        }

        if (n == 0) {
            if (d.audio >= 0 && now - d.idle_since >= IDLE_CLOSE_MS)
                device_close(&d);
            (void)os64_bell_wait(&s_audio.bell, 250);
            continue;
        }
        d.idle_since = now;

        if (wanting == 0) {
            // Nobody plays: the stream's own pause keeps what it holds and
            // stops the ear where it is. A long pause gives the device back
            // (AUDIO_YONDER.md § Pause), and each voice goes on from where
            // its ear was when it plays again.
            if (d.audio >= 0 && !d.paused) {
                control("pause\n");
                d.paused = true;
                d.paused_since = now;
            }
            bool close = d.audio < 0 || now - d.paused_since >= PAUSE_KEEP_MS;
            if (close && d.audio >= 0) {
                device_close(&d);
                voices_restart(&d, v, n, true);
            }
            for (uint32_t i = 0; i < n; i++) {
                yonder_media_t *m = v[i].m;
                os64_lock_acquire(&m->lock);
                m->facts.sounding = false;
                bool done = m->stop || m->facts.ended;
                os64_lock_release(&m->lock);
                // Its tail is for nobody (closed, or played out), or the
                // device that held it is shut.
                if (done || close)
                    voice_release(m);
            }
            (void)os64_bell_wait(&s_audio.bell, 250);
            continue;
        }

        // Someone plays: a voice to come back from where its ear was is
        // asked to (its decoder seeks and flushes), and sits this round out.
        for (uint32_t i = 0; i < n; i++)
            if (v[i].resync) {
                os64_lock_acquire(&v[i].m->lock);
                ring_decoder_locked(v[i].m);
                os64_lock_release(&v[i].m->lock);
            }
        if (d.audio < 0) {
            // No device, so no tail: a voice that does not play has nothing
            // left to be heard and leaves the mix now, not once the device
            // opens (another program may hold it for an hour).
            uint32_t kept = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (v[i].wants)
                    v[kept++] = v[i];
                else
                    voice_release(v[i].m);
            }
            n = kept;
            if (now - d.busy_tried < BUSY_RETRY_MS && d.busy_tried != 0) {
                (void)os64_bell_wait(&s_audio.bell, BUSY_RETRY_MS);
                continue;
            }
            d.busy_tried = now;
            int64_t h = os64_open("/dev/audio", "w");
            if (h < 0) {
                // Another program has the stream: whoever asks waits (busy),
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
            voices_restart(&d, v, n, false);
            os64_lock_acquire(&s_audio.lock);
            tell_busy_locked(false);
            os64_lock_release(&s_audio.lock);
        }
        Ear ear;
        bool lost;
        device_heard(&d, &ear, &lost);
        if (lost) {
            voices_restart(&d, v, n, true);
            continue;
        }
        if (d.paused) {
            // The stream comes back. What it holds that nobody playing owns
            // (the tails of voices paused or closed, heard much later than
            // they were meant to be, or a playing voice's frames from before
            // its seek) is flushed, and those voices go on from where their
            // ears were.
            bool owned = false;
            for (uint32_t i = 0; i < n; i++) {
                os64_lock_acquire(&v[i].m->lock);
                bool current = v[i].m->queue.generation == v[i].m->voice_generation;
                os64_lock_release(&v[i].m->lock);
                owned |= v[i].wants && current && voice_unheard(v[i].m, &ear);
            }
            if (!owned) {
                control("flush\n");
                voices_restart(&d, v, n, true);
            }
            control("resume\n");
            d.paused = false;
        }

        // A voice's queue that moved to another generation (a seek, or the
        // resync's flush): what the device holds of it is from before. When
        // it is the only one with frames there, those are flushed; with
        // others sounding, its last fifth of a second plays out under them.
        for (uint32_t i = 0; i < n; i++) {
            yonder_media_t *m = v[i].m;
            os64_lock_acquire(&m->lock);
            uint32_t generation = m->queue.generation;
            os64_lock_release(&m->lock);
            if (generation == m->voice_generation)
                continue;
            bool others = false;
            for (uint32_t j = 0; j < n; j++)
                others |= j != i && voice_unheard(v[j].m, &ear);
            if (!others && voice_unheard(m, &ear)) {
                // Nothing anyone was to hear is lost: no voice resyncs.
                control("flush\n");
                voices_restart(&d, v, n, false);
            }
            m->voice_generation = generation;
            os64_media_voice_reset(&m->voice);
        }

        // The block: what each voice playing has, up to WRITE_FRAMES, mixed.
        // A take never crosses a run of the queue (a loop's wrap), so a
        // voice takes again until the block is full or it has no more: one
        // that came up short at a wrap would be silent for the rest of a
        // block another voice filled. Each piece is a run of its own. No
        // take that would start a run a voice has no anchor for (a clip a
        // few milliseconds long on loop starts one per piece): the device
        // holds runs not yet heard, and they are what its clock reads.
        // A voice held back by those budgets (pieces, anchors) with more
        // queued behind them would be silent for the rest of a block another
        // voice filled, changing a short loop's cadence: the block is cut to
        // what it can give, worked out before anything is taken, as a
        // voice alone would make it. Less than BLOCK_FLOOR does not cut it
        // (a loop a few frames long would make every block that small); it
        // is padded, as a voice that has run dry is.
        uint32_t limit = WRITE_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            yonder_media_t *m = v[i].m;
            uint32_t budget = d.position < 0 ? VOICE_PIECES
                              : m->voice.anchors >= OS64_MEDIA_MIX_ANCHORS ? 0
                              : OS64_MEDIA_MIX_ANCHORS - m->voice.anchors;
            if (budget > VOICE_PIECES)
                budget = VOICE_PIECES;
            os64_lock_acquire(&m->lock);
            uint32_t avail = 0, runs = m->queue.segments < budget ? m->queue.segments : budget;
            for (uint32_t k = 0; k < runs; k++)
                avail += m->queue.segment[k].frames;
            bool held = v[i].wants && m->queue.segments > budget;
            os64_lock_release(&m->lock);
            if (held && avail >= BLOCK_FLOOR && avail < limit)
                limit = avail;
        }
        uint32_t block = 0;
        if (buf != NULL && acc != NULL)
            os64_memset(acc, 0, sizeof(int32_t) * 2 * WRITE_FRAMES);
        for (uint32_t i = 0; i < n; i++) {
            yonder_media_t *m = v[i].m;
            os64_lock_acquire(&m->lock);
            bool can = buf != NULL && acc != NULL && v[i].wants && !m->resync &&
                       m->queue.generation == m->voice_generation;
            uint32_t filled = 0;
            while (can && filled < limit && v[i].pieces < VOICE_PIECES &&
                   (m->voice.anchors + v[i].pieces < OS64_MEDIA_MIX_ANCHORS || d.position < 0)) {
                int64_t at;
                uint32_t got = os64_media_queue_take(&m->queue, buf, limit - filled, &at);
                if (got == 0)
                    break;
                os64_media_mix_add(acc + 2 * filled, buf, got, v[i].gain);
                v[i].piece[v[i].pieces++] = (Piece){filled, at, got};
                filled += got;
            }
            if (filled > 0)
                ring_decoder_locked(m);
            v[i].finished = os64_media_queue_finished(&m->queue);
            os64_lock_release(&m->lock);
            if (filled > block)
                block = filled;
        }
        if (block > 0) {
            os64_media_mix_out(acc, buf, block);
            if (!device_write(&d, buf, block)) {
                device_close(&d);
                for (uint32_t i = 0; i < n; i++)
                    if (v[i].pieces > 0)
                        fail(v[i].m, OS64_DOM_MEDIA_ERR_DECODE, "The sound device failed");
                voices_restart(&d, v, n, true);
                continue;
            }
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t k = 0; k < v[i].pieces; k++)
                    os64_media_voice_wrote(&v[i].m->voice, d.written + v[i].piece[k].offset,
                                           v[i].piece[k].at, v[i].piece[k].frames);
            d.written += block;
        }

        // The ear, for each voice: its position, whether it sounds, and its
        // end once its queue has given its last frame and the ear has heard
        // it. A voice done (closed, ended, or paused with its tail heard)
        // leaves the mix.
        device_heard(&d, &ear, &lost);
        if (lost) {
            voices_restart(&d, v, n, true);
            continue;
        }
        if (!ear.drained)
            d.drained_since = 0;
        else if (d.drained_since == 0)
            d.drained_since = (uint64_t)os64_micros();
        bool tail_heard = ear.drained && (uint64_t)os64_micros() - d.drained_since >= ear.delay_us;
        bool ring = ear.playing && now - d.rang >= RING_SOUNDING_MS;
        for (uint32_t i = 0; i < n; i++) {
            yonder_media_t *m = v[i].m;
            int64_t frame = -1;
            if (ear.known) {
                os64_media_voice_prune(&m->voice, ear.passed);
                frame = os64_media_voice_heard(&m->voice, ear.heard);
            }
            bool all_heard = m->voice.anchors == 0 ||
                             (ear.known && (ear.heard >= os64_media_voice_stream_end(&m->voice) || tail_heard));
            os64_lock_acquire(&m->lock);
            bool synced = m->queue.generation == m->voice_generation;
            if (synced && frame >= 0)
                m->facts.position = (double)frame / RATE;
            m->facts.sounding = ear.playing && synced && v[i].wants && m->voice.anchors > 0 && !all_heard;
            // `finished` and what was heard describe the generation the take
            // saw; a seek that flushed the queue since makes them another
            // playback's, which must not be called ended, and so does a
            // resync on its way (its tail was thrown away, not heard).
            bool ended_now = false;
            if (v[i].wants && synced && !m->resync && v[i].finished && all_heard && !m->facts.ended) {
                m->facts.ended = true;
                m->facts.sounding = false;
                ended_now = true;
            }
            bool done = m->stop || m->facts.ended || (!m->play && all_heard);
            os64_lock_release(&m->lock);
            ring |= ended_now;
            if (done)
                voice_release(m);
        }
        if (ring) {
            d.rang = now;
            ring_window();
        }
        if (block == 0)
            (void)os64_bell_wait(&s_audio.bell, 10);
    }
    device_close(&d);
    os64_free(buf);
    os64_free(acc);
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
    m->size = -1;
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
        bool gone = m->closed && threads_done_locked(m) && !m->voiced;
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
    // it. The audio thread lets go of the device first, so the voices are
    // let go once it has, and the reap may take the last session. A fetch
    // notices the stop at its next look (a hop, the pipe's room, libfetch's
    // queue); a start that truly failed would wait forever, which is the
    // price thread_start names.
    for (bool audio_done = !s_audio.started;;) {
        os64_lock_acquire(&s_audio.lock);
        if (!audio_done && s_audio.done) {
            audio_done = true;
            for (yonder_media_t *m = s_audio.sessions; m != NULL; m = m->next)
                m->voiced = false;
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
