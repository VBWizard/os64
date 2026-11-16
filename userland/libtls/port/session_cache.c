// session_cache.c — the process's kept TLS 1.3 sessions. See session_cache.h
// for what a session may resume and why it is filed as it is.
//
// ONE LOCK, HELD ONLY TO FIND, COPY AND STRIKE. A trust is released, and a
// struck session wiped, after the lock is let go: a release may free a
// store, and neither needs the table.
#include "session_cache.h"
#include "../include/tls/tls.h"   // os64_tls_sessions_drop is the public one
#include "os64/lock.h"
#include "os64/str.h"

typedef struct {
    bool used;
    char hostname[254];
    uint16_t port;
    const void *trust;
    void (*release)(const void *trust);
    uint64_t kept_at, serial;     // when it arrived, and the order it arrived in
    uint32_t lifetime;            // seconds
    tls13_session session;
} entry;

static os64_lock_t s_lock = OS64_LOCK_INIT;
static entry s_entries[TLS_SESSION_TOTAL];
static uint64_t s_serial;

static bool same_key(const entry *e, const tls_session_key *key)
{
    return e->used && e->port == key->port && e->trust == key->trust &&
           os64_streq(e->hostname, key->hostname);
}

// A clock behind the keeping needs no test of its own: the difference is
// unsigned, so it reads as an age no lifetime reaches.
static bool outlived(const entry *e, uint64_t now_ms)
{
    return (now_ms - e->kept_at) / 1000u >= e->lifetime;
}

// What was struck under the lock and is finished outside it.
typedef struct {
    const void *trust[TLS_SESSION_TOTAL];
    void (*release[TLS_SESSION_TOTAL])(const void *trust);
    size_t count;
} leaving;

// Under the lock: the entry stops being one. Its secret goes here, since the
// slot may be given out again the moment the lock is released.
static void strike(leaving *out, entry *e)
{
    if (e->release) { out->trust[out->count] = e->trust; out->release[out->count++] = e->release; }
    tls13_wipe(e, sizeof *e);
}

static void let_go(leaving *out)
{
    for (size_t i = 0; i < out->count; i++) out->release[i](out->trust[i]);
}

void os64_tls_session_put(const tls_session_key *key, const tls13_session *session,
                          uint32_t lifetime, uint64_t now_ms)
{
    if (!key || !key->hostname || !session || !lifetime ||
        !session->ticket_len || session->ticket_len > TLS13_TICKET_MAX ||
        os64_strlen(key->hostname) >= sizeof s_entries[0].hostname)
        return;
    // The entry's own hold, before the table is touched: it is the one step
    // that can refuse.
    if (key->retain && !key->retain(key->trust)) return;

    leaving gone = {.count = 0};
    os64_lock_acquire(&s_lock);
    entry *slot = NULL, *oldest = NULL, *oldest_of_key = NULL;
    size_t of_key = 0;
    for (size_t i = 0; i < TLS_SESSION_TOTAL; i++) {
        entry *e = &s_entries[i];
        if (!e->used) { if (!slot) slot = e; continue; }
        if (!oldest || e->serial < oldest->serial) oldest = e;
        if (same_key(e, key)) {
            of_key++;
            if (!oldest_of_key || e->serial < oldest_of_key->serial) oldest_of_key = e;
        }
    }
    if (of_key >= TLS_SESSION_PER_KEY) { strike(&gone, oldest_of_key); slot = oldest_of_key; }
    else if (!slot) { strike(&gone, oldest); slot = oldest; }
    slot->used = true;
    os64_strcopy(slot->hostname, sizeof slot->hostname, key->hostname);
    slot->port = key->port;
    slot->trust = key->trust;
    slot->release = key->retain ? key->release : NULL;
    slot->kept_at = now_ms;
    slot->serial = ++s_serial;
    slot->lifetime = lifetime;
    slot->session = *session;
    slot->session.age_ms = 0;
    os64_lock_release(&s_lock);
    let_go(&gone);
}

bool os64_tls_session_take(const tls_session_key *key, uint64_t now_ms, tls13_session *out)
{
    if (!key || !key->hostname || !out) return false;
    leaving gone = {.count = 0};
    bool found = false;
    os64_lock_acquire(&s_lock);
    entry *newest = NULL;
    for (size_t i = 0; i < TLS_SESSION_TOTAL; i++) {
        entry *e = &s_entries[i];
        if (!same_key(e, key)) continue;
        if (outlived(e, now_ms)) { strike(&gone, e); continue; }
        if (!newest || e->serial > newest->serial) newest = e;
    }
    if (newest) {
        *out = newest->session;
        // obfuscated_ticket_age is thirty-two bits of milliseconds; seven
        // days of them, the longest a session lives, is well inside that.
        out->age_ms = (uint32_t)(now_ms - newest->kept_at);
        strike(&gone, newest);
        found = true;
    }
    os64_lock_release(&s_lock);
    let_go(&gone);
    return found;
}

size_t os64_tls_session_count(const tls_session_key *key)
{
    if (!key || !key->hostname) return 0;
    size_t count = 0;
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < TLS_SESSION_TOTAL; i++) count += same_key(&s_entries[i], key);
    os64_lock_release(&s_lock);
    return count;
}

void os64_tls_sessions_drop(void)
{
    leaving gone = {.count = 0};
    os64_lock_acquire(&s_lock);
    for (size_t i = 0; i < TLS_SESSION_TOTAL; i++)
        if (s_entries[i].used) strike(&gone, &s_entries[i]);
    os64_lock_release(&s_lock);
    let_go(&gone);
}
