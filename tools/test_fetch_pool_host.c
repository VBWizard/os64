// test_fetch_pool_host.c — libfetch's kept connections (pool.c) under
// threads: the proof that a connection belongs to one fetch between its
// take and its put (WIRE.md § 2, "two threads taking from one key never
// sharing a connection").
//
// pool.c runs UNMODIFIED with its own lock; what it holds are fake
// connections whose handles this harness numbers. Each thread is a fetch
// that, over and over, takes a connection for one of two keys (dialling a
// new one when none is kept), claims it, works on it, and gives it back —
// or closes it, as a fetch whose exchange did not allow keeping it would.
// A claim is an atomic exchange on the connection's owner, so two holders
// at once is caught where it happens; so is a close of a held connection, a
// second close, and a take of a closed one. The clock jumps now and then, so
// the idle expiry runs against the takers. At the end every connection ever
// made has been closed exactly once.
//
// Run by tools/test_fetch_pool_host.sh twice: under ASan and UBSan, and
// under TSan, which sees a write to a kept connection's fields that the
// lock does not order.

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "os64/os64.h"

#define THREADS 4
#define ROUNDS  20000
#define HANDLES (THREADS * ROUNDS + 64)

static _Atomic int owner[HANDLES];       // 0: nobody; else the thread that holds it
static _Atomic int closed[HANDLES];
static _Atomic int next_handle = 1;
static _Atomic uint64_t clock_ms;
static _Atomic int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); atomic_fetch_add(&failures, 1); } } while (0)

void os64_yield(void) { sched_yield(); }
int64_t os64_ticks(os64_ticks_t *t)
{
    *t = (os64_ticks_t){ .ticks = atomic_load(&clock_ms), .per_second = 1000 };
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

static void *fetcher(void *arg)
{
    int me = (int)(intptr_t)arg;
    unsigned rng = (unsigned)me * 2654435761u;
    fetch_pool_key_t keys[2] = { { .host = "one.test", .port = 80 },
                                 { .tls = true, .host = "one.test", .port = 443 } };
    for (int round = 0; round < ROUNDS; round++) {
        rng = rng * 1103515245u + 12345u;
        const fetch_pool_key_t *key = &keys[(rng >> 9) & 1];
        fetch_transport_t io;
        if (fetch_pool_take(key, &io)) {
            // What was kept for this key comes back as it was given.
            CHECK(io.encrypted == key->tls && io.cancelled == NULL && io.cancel_ctx == NULL);
            claim(io.handle, me);
        } else {
            memset(&io, 0, sizeof(io));
            io.handle = atomic_fetch_add(&next_handle, 1);
            io.encrypted = key->tls;
            io.cancel_ctx = &io;       // a fetch's, which the pool must drop
            CHECK(io.handle < HANDLES);
            claim(io.handle, me);
        }
        // The exchange: its fields are this thread's alone now.
        io.received += (uint64_t)round;
        if ((rng >> 4) % 3 == 0)
            sched_yield();
        unclaim(io.handle, me);
        if ((rng >> 12) % 8 == 0) {
            // A reply that may not be kept: the fetch closes its connection.
            fetch_transport_close(&io, false);
        } else {
            fetch_pool_put(key, &io);
        }
        size_t idle = fetch_pool_idle();
        CHECK(idle <= FETCH_POOL_TOTAL);
        // Now and then a minute passes, and what was kept expires.
        if ((rng >> 16) % 1024 == 0)
            atomic_fetch_add(&clock_ms, FETCH_POOL_IDLE_MS);
        else
            atomic_fetch_add(&clock_ms, 1);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, fetcher, (void *)(intptr_t)(i + 1));
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    // Two keys, so never more than twice the per-key cap kept.
    CHECK(fetch_pool_idle() <= 2 * FETCH_POOL_PER_KEY);
    fetch_pool_drain();
    CHECK(fetch_pool_idle() == 0);
    int made = atomic_load(&next_handle) - 1, shut = 0;
    for (int h = 1; h <= made; h++) {
        shut += atomic_load(&closed[h]);
        CHECK(atomic_load(&owner[h]) == 0);
    }
    CHECK(shut == made);
    if (atomic_load(&failures) != 0) {
        fprintf(stderr, "test_fetch_pool_host: %d failures\n", atomic_load(&failures));
        return 1;
    }
    printf("test_fetch_pool_host: %d threads, %d connections made, each held by one fetch at a time"
           " and closed once\n", THREADS, made);
    return 0;
}
