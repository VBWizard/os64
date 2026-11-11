// pool.c — libfetch's connections, in use and kept (pool.h), and the rule
// that says which may be kept.

#include "pool.h"

#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/os64.h"
#include "os64/str.h"

// One connection. An HTTP/1.1 connection has one user, so its transport is
// lent whole: `io` is the pool's while the connection is idle and the
// fetch's own copy while it is in use, and `refs` says which. An HTTP/2
// connection keeps its transport for life (`h2`, which owns the engine and
// the socket), and `refs` counts the fetches with a stream on it.
struct fetch_conn {
    fetch_pool_key_t key;
    fetch_transport_t io;
    fetch_proto_t protocol;
    unsigned refs;                  // fetches using it: 0 idle; h1: 1 in use
    bool dialling;                  // a slot reserved for a dial in flight
    uint64_t kept_at;               // ticks: when it was last given back
    // HTTP/2 alone (pool.h's lifecycle).
    h2_conn_t *h2;                  // NULL while its reader is being started (ADOPTING)
    bool struck;                    // out of the table, for good
    bool dead_heard;                // its reader has said its last word: the socket is closed
    bool closing;                   // struck with its socket still open: one of s_closing
    uint64_t order;                 // when it became live: the longest-lived takes the new streams
    uint16_t tls_version;           // what its handshake chose, for the fetches that join it
    bool tls_resumed;
};

// The library's shared state: a fetch is still its own object, and what
// fetches share is the connections in here, the list of the fetches waiting
// for one, and what each key last answered ALPN with.
static os64_lock_t s_lock = OS64_LOCK_INIT;
static fetch_conn_t *s_conns[FETCH_POOL_TOTAL];
static fetch_pool_wait_t *s_waiters;
static size_t s_closing;            // connections struck whose reader has yet to close the socket
static uint64_t s_order;

bool fetch_h2_offer;

// What a key's last TLS dial answered ALPN with: h2 or not. It decides one
// thing, whether a fetch waits for a dial in flight to the key (take), so a
// wrong or forgotten answer costs a wait or a dial and nothing else.
#define REMEMBERED 16
static struct {
    fetch_pool_key_t key;
    bool used, h2;
    uint64_t written;
} s_remembered[REMEMBERED];
static uint64_t s_written;

static bool same_key(const fetch_pool_key_t *a, const fetch_pool_key_t *b)
{
    return a->tls == b->tls && a->via_proxy == b->via_proxy && a->port == b->port &&
           a->trust == b->trust && os64_streq(a->host, b->host);
}

// Under the lock.
static void remember(const fetch_pool_key_t *key, bool h2)
{
    size_t at = 0;
    for (size_t i = 0; i < REMEMBERED; i++) {
        if (s_remembered[i].used && same_key(&s_remembered[i].key, key)) {
            at = i;
            break;
        }
        if (!s_remembered[i].used || (s_remembered[at].used && s_remembered[i].written < s_remembered[at].written))
            at = i;
    }
    s_remembered[at].key = *key;
    s_remembered[at].used = true;
    s_remembered[at].h2 = h2;
    s_remembered[at].written = ++s_written;
}

static bool remembered_h2(const fetch_pool_key_t *key)
{
    for (size_t i = 0; i < REMEMBERED; i++)
        if (s_remembered[i].used && same_key(&s_remembered[i].key, key))
            return s_remembered[i].h2;
    return false;
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

// An HTTP/2 connection nobody will use again: the adopter's hold on it goes
// with the record. With no lock held.
static void free_h2(fetch_conn_t *c)
{
    if (c->h2 != NULL)
        h2_conn_release(c->h2);
    os64_free(c);
}

// Under the lock: an HTTP/2 connection leaves the table for good. Its
// socket is its reader's to close, so until the reader has said so the
// connection is CLOSING and still counts against the table's total (take).
static void strike(fetch_conn_t *c)
{
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] == c)
            s_conns[i] = NULL;
    c->struck = true;
    if (!c->dead_heard && !c->closing) {
        c->closing = true;
        s_closing++;
    }
}

// Under the lock: an IDLE connection leaves its slot. An HTTP/1.1 one is
// closed by the caller once the lock is let go; an HTTP/2 one is condemned,
// and its reader closes it and says so (dead).
static void leave(leaving_t *out, size_t slot)
{
    fetch_conn_t *c = s_conns[slot];
    if (c->protocol == FETCH_PROTO_H2) {
        strike(c);
        h2_conn_condemn(c->h2);
        return;
    }
    out->conn[out->n++] = c;
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
        fetch_conn_t *best = NULL, *slot_conn = NULL, *shared = NULL;
        bool dial_in_flight = false;
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
            bool starting = c->dialling || (c->protocol == FETCH_PROTO_H2 && c->h2 == NULL);
            dial_in_flight = dial_in_flight || (mine && starting);
            // An HTTP/2 connection with room takes another stream, the one
            // longest live first, so that duplicates dialled in a burst go
            // quiet and idle out (HTTP2.md § 2.2).
            if (mine && c->protocol == FETCH_PROTO_H2 && c->h2 != NULL && h2_conn_room(c->h2) &&
                (shared == NULL || c->order < shared->order))
                shared = c;
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
            // left for another. (An idle HTTP/2 connection is never "kept":
            // one with room was found above, and one without is no use.)
            if (c->protocol == FETCH_PROTO_H1 && !fresh && !idle_past(c, &now, idle_most_ms) &&
                (best == NULL || c->kept_at > best->kept_at))
                best = c;
        }
        if (shared != NULL) {
            shared->refs++;
            *conn = shared;
            answer = FETCH_POOL_SHARED;
        } else if (best != NULL) {
            best->refs = 1;
            *io = best->io;
            *conn = best;
            answer = FETCH_POOL_KEPT;
        } else if (dial_in_flight && remembered_h2(key)) {
            // The key answered h2 last time and a dial to it is in flight:
            // its verdict is worth waiting for, since one connection will
            // carry this fetch too. The dial's end, either way, rings.
            wait->for_the_table = false;
            wait_now = wait->bell != NULL && wait->bell->handle >= 0;
            if (wait_now)
                waiter_add(wait);
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
            // A connection whose socket is still open counts, in the table
            // or not: one struck and not yet closed by its reader holds a
            // handle, and the table's total is a promise about handles.
            size_t in_table = in_all - (slot != FETCH_POOL_TOTAL && slot != free_slot ? 1 : 0);
            bool admitted = in_table + s_closing < FETCH_POOL_TOTAL;
            if (slot != FETCH_POOL_TOTAL && admitted) {
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
                wait->for_the_table = of_key < FETCH_POOL_PER_KEY || slot != FETCH_POOL_TOTAL;
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
    if (conn->key.tls) {
        // Not h2: fetches waiting on this dial's verdict dial for themselves.
        remember(&conn->key, false);
        ring_waiters();
    }
    os64_lock_release(&s_lock);
}

// ── HTTP/2 connections (pool.h) ─────────────────────────────────────────

void fetch_pool_adopting(fetch_conn_t *conn)
{
    os64_lock_acquire(&s_lock);
    conn->dialling = false;
    conn->protocol = FETCH_PROTO_H2;
    conn->refs = 1;
    os64_lock_release(&s_lock);
}

void fetch_pool_dialled_h2(fetch_conn_t *conn, h2_conn_t *h2, uint16_t tls_version, bool tls_resumed)
{
    os64_lock_acquire(&s_lock);
    conn->h2 = h2;
    conn->tls_version = tls_version;
    conn->tls_resumed = tls_resumed;
    conn->order = ++s_order;
    remember(&conn->key, true);
    ring_waiters();
    os64_lock_release(&s_lock);
}

// The reader's question when its connection has no stream: has the pool
// done keeping it? Answered where a take would find the connection, and a
// yes strikes it in the same hold, so no take can be given a connection its
// reader is about to close.
static bool hook_idle_out(void *ctx, h2_conn_t *h2)
{
    fetch_conn_t *c = ctx;
    os64_ticks_t now;
    (void)h2;
    os64_lock_acquire(&s_lock);
    bool yes = !c->struck && c->h2 != NULL && c->refs == 0 && (!clock_now(&now) || expired(c, &now));
    if (yes)
        strike(c);
    os64_lock_release(&s_lock);
    return yes;
}

static void hook_room(void *ctx, h2_conn_t *h2)
{
    (void)ctx;
    (void)h2;
    os64_lock_acquire(&s_lock);
    ring_waiters();
    os64_lock_release(&s_lock);
}

// The reader's last word: the socket is closed. The connection leaves the
// table if it was still in it, stops counting as closing if it was, and is
// freed here when no fetch holds it; a fetch that does frees it at its put.
static void hook_dead(void *ctx, h2_conn_t *h2)
{
    fetch_conn_t *c = ctx;
    (void)h2;
    os64_lock_acquire(&s_lock);
    c->dead_heard = true;
    if (c->closing) {
        c->closing = false;
        s_closing--;
    }
    strike(c);
    bool last = c->refs == 0;
    ring_waiters();
    os64_lock_release(&s_lock);
    if (last)
        free_h2(c);
}

h2_conn_hooks_t fetch_pool_hooks(fetch_conn_t *conn)
{
    return (h2_conn_hooks_t){.ctx = conn, .idle_out = hook_idle_out, .room = hook_room, .dead = hook_dead};
}

h2_conn_t *fetch_pool_h2(fetch_conn_t *conn)
{
    os64_lock_acquire(&s_lock);
    h2_conn_t *h2 = conn->protocol == FETCH_PROTO_H2 ? conn->h2 : NULL;
    os64_lock_release(&s_lock);
    return h2;
}

void fetch_pool_tls_facts(fetch_conn_t *conn, uint16_t *version, bool *resumed)
{
    os64_lock_acquire(&s_lock);
    *version = conn->tls_version;
    *resumed = conn->tls_resumed;
    os64_lock_release(&s_lock);
}

void fetch_pool_h2_stats(size_t *connections, size_t *queued, size_t *buffered)
{
    *connections = *queued = *buffered = 0;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++) {
        fetch_conn_t *c = s_conns[i];
        if (c == NULL || c->protocol != FETCH_PROTO_H2 || c->h2 == NULL)
            continue;
        *connections += 1;
        *queued += h2_conn_queued(c->h2);
        *buffered += h2_conn_buffered(c->h2);
    }
    os64_lock_release(&s_lock);
}

size_t fetch_pool_closing(void)
{
    os64_lock_acquire(&s_lock);
    size_t n = s_closing;
    os64_lock_release(&s_lock);
    return n;
}

void fetch_pool_put(fetch_conn_t *conn, const fetch_transport_t *io)
{
    os64_ticks_t now;
    leaving_t out = {0};
    os64_lock_acquire(&s_lock);
    if (conn->protocol == FETCH_PROTO_H2) {
        // A fetch leaves an HTTP/2 connection: the connection goes on. Kept
        // from now when it was the last; freed when the connection had left
        // the table and its reader had finished before this fetch did.
        conn->refs--;
        if (conn->refs == 0)
            conn->kept_at = clock_now(&now) ? now.ticks : 0;
        bool last = conn->refs == 0 && conn->struck && conn->dead_heard;
        ring_waiters();
        os64_lock_release(&s_lock);
        if (last)
            free_h2(conn);
        return;
    }
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
    // An HTTP/1.1 one alone: closing it frees its handle before this
    // returns, which is what the caller is about to dial with. An HTTP/2
    // one's handle is freed when its reader next looks.
    for (size_t i = 0; i < FETCH_POOL_TOTAL; i++)
        if (s_conns[i] != NULL && idle(s_conns[i]) && s_conns[i]->protocol == FETCH_PROTO_H1 &&
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
