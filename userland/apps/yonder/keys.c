// keys.c — the window's media-key session (keys.h).
//
// The reader thread blocks in the session's read and queues each press,
// then rings the window; the window takes them at its loop's turn
// (keys_turn in yonder.c) and does each as a page task. The queue is the
// one thing both touch, under `s_lock`. The reader is never joined: it
// lives as long as the session, which is the window's life.

#include "keys.h"

#include "os64/lock.h"
#include "os64/mediakeys.h"
#include "os64/os64.h"
#include "os64/thread.h"

// Presses waiting for the window: a person presses slower than a turn, so
// a full queue lets its oldest go.
#define KEYS_QUEUED 8u

static os64_lock_t s_lock = OS64_LOCK_INIT;
static uint32_t s_queue[KEYS_QUEUED];
static uint32_t s_head, s_count;

static int32_t s_session = -1;
static void (*s_bell)(void);
static int s_said = -1;   // the state last written, -1 before the first

static int64_t reader_main(void *arg)
{
    (void)arg;
    for (;;) {
        os64_media_key_t k;
        int64_t n = os64_read(s_session, &k, sizeof(k));
        if (n == OS64_INTERRUPTED)
            continue;
        if (n != (int64_t)sizeof(k))
            return n;   // the session is gone: nothing more will come
        os64_lock_acquire(&s_lock);
        if (s_count == KEYS_QUEUED) {
            s_head = (s_head + 1) % KEYS_QUEUED;
            s_count--;
        }
        s_queue[(s_head + s_count) % KEYS_QUEUED] = k.key;
        s_count++;
        os64_lock_release(&s_lock);
        s_bell();
    }
}

bool yonder_keys_open(void (*bell)(void))
{
    if (s_session >= 0)
        return true;
    int64_t h = os64_open("/dev/mediakeys", "u");
    if (h < 0)
        return false;
    s_session = (int32_t)h;
    s_bell = bell;
    int64_t reader = os64_thread_named(reader_main, NULL, "keys");
    if (reader < 0) {
        os64_close(s_session);
        s_session = -1;
        return false;
    }
    os64_close((int32_t)reader);
    return true;
}

bool yonder_keys_opened(void)
{
    return s_session >= 0;
}

void yonder_keys_say(os64_dom_media_session_state_t state)
{
    if (s_session < 0 || (int)state == s_said)
        return;
    const char *word = state == OS64_DOM_MEDIA_SESSION_PLAYING ? "playing"
                     : state == OS64_DOM_MEDIA_SESSION_PAUSED ? "paused" : "stopped";
    if (os64_write(s_session, word, os64_strlen(word)) == (int64_t)os64_strlen(word))
        s_said = (int)state;
}

bool yonder_keys_take(uint32_t *key)
{
    os64_lock_acquire(&s_lock);
    bool got = s_count > 0;
    if (got) {
        *key = s_queue[s_head];
        s_head = (s_head + 1) % KEYS_QUEUED;
        s_count--;
    }
    os64_lock_release(&s_lock);
    return got;
}
