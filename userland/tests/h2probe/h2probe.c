// h2probe — does libfetch carry many fetches on one HTTP/2 connection, on
// this kernel, against a real server? (HTTP2.md § 4 H2.6)
//
//     h2probe [--cold] [--fixture] https://host/path [fetches]
//
// libfetch does not offer HTTP/2 yet (its switch is H2.5), so this program
// is built from libfetch's SOURCES, not against libfetch.so, and turns the
// library's private offer on for itself. Nothing else can.
//
// It makes one fetch first, so the server's ALPN answer is known and its
// connection is in the pool, then that many more at once (six unless told
// a number from 1 to 16), each on a thread of its own. Sixteen, because
// each fetch here holds two of this program's sixty-four handles while it
// runs: its thread's, and the bell an HTTP/2 fetch waits on. It prints each
// fetch's outcome, its bytes and its time, and then what the pool holds.
// Exit 0 when every fetch read its body whole and one HTTP/2 connection
// carried them all; 1 when a fetch failed; 2 when more than one connection
// was used, or none spoke HTTP/2.
//
// Whether the bodies must agree depends on the server. A live site may make
// its page anew for each request, so differing bodies there are printed and
// are not a failure. With --fixture the server is a local one serving a
// file, and bodies that differ are a failure (exit 1).
//
// --cold skips the first fetch. A server met for the first time may be
// dialled once for each fetch that finds no connection, up to libfetch's
// six to a host, and that is correct: the probe then prints how many
// connections the burst made and does not judge that count.
//
// --fixture verifies the server against libtls's test root in place of the
// system's store, for a local server holding the fixture certificate (its
// name is example.test).
//
// A hand probe, not a testrun fixture: it needs a network and a server.
#include "os64/os64.h"
#include "fetch/fetch.h"
#include "tls/tls.h"
#include "../../libfetch/pool.h"
#include "../../libtls/test/trust_vectors.h"

#define FETCHES_MAX 16

static const char *s_url;
static os64_tls_trust *s_trust;
static uint32_t s_gate;         // the threads start together when it opens

typedef struct {
    os64_fetch_status_t status;
    int32_t http;
    uint64_t bytes, sum;
    int64_t micros;
    char reason[OS64_FETCH_WHY_MAX];
} outcome_t;
static outcome_t s_outcome[FETCHES_MAX + 1];

static int64_t pem(void *context, void *out, size_t capacity)
{
    size_t *at = context, n = tls_fixture_a_pem_length - *at;
    if (n > capacity)
        n = capacity;
    os64_memcpy(out, tls_fixture_a_pem + *at, n);
    *at += n;
    return (int64_t)n;
}

static void fetch_one(outcome_t *o)
{
    static const os64_fetch_options_t kNone = {0};
    os64_fetch_options_t opt = kNone;
    opt.trust = s_trust;
    int64_t before = os64_micros();
    os64_fetch_t *f = os64_fetch_open(s_url, &opt);
    if (f == NULL) {
        o->status = OS64_FETCH_NO_MEMORY;
        os64_strcopy(o->reason, sizeof(o->reason), "out of memory");
        return;
    }
    const os64_fetch_head_t *head = os64_fetch_head(f);
    o->http = head != NULL ? head->status : 0;
    // On the heap: each fetch runs on a thread of its own, and os64 has no
    // thread-local storage to give each a buffer.
    enum { ROOM = 16384 };
    uint8_t *buf = os64_malloc(ROOM);
    for (;;) {
        int64_t n = buf != NULL ? os64_fetch_read(f, buf, ROOM) : -1;
        if (n <= 0)
            break;
        for (int64_t i = 0; i < n; i++)
            o->sum = o->sum * 31u + buf[i];
        o->bytes += (uint64_t)n;
    }
    os64_free(buf);
    o->micros = os64_micros() - before;
    o->status = os64_fetch_status(f);
    os64_strcopy(o->reason, sizeof(o->reason), os64_fetch_reason(f));
    os64_fetch_close(f);
}

static int64_t fetcher(void *arg)
{
    while (!__atomic_load_n(&s_gate, __ATOMIC_ACQUIRE))
        os64_yield();
    fetch_one(arg);
    return 0;
}

static void say(const char *who, const outcome_t *o)
{
    os64_printf("h2probe: %-8s %s, HTTP %d, %lu bytes, %ld ms%s%s\n", who, os64_fetch_status_name(o->status),
                (int)o->http, (unsigned long)o->bytes, (long)(o->micros / 1000),
                o->status == OS64_FETCH_OK ? "" : ": ", o->status == OS64_FETCH_OK ? "" : o->reason);
}

int main(int argc, char **argv)
{
    bool cold = false, fixture = false;
    int at = 1;
    for (; at < argc && argv[at][0] == '-'; at++) {
        if (os64_streq(argv[at], "--cold"))
            cold = true;
        else if (os64_streq(argv[at], "--fixture"))
            fixture = true;
        else
            at = argc;
    }
    uint64_t count = 6;
    if (at >= argc || argc - at > 2 ||
        (argc - at == 2 && (!os64_parse_u64(argv[at + 1], &count) || count < 1 || count > FETCHES_MAX))) {
        os64_printf("usage: h2probe [--cold] [--fixture] https://host/path [fetches, 1 to %d]\n", FETCHES_MAX);
        return 1;
    }
    s_url = argv[at];
    if (fixture) {
        size_t read = 0;
        os64_tls_store_detail_t detail;
        if (os64_tls_trust_load_pem(pem, &read, &s_trust, &detail) != OS64_TLS_STORE_OK) {
            os64_printf("h2probe: the fixture root could not be loaded\n");
            return 1;
        }
    }
    fetch_h2_offer = true;

    bool failed = false, differ = false;
    if (!cold) {
        fetch_one(&s_outcome[FETCHES_MAX]);
        say("first", &s_outcome[FETCHES_MAX]);
        failed = s_outcome[FETCHES_MAX].status != OS64_FETCH_OK;
    }
    int64_t threads[FETCHES_MAX];
    for (uint64_t i = 0; i < count; i++)
        threads[i] = os64_thread(fetcher, &s_outcome[i]);
    int64_t before = os64_micros();
    __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
    for (uint64_t i = 0; i < count; i++) {
        if (threads[i] < 0) {
            os64_printf("h2probe: thread %lu could not be started\n", (unsigned long)i + 1);
            failed = true;
            continue;
        }
        os64_thread_join((int32_t)threads[i], NULL);
    }
    int64_t all = os64_micros() - before;
    const outcome_t *like = cold ? &s_outcome[0] : &s_outcome[FETCHES_MAX];
    for (uint64_t i = 0; i < count; i++) {
        char who[16];
        os64_snprintf(who, sizeof(who), "%lu", (unsigned long)i + 1);
        say(who, &s_outcome[i]);
        failed = failed || s_outcome[i].status != OS64_FETCH_OK;
        differ = differ || s_outcome[i].bytes != like->bytes || s_outcome[i].sum != like->sum;
    }

    size_t kept = 0, live = 0, h2 = 0, queued = 0, buffered = 0;
    fetch_pool_counts(&kept, &live);
    fetch_pool_h2_stats(&h2, &queued, &buffered);
    os64_printf("h2probe: %lu at once in %ld ms; the pool holds %lu connection%s (%lu in use), %lu of them HTTP/2,"
                " with %lu bytes queued to send and %lu of receive buffer\n",
                (unsigned long)count, (long)(all / 1000), (unsigned long)(kept + live), kept + live == 1 ? "" : "s",
                (unsigned long)live, (unsigned long)h2, (unsigned long)queued, (unsigned long)buffered);
    fetch_pool_drain();
    // Outlive the connection's reader. The kernel kills whatever threads are
    // alive when a program exits and reports the program as killed (exit
    // 137) in place of the code it returned (DEBTS.md), and a drained
    // HTTP/2 connection's reader takes up to a slice to see that it was
    // condemned. The last sleep covers the reader's few steps between
    // telling the pool it is gone and returning.
    for (int waited = 0; fetch_pool_closing() != 0 && waited < 2000; waited += 20)
        os64_sleep(20);
    os64_sleep(50);
    if (failed) {
        os64_printf("h2probe: FAIL: a fetch failed\n");
        return 1;
    }
    if (differ && fixture) {
        os64_printf("h2probe: FAIL: the bodies differ, and the fixture server serves a file\n");
        return 1;
    }
    if (differ)
        os64_printf("h2probe: the bodies differ, as a page made anew for each request does\n");
    if (h2 == 0) {
        os64_printf("h2probe: FAIL: no connection spoke HTTP/2 (the server chose HTTP/1.1)\n");
        return 2;
    }
    if (!cold && kept + live != 1) {
        os64_printf("h2probe: FAIL: %lu connections, where one HTTP/2 connection carries them all\n",
                    (unsigned long)(kept + live));
        return 2;
    }
    os64_printf("h2probe: PASS\n");
    return 0;
}
