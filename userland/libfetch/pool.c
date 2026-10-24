// pool.c — the kept connections (pool.h), and the rule that says which may
// be kept.

#include "pool.h"

#include "os64/lock.h"
#include "os64/os64.h"
#include "os64/str.h"

typedef struct {
    bool used;
    fetch_pool_key_t key;
    fetch_transport_t io;
    uint64_t kept_at;               // ticks: when it was last given back
} kept_t;

// The library's one piece of shared state: a fetch is still its own object,
// and what fetches share is only the connections in here, one owner at a
// time.
static os64_lock_t s_lock = OS64_LOCK_INIT;
static kept_t s_kept[FETCH_POOL_TOTAL];

static bool same_key(const fetch_pool_key_t *a, const fetch_pool_key_t *b)
{
    return a->tls == b->tls && a->via_proxy == b->via_proxy && a->port == b->port &&
           a->trust == b->trust && os64_streq(a->host, b->host);
}

static bool clock_now(os64_ticks_t *now)
{
    return os64_ticks(now) >= 0 && now->per_second != 0;
}

// Past its idle time, or kept at a time the clock now contradicts.
static bool expired(const kept_t *k, const os64_ticks_t *now)
{
    if (now->ticks < k->kept_at)
        return true;
    uint64_t idle = now->ticks - k->kept_at;
    return idle >= (uint64_t)FETCH_POOL_IDLE_MS * now->per_second / 1000u;
}

// The connections leaving the pool, closed once the lock is let go: closing
// one frees a TLS client and closes a handle, which is no work to hold the
// lock across.
typedef struct {
    fetch_transport_t io[FETCH_POOL_TOTAL + 1];
    size_t n;
} leaving_t;

static void leave(leaving_t *out, kept_t *k)
{
    out->io[out->n++] = k->io;
    k->used = false;
}

static void close_all(leaving_t *out)
{
    for (size_t i = 0; i < out->n; i++)
        fetch_transport_close(&out->io[i], false);
}

bool fetch_pool_take(const fetch_pool_key_t *key, fetch_transport_t *io)
{
    os64_ticks_t now;
    bool timed = clock_now(&now);
    leaving_t out = {0};
    kept_t *best = NULL;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++) {
        kept_t *k = &s_kept[i];
        if (!k->used)
            continue;
        if (!timed || expired(k, &now)) {
            leave(&out, k);
            continue;
        }
        // The most recently kept: the one its server is least likely to
        // have closed already.
        if (same_key(&k->key, key) && (best == NULL || k->kept_at > best->kept_at))
            best = k;
    }
    if (best != NULL) {
        *io = best->io;
        best->used = false;
    }
    os64_lock_release(&s_lock);
    close_all(&out);
    return best != NULL;
}

void fetch_pool_put(const fetch_pool_key_t *key, const fetch_transport_t *io)
{
    os64_ticks_t now;
    leaving_t out = {0};
    if (!clock_now(&now)) {
        out.io[out.n++] = *io;
        close_all(&out);
        return;
    }
    os64_lock_acquire(&s_lock);
    size_t mine = 0, all = 0;
    kept_t *oldest_mine = NULL, *oldest = NULL, *free_slot = NULL;
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++) {
        kept_t *k = &s_kept[i];
        if (k->used && expired(k, &now))
            leave(&out, k);
        if (!k->used) {
            if (free_slot == NULL)
                free_slot = k;
            continue;
        }
        all++;
        if (oldest == NULL || k->kept_at < oldest->kept_at)
            oldest = k;
        if (same_key(&k->key, key)) {
            mine++;
            if (oldest_mine == NULL || k->kept_at < oldest_mine->kept_at)
                oldest_mine = k;
        }
    }
    if (mine >= FETCH_POOL_PER_KEY) {
        leave(&out, oldest_mine);
        free_slot = oldest_mine;
    } else if (all >= FETCH_POOL_TOTAL) {
        leave(&out, oldest);
        free_slot = oldest;
    }
    free_slot->used = true;
    free_slot->key = *key;
    free_slot->io = *io;
    free_slot->io.cancelled = NULL;
    free_slot->io.cancel_ctx = NULL;
    free_slot->kept_at = now.ticks;
    os64_lock_release(&s_lock);
    close_all(&out);
}

void fetch_pool_drain(void)
{
    leaving_t out = {0};
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_kept[i].used)
            leave(&out, &s_kept[i]);
    os64_lock_release(&s_lock);
    close_all(&out);
}

size_t fetch_pool_idle(void)
{
    size_t n = 0;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        n += s_kept[i].used;
    os64_lock_release(&s_lock);
    return n;
}

bool connection_reusable(const fetch_reuse_facts_t *f)
{
    if (!f->request_http11 || !f->response_http11)
        return false;
    if (f->request_close || f->response_close || !f->request_whole)
        return false;
    if (!f->transport_ok || f->leftover)
        return false;
    if (f->bodiless)
        return true;
    return (f->framing == HTTP_FRAMING_LENGTH || f->framing == HTTP_FRAMING_CHUNKED) && f->consumed;
}
