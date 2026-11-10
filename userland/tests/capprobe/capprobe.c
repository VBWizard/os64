// capprobe — how many connections does libfetch hold to one host at once,
// and do the fetches that had to wait for one get it? (HTTP2.md § 2.2)
//
//     capprobe http://host/path [fetches]
//
// Starts that many threads (twelve unless told a number from 2 to 32; twelve is
// as many as yonder has workers), and each opens the address at the same moment, holds its
// connection for a third of a second with the body unread, and closes. With
// six connections to a host and no more, the first six open at once and the
// rest wait in libfetch's queue for one of those to go, each woken by a
// close. The probe counts how many fetches held a connection at the same
// instant, and gives the shortest and the longest open: the longest is a
// fetch that waited its turn, about one hold longer than the shortest for
// every six ahead of it.
//
// A hand probe, not a testrun fixture: it needs a network and a server.
// Exit 0 when every fetch opened and never more than six held a connection
// together; 1 when one failed; 2 when more than six did.
#include "os64/os64.h"
#include "fetch/fetch.h"

#define FETCHES_MAX 32
#define HOLD_MS     300
#define PER_HOST    6       // libfetch's FETCH_POOL_PER_KEY, which is private to it

static const char *s_url;
static uint32_t s_gate;                 // the threads start together when it opens
static uint32_t s_holding, s_most;      // fetches with a connection now, and the most at once
static uint32_t s_failed;
static int64_t s_opened_after[FETCHES_MAX];   // microseconds from the gate to each open's answer

static int64_t fetcher(void *arg)
{
    int me = (int)(int64_t)arg;
    while (!__atomic_load_n(&s_gate, __ATOMIC_ACQUIRE))
        os64_yield();
    int64_t before = os64_micros();
    os64_fetch_t *f = os64_fetch_open(s_url, NULL);
    s_opened_after[me] = os64_micros() - before;
    if (f == NULL || os64_fetch_status(f) != OS64_FETCH_OK) {
        if (f != NULL) {
            os64_printf("capprobe: fetch %d failed: %s\n", me + 1, os64_fetch_reason(f));
            os64_fetch_close(f);
        }
        __atomic_add_fetch(&s_failed, 1, __ATOMIC_ACQ_REL);
        return 1;
    }
    uint32_t now = __atomic_add_fetch(&s_holding, 1, __ATOMIC_ACQ_REL);
    uint32_t most = __atomic_load_n(&s_most, __ATOMIC_ACQUIRE);
    while (now > most && !__atomic_compare_exchange_n(&s_most, &most, now, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        ;
    os64_sleep(HOLD_MS);
    __atomic_sub_fetch(&s_holding, 1, __ATOMIC_ACQ_REL);
    os64_fetch_close(f);        // body unread: the connection goes, and a waiter dials in its place
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        os64_printf("usage: capprobe http://host/path [fetches]\n");
        return 1;
    }
    s_url = argv[1];
    unsigned count = 0;
    if (argc == 3)
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++)
            count = count * 10 + (unsigned)(*p - '0');
    if (count < 2 || count > FETCHES_MAX)
        count = 12;

    int64_t threads[FETCHES_MAX];
    for (unsigned i = 0; i < count; i++) {
        threads[i] = os64_thread(fetcher, (void *)(int64_t)i);
        if (threads[i] < 0) {
            os64_printf("capprobe: no thread %u (%ld)\n", i + 1, threads[i]);
            return 1;
        }
    }
    int64_t began = os64_micros();
    __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; i++) {
        os64_thread_join((int32_t)threads[i], NULL);
        os64_close((int32_t)threads[i]);
    }
    int64_t took = os64_micros() - began;

    int64_t shortest = s_opened_after[0], longest = 0;
    for (unsigned i = 0; i < count; i++) {
        if (s_opened_after[i] < shortest)
            shortest = s_opened_after[i];
        if (s_opened_after[i] > longest)
            longest = s_opened_after[i];
    }
    os64_printf("capprobe: %u fetches, %u failed; at most %u held a connection at once (the cap is %d); "
                "opens took %ld to %ld ms; %ld ms in all\n",
                count, s_failed, s_most, PER_HOST, shortest / 1000, longest / 1000, took / 1000);
    if (s_failed)
        return 1;
    return s_most > PER_HOST ? 2 : 0;
}
