// test_fetch_pool_host.c — libfetch's connections (pool.c) under threads:
// the proof that a connection belongs to one fetch between its take and its
// put (WIRE.md § 2), that a key never has more than FETCH_POOL_PER_KEY of
// them nor the pool more than FETCH_POOL_TOTAL, and that a fetch which has
// to wait for one is woken (HTTP2.md § 2.2).
//
// pool.c runs UNMODIFIED with its own lock; what it holds are fake
// connections whose handles this harness numbers, and the bells its waiters
// park on are tools/host_bell.c's. Each thread of the sweep is a fetch
// that, over and over, asks for a connection to one of three keys (dialling
// in the slot it is given when none is kept, waiting when the key's six are
// all in use), claims it, works on it, and gives it back — or closes it, as
// a fetch whose exchange did not allow keeping it would. A claim is an
// atomic exchange on the connection's owner, so two holders at once is
// caught where it happens; so is a close of a held connection, a second
// close, a take of a closed one, and a seventh connection to a key. The
// clock jumps now and then, so the idle expiry runs against the takers, and
// two of the threads also let everything kept go, or the least recent, now
// and then. At the end every connection ever made has been closed exactly
// once. Before the sweep, cases on their own: the stale clock, a request
// sent once, the system's trust store read once, and each rule of the cap
// and the wait.
//
// Run by tools/test_fetch_pool_host.sh twice: under ASan and UBSan, and
// under TSan, which sees a write to a kept connection's fields that the
// lock does not order.

#define _POSIX_C_SOURCE 200809L      // sem_timedwait, clock_gettime under -std=c11
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "host_bell.h"
#include "os64/os64.h"

#define THREADS 8
#define KEYS    3
#define ROUNDS  20000
#define HANDLES (THREADS * ROUNDS + 64)

static _Atomic int owner[HANDLES];       // 0: nobody; else the thread that holds it
static _Atomic int closed[HANDLES];
static _Atomic int next_handle = 1;
static _Atomic uint64_t clock_ms;
static _Atomic int failures;
static _Atomic size_t most_held;           // the most connections the pool was seen holding, kept and in use
static _Atomic int key_held[KEYS];         // the sweep's connections to each key, in use right now

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); atomic_fetch_add(&failures, 1); } } while (0)

void os64_yield(void) { sched_yield(); }

// The race case's hook: a thread marked `stalls` stops inside its clock
// read, says so, and waits up to 300 ms for the other thread's put.
static __thread bool stalls;
static sem_t read_the_clock, put_done;

int64_t os64_ticks(os64_ticks_t *t)
{
    *t = (os64_ticks_t){ .ticks = atomic_load(&clock_ms), .per_second = 1000 };
    if (stalls) {
        sem_post(&read_the_clock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 300 * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        sem_timedwait(&put_done, &until);
    }
    return 0;
}
// The pool's only way to let a connection go. One the pool closes is one
// nobody holds and nobody closed before.
void fetch_transport_close(fetch_transport_t *io, bool normal)
{
    (void)normal;
    int h = io->handle;
    CHECK(h > 0 && h < HANDLES);
    CHECK(atomic_load(&owner[h]) == 0);
    CHECK(atomic_exchange(&closed[h], 1) == 0);
    io->handle = -1;
}

static void claim(int h, int me)
{
    int nobody = 0;
    CHECK(atomic_compare_exchange_strong(&owner[h], &nobody, me));
    CHECK(atomic_load(&closed[h]) == 0);
}
static void unclaim(int h, int me)
{
    int mine = me;
    CHECK(atomic_compare_exchange_strong(&owner[h], &mine, 0));
}

// malloc for pool.c's connection objects.
void *os64_malloc(size_t size) { return malloc(size); }
void os64_free(void *p) { free(p); }

// ── A fetch, as far as the pool is concerned ────────────────────────────

typedef struct {
    fetch_conn_t *conn;
    fetch_transport_t io;
    os64_bell_t bell;
    fetch_pool_take_t answer;       // how its last ask ended
    bool reused;
    _Atomic bool cancelled;
    uint64_t waited_ms;
    bool for_the_table;
} fetch_t;

static bool fetch_cancelled(void *ctx) { return atomic_load(&((fetch_t *)ctx)->cancelled); }

// Ask the pool as fetch.c's acquire does: a kept connection is claimed, a
// slot is dialled in (a new handle) and reported dialled. True with a
// connection in hand.
static bool obtain(fetch_t *f, const fetch_pool_key_t *key, uint32_t idle_most, bool fresh,
                   uint32_t patience_ms, int me)
{
    fetch_pool_wait_t wait = {.bell = &f->bell, .patience_ms = patience_ms,
                              .cancelled = fetch_cancelled, .cancel_ctx = f};
    f->conn = NULL;
    // Every fetch here would say the same greeting: who may share an HTTP/2
    // connection is test_fetch_h2_host's to prove.
    static const fetch_pool_say_t kSay = {0};
    f->answer = fetch_pool_take(key, &kSay, idle_most, fresh, &wait, &f->conn, &f->io);
    f->waited_ms = wait.waited_ms;
    f->for_the_table = wait.for_the_table;
    if (f->answer == FETCH_POOL_KEPT) {
        // What was kept for this key comes back as it was given.
        CHECK(f->io.encrypted == key->tls && f->io.cancelled == NULL && f->io.cancel_ctx == NULL);
        f->reused = true;
    } else if (f->answer == FETCH_POOL_DIAL) {
        memset(&f->io, 0, sizeof(f->io));
        f->io.handle = atomic_fetch_add(&next_handle, 1);
        f->io.encrypted = key->tls;
        f->io.cancel_ctx = &f->io;      // a fetch's, which the pool must drop
        CHECK(f->io.handle < HANDLES);
        fetch_pool_dialled(f->conn);
        f->reused = false;
    } else {
        CHECK(f->conn == NULL);
        return false;
    }
    claim(f->io.handle, me);
    return true;
}
static void give_back(fetch_t *f, int me)
{
    unclaim(f->io.handle, me);
    fetch_pool_put(f->conn, &f->io);
    f->conn = NULL;
}
static void shut(fetch_t *f, int me)
{
    unclaim(f->io.handle, me);
    fetch_transport_close(&f->io, false);
    fetch_pool_gone(f->conn);
    f->conn = NULL;
}
static fetch_t fresh_fetch(void) { return (fetch_t){.bell = OS64_BELL_NONE}; }
// A case's slot takes over another fetch's connection, bell and all; the
// bell the slot had is closed, as its fetch closing would.
static void takes_over(fetch_t *slot, const fetch_t *from)
{
    os64_bell_close(&slot->bell);
    *slot = *from;
}

static void *fetcher(void *arg)
{
    int me = (int)(intptr_t)arg;
    unsigned rng = (unsigned)me * 2654435761u;
    // Three keys and eight threads: more threads than a key's six, so they
    // wait on one another, and more connections wanted (eighteen) than the
    // pool holds (sixteen), so the least recently kept of all is put out.
    fetch_pool_key_t keys[KEYS];
    for (int k = 0; k < KEYS; k++)
        keys[k] = (fetch_pool_key_t){ .tls = k & 1, .host = "one.test", .port = (uint16_t)(80 + k) };
    fetch_t f = fresh_fetch();
    for (int round = 0; round < ROUNDS; round++) {
        rng = rng * 1103515245u + 12345u;
        // Most asks go to one key, so that its six are usually all in use.
        int k = (rng >> 9) % 8 < 6 ? 0 : 1 + (int)((rng >> 13) % (KEYS - 1));
        const fetch_pool_key_t *key = &keys[k];
        // And everything kept is let go now and then, or the least recent
        // alone, while the others take and put.
        if (me == 1 && round % 997 == 0)
            fetch_pool_drain();
        if (me == 2 && round % 389 == 0)
            fetch_pool_evict_one();
        // A request sent once asks for a younger connection now and then,
        // and a retry for no kept one at all.
        uint32_t idle_most = (rng >> 20) % 4 == 0 ? FETCH_POOL_ONCE_MS : FETCH_POOL_IDLE_MS;
        bool fresh = (rng >> 23) % 16 == 0;
        // Patience beyond anything this clock will count: it jumps a minute
        // at a time, and a wait here ends by being rung or not at all.
        if (!obtain(&f, key, idle_most, fresh, UINT32_MAX, me)) {
            CHECK(!"a fetch of the sweep was refused a connection");
            continue;
        }
        // Never a seventh to one key, never a seventeenth in all.
        CHECK(atomic_fetch_add(&key_held[k], 1) < FETCH_POOL_PER_KEY);
        size_t kept_now, live_now;
        fetch_pool_counts(&kept_now, &live_now);
        size_t held = kept_now + live_now;
        CHECK(held <= FETCH_POOL_TOTAL);
        size_t seen = atomic_load(&most_held);
        while (held > seen && !atomic_compare_exchange_weak(&most_held, &seen, held))
            ;
        // The exchange: its fields are this thread's alone now.
        f.io.received += (uint64_t)round;
        if ((rng >> 4) % 3 == 0)
            sched_yield();
        atomic_fetch_sub(&key_held[k], 1);
        if ((rng >> 12) % 8 == 0)
            shut(&f, me);           // a reply that may not be kept
        else
            give_back(&f, me);
        // Now and then a minute passes, and what was kept expires.
        if ((rng >> 16) % 1024 == 0)
            atomic_fetch_add(&clock_ms, FETCH_POOL_IDLE_MS);
        else
            atomic_fetch_add(&clock_ms, 1);
    }
    os64_bell_close(&f.bell);
    return NULL;
}

// A TAKE'S CLOCK IS NEVER OLDER THAN A PUT IT SEES (the cold read's stale
// clock). One thread asks for another key and stops inside its clock read;
// meanwhile a connection is put, a tick later. Read under the lock, the
// put waits for the take, and its connection is kept; read before the
// lock, the take would carry its older `now` past the put and close the
// new connection as one kept in the future.
static fetch_pool_key_t race_a = { .host = "a.test", .port = 80 },
                        race_b = { .host = "b.test", .port = 80 };
static fetch_t race_held;
static void *race_taker(void *x)
{
    (void)x;
    fetch_t f = fresh_fetch();
    stalls = true;
    CHECK(obtain(&f, &race_b, FETCH_POOL_IDLE_MS, false, 1000, 91) && !f.reused);
    stalls = false;
    shut(&f, 91);
    return NULL;
}
static void *race_putter(void *x)
{
    (void)x;
    atomic_fetch_add(&clock_ms, 1);
    give_back(&race_held, 90);
    return NULL;
}
static void stale_clock_case(void)
{
    sem_init(&read_the_clock, 0, 0);
    sem_init(&put_done, 0, 0);
    race_held = fresh_fetch();
    CHECK(obtain(&race_held, &race_a, FETCH_POOL_IDLE_MS, false, 1000, 90));
    int kept = race_held.io.handle;
    pthread_t taker, putter;
    pthread_create(&taker, NULL, race_taker, NULL);
    sem_wait(&read_the_clock);            // the take has its `now`
    pthread_create(&putter, NULL, race_putter, NULL);
    // The put is waiting on the pool's lock, which the stalled take holds.
    struct timespec nap = {0, 50 * 1000000L};
    nanosleep(&nap, NULL);
    sem_post(&put_done);
    pthread_join(putter, NULL);
    pthread_join(taker, NULL);
    CHECK(atomic_load(&closed[kept]) == 0 && fetch_pool_idle() == 1);
    fetch_pool_drain();
}

// THE SYSTEM'S STORE IS READ ONCE, however many threads ask for it at once:
// the read is slow on purpose, every thread asks, one read happens, and
// every thread is handed the same store.
static _Atomic int store_reads, store_frees;
static char the_store;
os64_tls_store_status_t os64_tls_trust_reload(os64_tls_trust **current, os64_tls_store_report_t *report)
{
    (void)report;
    atomic_fetch_add(&store_reads, 1);
    for (int i = 0; i < 2000; i++)
        sched_yield();
    *current = (os64_tls_trust *)&the_store;
    return OS64_TLS_STORE_OK;
}
void os64_tls_trust_free(os64_tls_trust *store)
{
    CHECK(store == (os64_tls_trust *)&the_store);
    atomic_fetch_add(&store_frees, 1);
}
static os64_tls_trust *handed[THREADS];
static void *store_asker(void *arg)
{
    int me = (int)(intptr_t)arg;
    os64_tls_store_report_t report;
    CHECK(fetch_pool_trust(&handed[me], &report) == OS64_TLS_STORE_OK);
    return NULL;
}
static void one_store_case(void)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, store_asker, (void *)(intptr_t)i);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    CHECK(atomic_load(&store_reads) == 1);
    for (int i = 0; i < THREADS; i++)
        CHECK(handed[i] == (os64_tls_trust *)&the_store);
    fetch_pool_trust_drop();
    CHECK(atomic_load(&store_frees) == 1);
}

// A REQUEST SENT ONCE TAKES ONLY A CONNECTION USED MOMENTS AGO: one kept
// for FETCH_POOL_ONCE_MS is passed over, stays kept, and a request that
// may be sent again takes it; one younger than that is taken.
static void once_case(void)
{
    static const fetch_pool_key_t key = { .host = "once.test", .port = 443, .tls = true };
    fetch_t f = fresh_fetch(), other = fresh_fetch();
    CHECK(obtain(&f, &key, FETCH_POOL_IDLE_MS, false, 1000, 1) && !f.reused);
    int handle = f.io.handle;
    give_back(&f, 1);
    atomic_fetch_add(&clock_ms, FETCH_POOL_ONCE_MS - 1);
    CHECK(obtain(&f, &key, FETCH_POOL_ONCE_MS, false, 1000, 1) && f.reused && f.io.handle == handle);
    give_back(&f, 1);
    atomic_fetch_add(&clock_ms, FETCH_POOL_ONCE_MS);
    // Passed over: the key is nowhere near its cap, so it dials beside it.
    CHECK(obtain(&other, &key, FETCH_POOL_ONCE_MS, false, 1000, 2) && !other.reused);
    CHECK(fetch_pool_idle() == 1 && atomic_load(&closed[handle]) == 0);
    shut(&other, 2);
    CHECK(obtain(&f, &key, FETCH_POOL_IDLE_MS, false, 1000, 1) && f.reused && f.io.handle == handle);
    give_back(&f, 1);
    fetch_pool_drain();
}

// ── The cap and the wait (HTTP2.md § 2.2) ───────────────────────────────

static const fetch_pool_key_t cap_key = { .host = "cap.test", .port = 443, .tls = true };
// A caller that needs a handle back asks the pool to close one: it closes
// the connection that has been kept longest.
static void evict_order_case(void)
{
    static const fetch_pool_key_t keys[3] = { { .host = "first.test", .port = 80 },
                                              { .host = "second.test", .port = 80 },
                                              { .host = "third.test", .port = 80 } };
    fetch_t f[3];
    fetch_pool_drain();
    for (int i = 0; i < 3; i++) {
        f[i] = fresh_fetch();
        CHECK(obtain(&f[i], &keys[i], FETCH_POOL_IDLE_MS, false, 1000, 60 + i));
    }
    for (int i = 0; i < 3; i++) {
        give_back(&f[i], 60 + i);
        atomic_fetch_add(&clock_ms, 1);
    }
    CHECK(fetch_pool_evict_one() && fetch_pool_idle() == 2);
    CHECK(obtain(&f[0], &keys[0], FETCH_POOL_IDLE_MS, false, 1000, 60) && !f[0].reused);
    CHECK(obtain(&f[1], &keys[1], FETCH_POOL_IDLE_MS, false, 1000, 61) && f[1].reused);
    CHECK(obtain(&f[2], &keys[2], FETCH_POOL_IDLE_MS, false, 1000, 62) && f[2].reused);
    for (int i = 0; i < 3; i++) {
        shut(&f[i], 60 + i);
        os64_bell_close(&f[i].bell);
    }
    CHECK(!fetch_pool_evict_one());
}

static void nap_ms(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

typedef struct {
    fetch_t f;
    const fetch_pool_key_t *key;
    uint32_t idle_most, patience_ms;
    bool fresh, got;
    _Atomic bool answered;          // the ask is over: the one field another thread may read meanwhile
    int me;
} asker_t;
static void *asker(void *arg)
{
    asker_t *a = arg;
    a->got = obtain(&a->f, a->key, a->idle_most, a->fresh, a->patience_ms, a->me);
    atomic_store(&a->answered, true);
    return NULL;
}
static asker_t an_asker(const fetch_pool_key_t *key, int me)
{
    return (asker_t){.f = fresh_fetch(), .key = key, .idle_most = FETCH_POOL_IDLE_MS,
                     .patience_ms = 5000, .me = me};
}
// Wait until `n` more waits have parked than `before`: the askers are in
// the pool's list and asleep on their bells.
static void until_parked(unsigned long before, unsigned n)
{
    for (int i = 0; i < 2000 && host_bell_waits() < before + n; i++)
        nap_ms(1);
    CHECK(host_bell_waits() >= before + n);
}

static void cap_cases(void)
{
    fetch_t held[FETCH_POOL_TOTAL];
    for (int i = 0; i < FETCH_POOL_TOTAL; i++)
        held[i] = fresh_fetch();

    // Six to a key, each dialled; the seventh waits out its patience and is
    // told so, with nothing changed and nothing left in the list.
    for (int i = 0; i < FETCH_POOL_PER_KEY; i++)
        CHECK(obtain(&held[i], &cap_key, FETCH_POOL_IDLE_MS, false, 1000, 10 + i) && !held[i].reused);
    CHECK(fetch_pool_live() == FETCH_POOL_PER_KEY && fetch_pool_idle() == 0);
    fetch_t seventh = fresh_fetch();
    CHECK(!obtain(&seventh, &cap_key, FETCH_POOL_IDLE_MS, false, 60, 20));
    CHECK(seventh.answer == FETCH_POOL_TIMED_OUT && fetch_pool_live() == FETCH_POOL_PER_KEY);
    CHECK(!seventh.for_the_table);              // it waited on its own key's six
    // A retry, which takes no kept connection, waits at the cap all the same.
    CHECK(!obtain(&seventh, &cap_key, FETCH_POOL_IDLE_MS, true, 60, 20) && seventh.answer == FETCH_POOL_TIMED_OUT);

    // It proceeds when one is given back: on that very connection.
    asker_t a = an_asker(&cap_key, 21);
    pthread_t t;
    unsigned long parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    int given = held[0].io.handle;
    give_back(&held[0], 10);
    pthread_join(t, NULL);
    CHECK(a.got && a.f.reused && a.f.io.handle == given);
    takes_over(&held[0], &a.f);                 // six in use again, under thread 21's claim

    // It proceeds when one goes: in the slot that left.
    a = an_asker(&cap_key, 22);
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    shut(&held[0], 21);
    pthread_join(t, NULL);
    CHECK(a.got && !a.f.reused && fetch_pool_live() == FETCH_POOL_PER_KEY);
    takes_over(&held[0], &a.f);

    // Cancelled while it waits: it leaves at the next wake, takes nothing,
    // and is in the list no more. Its bell is closed at once, so a pool
    // that still held its node would ring a bell that is not one at the put
    // below, and host_bell.c stops the program there.
    a = an_asker(&cap_key, 23);
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    atomic_store(&a.f.cancelled, true);
    host_bell_interrupt(&a.f.bell);
    pthread_join(t, NULL);
    CHECK(!a.got && a.f.answer == FETCH_POOL_INTERRUPTED);
    os64_bell_close(&a.f.bell);
    os64_bell_close(&seventh.bell);
    give_back(&held[1], 11);
    CHECK(obtain(&held[1], &cap_key, FETCH_POOL_IDLE_MS, false, 1000, 11) && held[1].reused);

    // Cancelled before it ever asks: it does not park to find out.
    a = an_asker(&cap_key, 24);
    atomic_store(&a.f.cancelled, true);
    parked = host_bell_waits();
    CHECK(!obtain(&a.f, &cap_key, FETCH_POOL_IDLE_MS, false, 5000, 24) && a.f.answer == FETCH_POOL_INTERRUPTED);
    CHECK(host_bell_waits() == parked);
    os64_bell_close(&a.f.bell);

    // Cancelled, and then rung by a connection it could have had: it leaves
    // without it, and the connection stays kept for a fetch that is going on.
    a = an_asker(&cap_key, 25);
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    atomic_store(&a.f.cancelled, true);
    give_back(&held[1], 11);
    pthread_join(t, NULL);
    CHECK(!a.got && a.f.answer == FETCH_POOL_INTERRUPTED && fetch_pool_idle() == 1);
    os64_bell_close(&a.f.bell);
    CHECK(obtain(&held[1], &cap_key, FETCH_POOL_IDLE_MS, false, 1000, 11) && held[1].reused);

    // A look-up is not the end of the wait: one that has looked up and gone
    // back to sleep still gets the connection given back after.
    a = an_asker(&cap_key, 28);
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 2);                    // parked, looked up, parked again
    give_back(&held[1], 11);
    pthread_join(t, NULL);
    CHECK(a.got && a.f.reused);
    give_back(&a.f, 28);
    os64_bell_close(&a.f.bell);
    CHECK(obtain(&held[1], &cap_key, FETCH_POOL_IDLE_MS, false, 1000, 11) && held[1].reused);

    // Cancelled with nothing to ring it: it looks up, and leaves, long
    // before its patience is out. And one whose patience runs out while it
    // is cancelled was cancelled, not refused.
    a = an_asker(&cap_key, 26);
    a.patience_ms = 60000;
    parked = host_bell_waits();
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    atomic_store(&a.f.cancelled, true);
    pthread_join(t, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(!a.got && a.f.answer == FETCH_POOL_INTERRUPTED);
    CHECK(t1.tv_sec - t0.tv_sec < 20);          // a look or two, not its minute of patience
    os64_bell_close(&a.f.bell);
    a = an_asker(&cap_key, 27);
    a.patience_ms = 300;
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    atomic_store(&a.f.cancelled, true);
    pthread_join(t, NULL);
    CHECK(!a.got && a.f.answer == FETCH_POOL_INTERRUPTED);
    os64_bell_close(&a.f.bell);

    // A ring is a hint. Three wait; the one the pool reaches first has been
    // cancelled and is on its way out. One connection given back still
    // reaches one of the other two, and a second the other.
    asker_t w[3] = { an_asker(&cap_key, 30), an_asker(&cap_key, 31), an_asker(&cap_key, 32) };
    pthread_t wt[3];
    for (int i = 0; i < 3; i++) {
        parked = host_bell_waits();
        pthread_create(&wt[i], NULL, asker, &w[i]);
        until_parked(parked, 1);                // in order, so the last to park heads the list
    }
    atomic_store(&w[2].f.cancelled, true);
    give_back(&held[2], 12);
    pthread_join(wt[2], NULL);
    CHECK(!w[2].got && w[2].f.answer == FETCH_POOL_INTERRUPTED);
    for (int i = 0; i < 400 && !atomic_load(&w[0].answered) && !atomic_load(&w[1].answered); i++)
        nap_ms(5);
    CHECK(atomic_load(&w[0].answered) != atomic_load(&w[1].answered));    // one connection, one of the two
    give_back(&held[3], 13);
    pthread_join(wt[0], NULL);
    pthread_join(wt[1], NULL);
    CHECK(w[0].got && w[1].got && w[0].f.reused && w[1].f.reused);
    takes_over(&held[2], &w[0].f);
    takes_over(&held[3], &w[1].f);
    os64_bell_close(&w[2].f.bell);

    // A fetch that cannot have a bell cannot wait, and says so.
    fetch_t bellless = fresh_fetch();
    host_bell_refuse_make = true;
    CHECK(!obtain(&bellless, &cap_key, FETCH_POOL_IDLE_MS, false, 1000, 40) && bellless.answer == FETCH_POOL_FAILED);
    host_bell_refuse_make = false;

    // At the key's cap with a kept connection this request may not take:
    // one kept too long for a request sent once, and any at all for a
    // retry. That one is closed for the slot, and nobody waits.
    int kept = held[4].io.handle;
    give_back(&held[4], 14);
    atomic_fetch_add(&clock_ms, FETCH_POOL_ONCE_MS);
    CHECK(obtain(&held[4], &cap_key, FETCH_POOL_ONCE_MS, false, 1000, 14) && !held[4].reused);
    CHECK(atomic_load(&closed[kept]) == 1 && fetch_pool_idle() == 0 && fetch_pool_live() == FETCH_POOL_PER_KEY);
    kept = held[5].io.handle;
    give_back(&held[5], 15);
    CHECK(obtain(&held[5], &cap_key, FETCH_POOL_IDLE_MS, true, 1000, 15) && !held[5].reused);
    CHECK(atomic_load(&closed[kept]) == 1 && fetch_pool_idle() == 0);

    // Sixteen in all. With every one in use a new key waits; with one of
    // them kept, of whatever key, that one is closed and the new key dials.
    static const fetch_pool_key_t more[2] = { { .host = "more.test", .port = 80 }, { .host = "more.test", .port = 81 } };
    for (int i = FETCH_POOL_PER_KEY; i < FETCH_POOL_TOTAL; i++)
        CHECK(obtain(&held[i], &more[i < 11 ? 0 : 1], FETCH_POOL_IDLE_MS, false, 1000, 10 + i));
    CHECK(fetch_pool_live() == FETCH_POOL_TOTAL);
    static const fetch_pool_key_t newcomer = { .host = "new.test", .port = 80 };
    fetch_t seventeenth = fresh_fetch();
    CHECK(!obtain(&seventeenth, &newcomer, FETCH_POOL_IDLE_MS, false, 60, 50) && seventeenth.answer == FETCH_POOL_TIMED_OUT);
    CHECK(seventeenth.for_the_table);           // its key has none: the table was what it waited on
    kept = held[7].io.handle;
    give_back(&held[7], 17);
    CHECK(obtain(&seventeenth, &newcomer, FETCH_POOL_IDLE_MS, false, 1000, 50) && !seventeenth.reused);
    CHECK(atomic_load(&closed[kept]) == 1 && fetch_pool_idle() == 0 && fetch_pool_live() == FETCH_POOL_TOTAL);
    takes_over(&held[7], &seventeenth);

    // Patience is counted by the clock, across wakes: a waiter whose time
    // ran out while it slept is told so at the next ring, though the ring
    // (a connection of another key given back) brought it nothing.
    a = an_asker(&cap_key, 52);
    a.patience_ms = 100;
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    atomic_fetch_add(&clock_ms, 200);
    give_back(&held[12], 22);
    pthread_join(t, NULL);
    CHECK(!a.got && a.f.answer == FETCH_POOL_TIMED_OUT && a.f.waited_ms >= 200);
    os64_bell_close(&a.f.bell);
    CHECK(obtain(&held[12], &more[1], FETCH_POOL_IDLE_MS, false, 1000, 22) && held[12].reused);

    // And a slot that waits for a table place is rung when one goes.
    a = an_asker(&newcomer, 51);
    parked = host_bell_waits();
    pthread_create(&t, NULL, asker, &a);
    until_parked(parked, 1);
    shut(&held[8], 18);
    pthread_join(t, NULL);
    CHECK(a.got && !a.f.reused);
    takes_over(&held[8], &a.f);

    for (int i = 0; i < FETCH_POOL_TOTAL; i++) {
        // Whoever holds each by now: the claim is cleared as its holder's.
        atomic_store(&owner[held[i].io.handle], 0);
        fetch_transport_close(&held[i].io, false);
        fetch_pool_gone(held[i].conn);
        os64_bell_close(&held[i].bell);
    }
    os64_bell_close(&bellless.bell);
    CHECK(fetch_pool_live() == 0 && fetch_pool_idle() == 0);
}

int main(void)
{
    stale_clock_case();
    once_case();
    one_store_case();
    evict_order_case();
    cap_cases();
    CHECK(host_bell_alive() == 0);          // every bell a case made, it closed
    unsigned long waits_before = host_bell_waits();
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, fetcher, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    // The whole pool's cap was reached, and never passed; and the fetches
    // did wait for one another.
    CHECK(atomic_load(&most_held) == FETCH_POOL_TOTAL);
    unsigned long waits = host_bell_waits() - waits_before;
    CHECK(waits > 0);
    fetch_pool_drain();
    CHECK(fetch_pool_idle() == 0 && fetch_pool_live() == 0 && host_bell_alive() == 0);
    int made = atomic_load(&next_handle) - 1, shut_count = 0;
    for (int h = 1; h <= made; h++) {
        shut_count += atomic_load(&closed[h]);
        CHECK(atomic_load(&owner[h]) == 0);
    }
    CHECK(shut_count == made);
    if (atomic_load(&failures) != 0) {
        fprintf(stderr, "test_fetch_pool_host: %d failures\n", atomic_load(&failures));
        return 1;
    }
    printf("test_fetch_pool_host: %d threads, %d connections made, each held by one fetch at a time"
           " and closed once; never seven to a key or seventeen in all; %lu waits\n", THREADS, made, waits);
    return 0;
}
