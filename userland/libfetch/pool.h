#ifndef FETCH_POOL_H
#define FETCH_POOL_H
// pool.h — the connections libfetch holds, in use and kept between fetches,
// so one connection carries many requests (WIRE.md § 2) and one host is
// spoken to over a bounded number of them (HTTP2.md § 2.2). Private to the
// library: callers see only fetches, and a fetch asks here for a connection
// before it dials and gives it back at close when connection_reusable says
// it may.
//
// A CONNECTION IS AN OBJECT WITH A COUNT (fetch_conn_t). The pool's table
// holds every one libfetch has: DIALLING (a slot reserved while its dial is
// in flight), LIVE (in use; the count is the fetches using it), or IDLE
// (kept; the count is 0). All three count toward the two caps below, which
// is what makes them caps on what a server sees and not only on what is
// kept.
//
// AN HTTP/1.1 CONNECTION belongs to exactly one fetch between its take and
// its put: the pool reads its key and its state to count it and leaves its
// transport alone, and closes what it evicts after the lock is let go.
//
// AN HTTP/2 CONNECTION is shared: every fetch with a stream on it holds a
// count, its transport is the connection's own for life (h2_conn.h), and
// nobody but its reader closes its socket. Its life, from the dial:
//
//   DIALLING   fetch_pool_take answered DIAL. Count 0, no reader.
//   ADOPTING   fetch_pool_adopting: the dialler's count, taken BEFORE
//              h2_conn_adopt starts the reader, so a peer that fails at
//              once cannot have the record freed under the dialler.
//   LIVE       fetch_pool_dialled_h2. Takes join it, puts leave it.
//   STRUCK     out of the table for good: its reader said it died, or
//              that it idled out; or it was evicted for a slot or drained
//              (and condemned). No take finds it.
//   freed      by whoever sees the last of three things: struck, the last
//              fetch's put, and the reader's last word. The reader's hooks
//              are handed the record, so it must outlive the reader.
//
// A struck connection whose reader has yet to close the socket is CLOSING
// and still counts against FETCH_POOL_TOTAL, which is a promise about the
// handles libfetch holds and not about a table.
//
// ONE LOCK, HELD ONLY TO TAKE, PUT OR LET GO, to edit the waiters' list and
// to ring the bells on it. Never across a dial, a probe, a close or a wait.
// It is taken BEFORE an HTTP/2 connection's own lock (the pool asks a
// connection whether it has room, and condemns it, under this one), and the
// connection's hooks arrive with none of the connection's locks held.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/bell.h"
#include "os64/file.h"
#include "h2_conn.h"
#include "fetch/http.h"
#include "fetch/transport.h"
#include "tls/tls.h"

// What a kept connection is for. Two fetches may share one only when every
// field matches.
typedef struct {
    bool tls;                       // TLS to the origin (direct https)
    // A plain connection to a proxy, which carries any host it serves
    // (RFC 9112 § 9.3 applies to the hop), so `host` and `port` are the
    // proxy's. libfetch sends https through a proxy in absolute form, plain
    // to the proxy (fetch.c's ask), so a proxied connection is never TLS.
    bool via_proxy;
    char host[HTTP_HOST_MAX];       // the peer dialled: the origin's host, or the proxy's
    uint16_t port;
    // TLS: the store it was verified under — the caller's, or the system's
    // that every fetch without one shares (fetch_pool_trust). A kept TLS
    // client holds its own reference to it (tls.h), so no other store can be
    // made at the same address while the connection is kept, and the
    // pointer is an identity.
    const os64_tls_trust *trust;
} fetch_pool_key_t;

// THE NUMBERS. Six connections to one key is Chrome's per-host figure, and
// more than that to one server is a burst a server reads as abuse. Sixteen
// in all is a quarter of a task's handle table (OS64_TASK_HANDLES): every
// connection holds a handle, in use or kept, and the program holds the rest
// — its standard three, its files, its windows, a bell for each fetch that
// had to wait — so libfetch's connections may never be what runs a task out
// of handles. When a dial is refused for want of resources all the same, a
// kept connection is closed and the dial tried once more (fetch.c's dial).
// Sixty seconds idle and a connection is closed when next seen: most
// servers close theirs sooner, which the probe at take and the retry rule
// (fetch.c) absorb, and one that keeps it longer is not kept waiting on.
#define FETCH_POOL_PER_KEY 6
#define FETCH_POOL_TOTAL   16
#define FETCH_POOL_IDLE_MS 60000u
// A REQUEST THAT MAY NOT BE SENT TWICE (a POST, fetch.c's idempotent says
// which) takes a kept connection only when it was given back less than
// this long ago, and dials one of its own otherwise. The retry rule cannot
// save it from a server that closed the connection while it sat idle, so
// it does not ride one that may have been closed: the shortest idle time
// a common server keeps (Apache's KeepAliveTimeout and Node's
// keepAliveTimeout, 5 s each) is more than twice this, while a page's own
// burst of requests, a script's POST right behind the requests that loaded
// the page, still shares the connection. A connection it passes over stays
// kept for a request that can be sent again, unless this one needs its slot
// to dial in (fetch_pool_take).
#define FETCH_POOL_ONCE_MS 2000u
// HOW LONG A FETCH WAITS FOR A CONNECTION when every one that stands in its
// way is in use. Not the fetch's idle deadline: that measures a server's
// silence, and a fetch in this queue is waiting on its own program's other
// fetches, each of which is making progress or will be ended by its own
// deadline. A browser's queue has no limit at all; this one has a long
// one, so that connections held by fetches nobody is reading end as a
// named refusal and not as a wait nobody can explain.
#define FETCH_POOL_WAIT_MS 300000u
// A WAITING FETCH LOOKS UP THIS OFTEN to ask whether it was cancelled. A
// cancellation is a flag the caller sets, which rings nothing, so a waiter
// that slept its whole patience would learn of it only when some other
// fetch gave a connection back.
#define FETCH_POOL_LOOK_MS 1000u
_Static_assert(FETCH_POOL_ONCE_MS < FETCH_POOL_IDLE_MS, "a request sent once asks less of a connection's age");
_Static_assert(FETCH_POOL_TOTAL <= OS64_TASK_HANDLES / 4,
               "libfetch's connections may hold at most a quarter of a task's handles");
_Static_assert(FETCH_POOL_PER_KEY <= FETCH_POOL_TOTAL, "a key's cap is within the whole's");

// What a connection speaks: what its TLS handshake's ALPN answer said, for
// its whole life (HTTP2.md § 2.1).
typedef enum { FETCH_PROTO_H1 = 1, FETCH_PROTO_H2 } fetch_proto_t;

// WHETHER A DIAL OFFERS HTTP/2. False at rest, and nothing in the library
// sets it: libfetch.so exports no way to reach it, so a program linked
// against the library speaks HTTP/1.1. The host harness and /tests/h2probe
// compile these sources into themselves and set it (HTTP2.md § 4 H2.6).
extern bool fetch_h2_offer;

typedef struct fetch_conn fetch_conn_t;

// What a fetch brings to the wait: its bell (OS64_BELL_NONE until its first
// wait makes it; the fetch closes it), its patience, and its cancellation.
// `node` is the pool's while a take is waiting and nobody's otherwise.
typedef struct fetch_pool_wait {
    os64_bell_t *bell;
    uint32_t patience_ms;                   // how long a take may wait in all
    bool (*cancelled)(void *ctx);           // NULL = never
    void *cancel_ctx;
    uint64_t waited_ms;                     // out: how long the take did wait
    bool for_the_table;                     // out: its last wait was for a place in the whole
                                            // table, its key being under its own cap
    struct {
        struct fetch_pool_wait *next;
    } node;
} fetch_pool_wait_t;

typedef enum {
    // A kept connection of the key, now the caller's, its transport in *io.
    // The caller probes it (fetch_transport_idle_ok); one the probe refuses
    // it closes and reports with fetch_pool_gone, then asks again.
    FETCH_POOL_KEPT,
    // A slot for a connection the caller is to dial. fetch_pool_dialled
    // when the dial succeeds (fetch_pool_adopting and fetch_pool_dialled_h2
    // when it answered h2), fetch_pool_gone when it does not.
    FETCH_POOL_DIAL,
    // A live HTTP/2 connection of the key that had room for a stream when
    // it was asked, now counted as this fetch's too. Leave to try: the
    // fetch opens its stream (fetch_pool_h2), and a connection that then
    // says H2_NO_ROOM is given back (fetch_pool_put) and the pool asked
    // again. `*io` is untouched.
    FETCH_POOL_SHARED,
    FETCH_POOL_TIMED_OUT,       // waited out its patience
    FETCH_POOL_INTERRUPTED,     // its cancellation said stop
    FETCH_POOL_FAILED           // no bell, no memory or no clock to wait by
} fetch_pool_take_t;

// A connection for `key`, in this order (HTTP2.md § 2.2):
//   1. a live HTTP/2 connection with room for a stream, the one longest
//      live first (FETCH_POOL_SHARED);
//   2. unless `fresh`, a kept HTTP/1.1 one given back less than
//      `idle_most_ms` ago (FETCH_POOL_IDLE_MS, or FETCH_POOL_ONCE_MS for a
//      request sent once), the most recently kept first;
//   3. when the key last answered ALPN with h2 and a dial to it is in
//      flight, a WAIT for that dial's verdict, even under the key's cap;
//   4. a slot to dial in, when the key holds fewer than FETCH_POOL_PER_KEY
//      connections of every kind and the sockets libfetch holds, closing
//      ones among them, are under FETCH_POOL_TOTAL. AN IDLE CONNECTION
//      NEVER MAKES A FETCH WAIT FOR ONE longer than a closing HTTP/2
//      connection's reader takes to look (H2_READER_SLICE_MS): with the
//      table full, the least recently kept of any key is closed for the
//      slot; with the key at its cap and holding a kept connection this
//      fetch may not take (`fresh`, or kept too long for it), that one is;
//   5. otherwise the fetch WAITS, on its bell, until a connection is given
//      back, goes, finishes its dial or gains room, and asks again from
//      the top.
// Connections past their idle time are closed on the way, whatever their
// key. Every waiter is rung at every put and every departure: a ring is a
// hint, and each one woken asks again under the lock. No order is promised
// among waiters.
fetch_pool_take_t fetch_pool_take(const fetch_pool_key_t *key, uint32_t idle_most_ms, bool fresh,
                                  fetch_pool_wait_t *wait, fetch_conn_t **conn, fetch_transport_t *io);

// The dial a FETCH_POOL_DIAL slot was reserved for succeeded: the
// connection is in use, by the fetch that dialled it.
void fetch_pool_dialled(fetch_conn_t *conn);

// The dial answered h2: the dialler's count on the connection, taken before
// h2_conn_adopt starts a reader whose hooks (fetch_pool_hooks) name it.
// After this, a failed adoption is still fetch_pool_gone.
void fetch_pool_adopting(fetch_conn_t *conn);
// The hooks an HTTP/2 connection made for this record is given.
h2_conn_hooks_t fetch_pool_hooks(fetch_conn_t *conn);
// The adoption succeeded: `h2` is the connection's, with the adopter's hold
// on it, and takes may join it — unless its reader has already said it
// died, in which case it stays out of the table and the dialler, whose
// stream it will refuse, gives it back like any other. The key is
// remembered as h2, and fetches waiting on this dial are rung. The TLS
// facts are kept for the fetches that join (fetch_pool_tls_facts).
void fetch_pool_dialled_h2(fetch_conn_t *conn, h2_conn_t *h2, uint16_t tls_version, bool tls_resumed);
// The HTTP/2 connection a fetch holding `conn` opens its stream on; NULL
// for an HTTP/1.1 connection.
h2_conn_t *fetch_pool_h2(fetch_conn_t *conn);
void fetch_pool_tls_facts(fetch_conn_t *conn, uint16_t *version, bool *resumed);

// Give a connection in use back. HTTP/1.1: to be kept, with its transport,
// which is the pool's from here: kept, or closed at once when the clock
// cannot be read. The transport's cancellation is dropped: it names the
// fetch that is leaving. HTTP/2: the fetch's count alone (`io` is not
// read); the connection goes on, kept from now when no fetch is left on it.
void fetch_pool_put(fetch_conn_t *conn, const fetch_transport_t *io);

// A connection that is no more: a dial that failed (an adoption that
// failed among them), a kept HTTP/1.1 one its probe refused, one its fetch
// closed. The caller has closed its transport, or never had one. Its slot
// is free from here. Never an HTTP/2 connection that was adopted: those
// end by their reader.
void fetch_pool_gone(fetch_conn_t *conn);

// Close every kept connection. For a program that wants none left behind,
// and for the host harness, which checks for leaks at exit. Connections in
// use are their fetches'. An HTTP/2 one is condemned and is closed by its
// reader within a slice: fetch_pool_closing says how many have yet to be.
void fetch_pool_drain(void);
size_t fetch_pool_closing(void);

// Close the least recently used kept HTTP/1.1 connection, whatever its key:
// true when there was one. For a dial refused for want of resources (a handle,
// a port, memory), which a kept connection may be holding.
bool fetch_pool_evict_one(void);

// How many connections are kept now, and how many are in use or being
// dialled. fetch_pool_counts reads both in one look, for a caller that
// adds them up.
size_t fetch_pool_idle(void);
size_t fetch_pool_live(void);
void fetch_pool_counts(size_t *kept, size_t *live);

// ── The system's trust store, one per process ───────────────────────────
//
// What a fetch whose caller gives no store verifies against: the store
// tls.conf names, read on the first call any fetch makes and shared by
// every one after, so each connection verified without a store of its
// caller's is verified under the same one, and keyed by it. The library
// holds one reference and each TLS client its own (tls.h), so the store
// lives while anything uses it. One lock, held across the first read, so
// two threads asking at once read it once. A read that fails is not kept:
// the next call reads again, and `report` says what failed. An edit to the
// store reaches the programs started after it.
os64_tls_store_status_t fetch_pool_trust(os64_tls_trust **out, os64_tls_store_report_t *report);

// Let the library's reference go, so the next fetch reads the store again
// (a connection verified under the old one keeps it, and its key: a fresh
// read is another store). For the host harnesses, between cases.
void fetch_pool_trust_drop(void);

// ── Whether a connection may be kept (RFC 9112 § 9.3) ───────────────────

// The facts of one exchange, each a clause of the rule.
typedef struct {
    bool request_http11;        // the request line said HTTP/1.1
    bool request_close;         // the request said `Connection: close`
    // Every byte of the request went: a server that answered before an
    // upload was whole (a 401 or a 413 to a POST) stopped it partway, and
    // the server is still reading a body that will never end.
    bool request_whole;
    bool response_http11;       // the status line said HTTP/1.1, or a later 1.x
    bool response_close;        // the reply named `close` in its Connection line(s)
    // A reply with no body whatever its head says (RFC 9112 § 6.3): the
    // answer to a HEAD, a 204, a 304. (A 1xx is interim and never the reply.)
    bool bodiless;
    http_framing_t framing;     // otherwise how its body was framed
    bool consumed;              // ...and every byte that framing promised was read off the wire
    bool leftover;              // bytes past the message have arrived: nobody asked for them
    // The connection itself: no transport or TLS failure, no closure begun
    // or heard, not cancelled, and the fetch did not end in a failure (one
    // whose framing or coding is refused closes its connection before it
    // is ever judged).
    bool transport_ok;
} fetch_reuse_facts_t;

// Pure: true when every clause allows keeping the connection. A body read to
// the close (HTTP_FRAMING_CLOSE) never does, since the close was its end.
bool connection_reusable(const fetch_reuse_facts_t *facts);
#endif
