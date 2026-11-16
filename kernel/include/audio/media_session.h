// media_session.h — the lock, the waits and the names around the media keys
// (docs/design/pending/MEDIA_KEYS.md). The rule itself is media_keys.c's.

#ifndef MEDIA_SESSION_H
#define MEDIA_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/mediakeys.h"

// ── The handle (HANDLE_MEDIA_KEYS) ──────────────────────────────────────────
// One open of /dev/mediakeys is one session, held by one handle: it is never
// shared, so its handle's close is its close. Its memory is held by the
// handle and by each operation in flight (handle.c § The pin).

typedef struct media_session media_session_t;

// A new session for the calling task, its lineage taken now. NULL with
// *refusal set: OS64_MEDIA_KEYS_FULL (every session is taken) or -1 (no
// memory). Task context.
media_session_t *media_session_open(long *refusal);
void media_session_ref(media_session_t *s);
void media_session_release(media_session_t *s);
// The handle goes: the session is struck from the table, so no press can
// reach it again, and a reader parked in it wakes to CLOSED. A spinlock and
// nothing else: legal with interrupts off and under any address space.
void media_session_close(media_session_t *s);

#define MEDIA_SESSION_ERR_CLOSED       (-1)   // the handle was closed under the call
#define MEDIA_SESSION_ERR_INTERRUPTED  (-3)   // signal_park_must_end
#define MEDIA_SESSION_ERR_TIMEOUT      (-5)   // the deadline passed

// The next press for this session, waiting for one until `deadline` (a
// tick; 0 = no limit). 1 and the record; or an error above; or
// OS64_MEDIA_KEYS_BUSY when another thread is already waiting here. Task
// context only.
long media_session_read(media_session_t *s, os64_media_key_t *out, uint64_t deadline);
// A record a read took and could not hand to its caller: counted lost.
void media_session_undelivered(media_session_t *s);
// The program's state, one word (`playing`, `paused`, `stopped`, optional
// trailing whitespace). 0, or -1 for anything else and for a closed
// session. Never waits.
long media_session_write(media_session_t *s, const char *text, size_t length);

// ── The presses ─────────────────────────────────────────────────────────────

// One press of `key` (OS64_MEDIA_KEY_*), routed by the rule now: to a
// session, or, with no session to take it, Play and Pause to the stream's
// own pause and resume. From kusb and from /sys/mediakeys' writer, at once
// if need be: the session lock is all that serializes them. Any thread
// that may take a spinlock; never waits.
void media_key_pressed(uint32_t key);

// /sys/mediakeys: `press play|pause|next|previous|stop`, one command a
// write (optional trailing whitespace). 0, or -1 for anything else.
long media_keys_command(const char *text, size_t length);
// /sys/mediakeys' text.
size_t media_keys_report(char *out, size_t capacity);

#endif
