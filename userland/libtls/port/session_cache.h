#ifndef OS64_TLS_SESSION_CACHE_H
#define OS64_TLS_SESSION_CACHE_H
#include "tls13_engine.h"

// session_cache.h — where TLS 1.3 sessions wait to be offered (tls13_engine.h
// § Resumption). One cache for the process, under one lock.
//
// WHAT A SESSION MAY RESUME. A resumed handshake shows no certificate: the
// server is believed because it was believed before. So a session is filed
// under everything that earlier belief rested on, and offered only where all
// of it matches: the HOSTNAME the certificate was checked against (as the
// engine normalises it), the PORT (another port is another server), and the
// TRUST the chain was verified under (a session earned under one set of
// anchors says nothing under another).
//
// The trust is an identity: compared, never looked into. An entry keeps its
// own hold on it through `retain`/`release` when the key gives the pair, so
// that no other trust can come to live at the same address while the entry
// names it (libfetch's pool keys its kept connections the same way).
typedef struct {
    const char *hostname;
    uint16_t port;
    const void *trust;
    bool (*retain)(const void *trust);     // false: the hold could not be taken, and nothing is kept
    void (*release)(const void *trust);
} tls_session_key;

// The numbers. Servers commonly send two tickets a connection, and a ticket
// is offered once, so two a key is one connection's worth. Thirty-two in
// all, each the size of a session.
#define TLS_SESSION_PER_KEY 2u
#define TLS_SESSION_TOTAL   32u

// Keep a session for `key`, to live `lifetime` seconds from `now_ms` (any
// clock that only moves forward, the same one for every call). The least
// recently kept goes when its key holds TLS_SESSION_PER_KEY or the cache
// TLS_SESSION_TOTAL. A lifetime of zero keeps nothing.
void os64_tls_session_put(const tls_session_key *key, const tls13_session *session,
                          uint32_t lifetime, uint64_t now_ms);

// Take the most recently kept session for `key` that has not outlived its
// lifetime, with `age_ms` filled in: true, and it is the caller's alone. It
// is REMOVED: a ticket is offered once, so that somebody watching the wire
// cannot tie two connections together by the ticket they showed (RFC 8446
// C.4). The server that issued it always can. Sessions of the
// key found past their lifetime are dropped on the way.
bool os64_tls_session_take(const tls_session_key *key, uint64_t now_ms, tls13_session *out);

// How many sessions are kept for `key`, whatever their age. For the harness.
size_t os64_tls_session_count(const tls_session_key *key);
#endif
