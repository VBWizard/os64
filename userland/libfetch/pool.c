// pool.c — libfetch's connections, in use and kept (pool.h), and the rule
// that says which may be kept.

#include "pool.h"

#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/os64.h"
#include "os64/str.h"

// One connection. `io` is the pool's while the connection is idle and the
// fetch's own copy while it is in use (an HTTP/1.1 connection has one user,
// so the transport is lent whole); `refs` says which.
struct fetch_conn {
    fetch_pool_key_t key;
    fetch_transport_t io;
    fetch_proto_t protocol;
    unsigned refs;                  // fetches using it: 0 idle, 1 in use
    bool dialling;                  // a slot reserved for a dial in flight
    uint64_t kept_at;               // ticks: when it was last given back
};

// The library's one piece of shared state: a fetch is still its own object,
// and what fetches share is only the connections in here, one user at a
// time, and the list of the fetches waiting for one.
static os64_lock_t s_lock = OS64_LOCK_INIT;
static fetch_conn_t *s_conns[FETCH_POOL_TOTAL];
static fetch_pool_wait_t *s_waiters;

static bool same_key(const fetch_pool_key_t *a, const fetch_pool_key_t *b)
{
    return a->tls == b->tls && a->via_proxy == b->via_proxy && a->port == b->port &&
           a->trust == b->trust && os64_streq(a->host, b->host);
}

static bool clock_now(os64_ticks_t *now)
{
    return os64_ticks(now) >= 0 && now->per_second != 0;
}

static bool idle(const fetch_conn_t *c)
{
    return c->refs == 0 && !c->dialling;
}

// Idle `ms` or longer, or kept at a time the clock now contradicts. The
// clock is read under the lock, by take and put alike, so every kept_at
// was written by an earlier holder than the reading: a `now` before one is
// a clock that went backwards, not a put that overtook the read, and a
// connection whose age cannot be known is closed rather than trusted
// (expired) and taken by nobody.
static bool idle_past(const fetch_conn_t *c, const os64_ticks_t *now, uint32_t ms)
{
    if (now->ticks < c->kept_at)
        return true;
    uint64_t age = now->ticks - c->kept_at;
    return age >= (uint64_t)ms * now->per_second / 1000u;
}

// Past its idle time: closed when next seen.
static bool expired(const fetch_conn_t *c, const os64_ticks_t *now)
{
    return idle_past(c, now, FETCH_POOL_IDLE_MS);
}

// The connections leaving the pool, closed and freed once the lock is let
// go: closing one frees a TLS client and closes a handle, which is no work
// to hold the lock across.
typedef struct {
    fetch_conn_t *conn[FETCH_POOL_TOTAL];
    size_t n;
} leaving_t;

// Under the lock: an IDLE connection leaves its slot.
static void leave(leaving_t *out, size_t slot)
{
    out->conn[out->n++] = s_conns[slot];
    s_conns[slot] = NULL;
}

static void close_all(leaving_t *out)
{
    for (size_t i = 0; i < out->n; i++) {
        fetch_transport_close(&out->conn[i]->io, false);
        os64_free(out->conn[i]);
    }
}

// Under the lock (os64/bell.h's rule: a ring is made under the lock the
// bell's owner takes to withdraw it, and a waiter withdraws under this
// one). Everybody, because a ring is a hint: whoever is rung asks again,
// and one rung alone may be the one that is on its way out.
static void ring_waiters(void)
{
    for (fetch_pool_wait_t *w = s_waiters; w != NULL; w = w->node.next)
        os64_bell_ring(w->bell);
}

static void waiter_add(fetch_pool_wait_t *w)
{
    w->node.next = s_waiters;
    s_waiters = w;
}

static void waiter_remove(fetch_pool_wait_t *w)
{
    for (fetch_pool_wait_t **link = &s_waiters; *link != NULL; link = &(*link)->node.next)
        if (*link == w) {
            *link = w->node.next;
            break;
        }
    w->node.next = NULL;
}

// How long since `start`, in milliseconds; false when the clock cannot say.
static bool waited(const os64_ticks_t *start, uint64_t *ms)
{
    os64_ticks_t now;
    if (!clock_now(&now) || now.per_second != start->per_second || now.ticks < start->ticks)
        return false;
    *ms = (now.ticks - start->ticks) * 1000u / now.per_second;
    return true;
}

fetch_pool_take_t fetch_pool_take(const fetch_pool_key_t *key, uint32_t idle_most_ms, bool fresh,
                                  fetch_pool_wait_t *wait, fetch_conn_t **conn, fetch_transport_t *io)
{
    // The object a dial slot would need, made before the lock is taken and
    // freed if this pass did not use it.
    fetch_conn_t *spare = NULL;
    os64_ticks_t began;
    bool timing = false;
    *conn = NULL;
    wait->waited_ms = 0;
    wait->for_the_table = false;
    for (;;) {
        if (spare == NULL) {
            spare = os64_malloc(sizeof(*spare));
            if (spare == NULL)
                return FETCH_POOL_FAILED;
        }
        os64_ticks_t now;
        leaving_t out = {0};
        fetch_conn_t *best = NULL, *slot_conn = NULL;
        size_t of_key = 0, in_all = 0, free_slot = FETCH_POOL_TOTAL;
        size_t oldest_idle = FETCH_POOL_TOTAL, oldest_idle_of_key = FETCH_POOL_TOTAL;
        fetch_pool_take_t answer = FETCH_POOL_FAILED;
        bool wait_now = false;

        os64_lock_acquire(&s_lock);
        bool timed = clock_now(&now);
        for (size_t i = 0; i < FETCH_POOL_TOTAL; i++) {
            fetch_conn_t *c = s_conns[i];
            if (c != NULL && idle(c) && (!timed || expired(c, &now))) {
                leave(&out, i);
                c = NULL;
            }
            if (c == NULL) {
                if (free_slot == FETCH_POOL_TOTAL)
                    free_slot = i;
                continue;
            }
            in_all++;
            bool mine = same_key(&c->key, key);
            of_key += mine;
            if (!idle(c))
                continue;
            if (oldest_idle == FETCH_POOL_TOTAL || c->kept_at < s_conns[oldest_idle]->kept_at)
                oldest_idle = i;
            if (!mine)
                continue;
            if (oldest_idle_of_key == FETCH_POOL_TOTAL || c->kept_at < s_conns[oldest_idle_of_key]->kept_at)
                oldest_idle_of_key = i;
            // The most recently kept: the one its server is least likely to
            // have closed already. One idle too long for this request is
            // left for another.
            if (!fresh && !idle_past(c, &now, idle_most_ms) && (best == NULL || c->kept_at > best->kept_at))
                best = c;
        }
        if (best != NULL) {
            best->refs = 1;
            *io = best->io;
            *conn = best;
            answer = FETCH_POOL_KEPT;
        } else {
            // A slot to dial in. An idle connection never makes this fetch
            // wait for one: at the key's cap the key's own (which this fetch
            // may not take, or it would have above), and with the table
            // full the least recently kept of any key, is closed for it.
            size_t slot = FETCH_POOL_TOTAL;
            if (of_key >= FETCH_POOL_PER_KEY) {
                if (oldest_idle_of_key != FETCH_POOL_TOTAL) {
                    leave(&out, oldest_idle_of_key);
                    slot = oldest_idle_of_key;
                }
            } else if (free_slot != FETCH_POOL_TOTAL)
                slot = free_slot;
            else if (in_all >= FETCH_POOL_TOTAL && oldest_idle != FETCH_POOL_TOTAL) {
                leave(&out, oldest_idle);
                slot = oldest_idle;
            }
            if (slot != FETCH_POOL_TOTAL) {
                os64_memset(spare, 0, sizeof(*spare));
                spare->key = *key;
                spare->io.handle = -1;
                spare->protocol = FETCH_PROTO_H1;
                spare->dialling = true;
                s_conns[slot] = slot_conn = spare;
                *conn = spare;
                answer = FETCH_POOL_DIAL;
            } else {
                // Every connection in the way is in use or being dialled.
                // Registered in the same hold that found it so, so that a
                // put or a departure either came before (and was seen
                // above) or rings this bell.
                wait->for_the_table = of_key < FETCH_POOL_PER_KEY;
                wait_now = wait->bell != NULL && wait->bell->handle >= 0;
                if (wait_now)
                    waiter_add(wait);
            }
        }
        // A connection closed above left a slot some other waiter may want.
        if (out.n != 0)
            ring_waiters();
        os64_lock_release(&s_lock);
        close_all(&out);
        if (slot_conn != NULL)
            spare = NULL;
        if (answer != FETCH_POOL_FAILED) {
            os64_free(spare);
            return answer;
        }

        // To wait: the bell first, if this fetch has never waited, and the
        // question asked again with it in hand, since the world may have
        // moved while it was made.
        if (!wait_now) {
            if (wait->bell == NULL || (wait->bell->handle < 0 && os64_bell_make(wait->bell) < 0)) {
                os64_free(spare);
                return FETCH_POOL_FAILED;
            }
            continue;
        }
        fetch_pool_take_t ended = FETCH_POOL_FAILED;
        bool again = false;
        uint64_t so_far = 0;
        if (!timing)
            timing = clock_now(&began);
        if (!timing || !waited(&began, &so_far))
            ended = FETCH_POOL_FAILED;
        else if (wait->cancelled != NULL && wait->cancelled(wait->cancel_ctx))
            ended = FETCH_POOL_INTERRUPTED;
        else if (so_far >= wait->patience_ms)
            ended = FETCH_POOL_TIMED_OUT;
        else {
            // Rung, or interrupted by a signal: both are "ask again", and
            // the cancellation is asked below. So is a look-up, a wait cut
            // short of what was left; a bell that timed out with nothing
            // held back has waited out the patience. Any other answer is a
            // bell that cannot be waited on, and asking again would only
            // spin.
            uint64_t left = wait->patience_ms - so_far;
            bool look = left > FETCH_POOL_LOOK_MS;
            int64_t rung = os64_bell_wait(wait->bell, look ? FETCH_POOL_LOOK_MS : left);
            if (rung == 1 || rung == OS64_INTERRUPTED || (rung == OS64_ERR_TIMEOUT && look))
                again = true;
            else if (rung == OS64_ERR_TIMEOUT)
                ended = FETCH_POOL_TIMED_OUT;
        }
        // Out of the list before anything else: a node left there names a
        // fetch that has moved on, and its bell would be rung after the
        // fetch closed it.
        os64_lock_acquire(&s_lock);
        waiter_remove(wait);
        os64_lock_release(&s_lock);
        if (timing && waited(&began, &so_far))
            wait->waited_ms = so_far;
        // Cancelled while it waited: that is how it ends, whatever else the
        // wait would have answered, and it takes no connection on the way.
        if (wait->cancelled != NULL && wait->cancelled(wait->cancel_ctx)) {
            again = false;
            ended = FETCH_POOL_INTERRUPTED;
        }
        if (!again) {
            os64_free(spare);
            return ended;
        }
    }
}

void fetch_pool_dialled(fetch_conn_t *conn)
{
    os64_lock_acquire(&s_lock);
    conn->dialling = false;
    conn->refs = 1;
    os64_lock_release(&s_lock);
}

void fetch_pool_put(fetch_conn_t *conn, const fetch_transport_t *io)
{
    os64_ticks_t now;
    leaving_t out = {0};
    os64_lock_acquire(&s_lock);
    conn->io = *io;
    conn->io.cancelled = NULL;
    conn->io.cancel_ctx = NULL;
    conn->refs = 0;
    if (clock_now(&now))
        conn->kept_at = now.ticks;
    else
        // A connection whose age could never be told is not kept.
        for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
            if (s_conns[i] == conn)
                leave(&out, i);
    // Kept or gone, it is a connection a waiter can use or a slot one can.
    ring_waiters();
    os64_lock_release(&s_lock);
    close_all(&out);
}

void fetch_pool_gone(fetch_conn_t *conn)
{
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] == conn)
            s_conns[i] = NULL;
    ring_waiters();
    os64_lock_release(&s_lock);
    os64_free(conn);
}

void fetch_pool_drain(void)
{
    leaving_t out = {0};
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] != NULL && idle(s_conns[i]))
            leave(&out, i);
    if (out.n != 0)
        ring_waiters();
    os64_lock_release(&s_lock);
    close_all(&out);
}

bool fetch_pool_evict_one(void)
{
    leaving_t out = {0};
    size_t oldest = FETCH_POOL_TOTAL;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] != NULL && idle(s_conns[i]) &&
            (oldest == FETCH_POOL_TOTAL || s_conns[i]->kept_at < s_conns[oldest]->kept_at))
            oldest = i;
    if (oldest != FETCH_POOL_TOTAL) {
        leave(&out, oldest);
        ring_waiters();
    }
    os64_lock_release(&s_lock);
    close_all(&out);
    return out.n != 0;
}

void fetch_pool_counts(size_t *kept, size_t *live)
{
    size_t k = 0, l = 0;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] != NULL) {
            if (idle(s_conns[i])) k++;
            else l++;
        }
    os64_lock_release(&s_lock);
    *kept = k;
    *live = l;
}

size_t fetch_pool_idle(void)
{
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    return kept;
}

size_t fetch_pool_live(void)
{
    size_t kept, live;
    fetch_pool_counts(&kept, &live);
    return live;
}

static os64_lock_t s_trust_lock = OS64_LOCK_INIT;
static os64_tls_trust *s_trust;     // the system's store, once read

os64_tls_store_status_t fetch_pool_trust(os64_tls_trust **out, os64_tls_store_report_t *report)
{
    os64_tls_store_status_t status = OS64_TLS_STORE_OK;
    os64_lock_acquire(&s_trust_lock);
    if (s_trust == NULL) {
        os64_tls_trust *read = NULL;
        status = os64_tls_trust_reload(&read, report);
        if (status == OS64_TLS_STORE_OK)
            s_trust = read;
    }
    *out = s_trust;
    os64_lock_release(&s_trust_lock);
    return status;
}

void fetch_pool_trust_drop(void)
{
    os64_lock_acquire(&s_trust_lock);
    os64_tls_trust *store = s_trust;
    s_trust = NULL;
    os64_lock_release(&s_trust_lock);
    if (store != NULL)
        os64_tls_trust_free(store);
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
