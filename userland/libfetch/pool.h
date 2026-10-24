#ifndef FETCH_POOL_H
#define FETCH_POOL_H
// pool.h — the connections libfetch keeps between fetches, so one connection
// carries many requests (WIRE.md § 2). Private to the library: callers see
// only fetches, and a fetch takes a connection here before it dials and
// gives it back at close when connection_reusable says it may.
//
// ONE LOCK, HELD ONLY TO TAKE OR PUT. A connection belongs to exactly one
// fetch between its take and its put; the pool never reads or writes one it
// does not hold, and closes what it evicts after the lock is let go.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    // TLS: the caller's trust store, or NULL for the store the library loads
    // itself. A kept TLS client holds its own reference to the store it was
    // verified under (tls.h), so no other store can be made at the same
    // address while the connection is kept, and the pointer is an identity.
    const os64_tls_trust *trust;
} fetch_pool_key_t;

// THE NUMBERS. Six idle connections for one key is Chrome's per-host
// figure, and more than that to one server is a burst a server reads as
// abuse; thirty-two in all bounds the handles and the TLS state a browser
// holds idle (each TLS client keeps its record buffers). The least recently
// used goes first when either is full. Sixty seconds idle and a connection
// is closed when next seen: most servers close theirs sooner, which the
// probe at take and the retry rule (fetch.c) absorb, and one that keeps it
// longer is not kept waiting on.
#define FETCH_POOL_PER_KEY 6
#define FETCH_POOL_TOTAL   32
#define FETCH_POOL_IDLE_MS 60000u

// A kept connection for `key`, the most recently kept first: true with `*io`
// the caller's to own. Connections past their idle time are closed on the
// way, whatever their key. The caller probes what it takes
// (fetch_transport_idle_ok) and closes it if the probe says no.
bool fetch_pool_take(const fetch_pool_key_t *key, fetch_transport_t *io);

// Give a connection to the pool, which owns it from here: kept, or closed at
// once when the clock cannot be read. Evicts the least recently used of its
// key past FETCH_POOL_PER_KEY, and of all past FETCH_POOL_TOTAL. The
// connection's cancellation is dropped: it names the fetch that is leaving.
void fetch_pool_put(const fetch_pool_key_t *key, const fetch_transport_t *io);

// Close every kept connection. For a program that wants none left behind,
// and for the host harness, which checks for leaks at exit.
void fetch_pool_drain(void);

// How many connections are kept now.
size_t fetch_pool_idle(void);

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
