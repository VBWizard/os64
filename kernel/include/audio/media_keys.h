// media_keys.h — which program a media key is for, as arithmetic
// (docs/design/pending/MEDIA_KEYS.md).
//
// PURE: no kernel headers, no lock, no clock. The caller holds its lock
// around every call, owns the sessions' storage, and passes the time and
// the focus word. tools/test_media_keys_host.sh runs it beside a model.

#ifndef MEDIA_KEYS_H
#define MEDIA_KEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os64/mediakeys.h"

#define MEDIA_KEYS_SESSIONS 32u   // the machine's sessions (the wall)
#define MEDIA_KEYS_RING      8u   // a session's unread presses
#define MEDIA_KEYS_LINEAGE   8u   // the opener and its ancestors kept

// A session's word for its program.
#define MEDIA_KEYS_STOPPED 0u
#define MEDIA_KEYS_PLAYING 1u
#define MEDIA_KEYS_PAUSED  2u

typedef struct media_keys_session
{
	uint8_t  state;              // MEDIA_KEYS_STOPPED .. _PAUSED
	bool     passed_full;        // the last press passed it over: its ring was full
	// The opener's task ID and its ancestors', nearest first, as at open.
	uint64_t lineage[MEDIA_KEYS_LINEAGE];
	uint32_t lineage_len;
	// Stamps from the table's sequence: the last time it began to say
	// `playing`, and its last activity (listed, a state write, a delivery).
	uint64_t playing_seq;
	uint64_t active_seq;
	// The presses not yet read: `count` from `head`.
	os64_media_key_t ring[MEDIA_KEYS_RING];
	uint32_t head;
	uint32_t count;
	uint32_t serial;             // presses given, the last one's number
	uint64_t delivered;          // presses read
	uint64_t lost;               // taken and then not delivered (read into memory that was not there)
} media_keys_session_t;

// How a press was answered (media_keys_table_t.last_rule).
#define MEDIA_KEYS_RULE_PLAYING  1u   // the newest session saying `playing`
#define MEDIA_KEYS_RULE_FOCUS    2u   // the session of the focused window's program
#define MEDIA_KEYS_RULE_ACTIVE   3u   // the session most recently active
#define MEDIA_KEYS_RULE_KERNEL   4u   // no session would take it: the kernel's

typedef struct media_keys_table
{
	media_keys_session_t *slot[MEDIA_KEYS_SESSIONS];
	uint32_t listed;
	uint64_t seq;                // the stamp's source: one a stamp, never reused
	// The last press, as routed.
	uint32_t last_key;           // as pressed
	uint32_t last_delivered;     // as given (the toggle may turn one into the other), 0 when none was
	uint32_t last_rule;          // MEDIA_KEYS_RULE_*, 0 before the first press
	int32_t  last_slot;          // the slot it went to, -1 when none
	uint64_t last_focus;         // the focus word it was routed against
	uint64_t presses;            // every press routed
	uint64_t unanswered;         // ones the kernel could not answer either
} media_keys_table_t;

void media_keys_table_init(media_keys_table_t *t);

// A fresh session (`stopped`, nothing to read) remembering `n` of the
// lineage given, nearest first (more than MEDIA_KEYS_LINEAGE are dropped
// from the far end).
void media_keys_session_init(media_keys_session_t *s, const uint64_t *lineage, uint32_t n);

// List a session, stamped active: its slot, or -1 when every slot is taken.
int media_keys_add(media_keys_table_t *t, media_keys_session_t *s);
// Strike it, so no press can reach it again. Not listed is no harm.
void media_keys_remove(media_keys_table_t *t, media_keys_session_t *s);

// One word, with optional trailing whitespace: true and the state, or
// false for anything else.
bool media_keys_parse_state(const char *text, size_t length, uint8_t *state);
// The session says `state` (stamped active; and beginning to say `playing`
// stamps that too).
void media_keys_set_state(media_keys_table_t *t, media_keys_session_t *s, uint8_t state);

// What routing a press decided.
typedef struct
{
	media_keys_session_t *target;   // given the press, or NULL
	uint32_t delivered;             // the key it was given as
	bool kernel_toggle;             // no target, and it is Play or Pause: the kernel pauses, else resumes
} media_keys_route_t;

// Route one press of `key` (OS64_MEDIA_KEY_*) arriving at `now_us` with the
// focused window's owner `focus` (0 for none): pick the target by the rule,
// passing over every session whose ring is full (so a ring never drops a
// press), turn Play and Pause into the target's toggle, append it to the
// target's ring and stamp it active. With no target, Next, Previous and Stop
// are counted unanswered here; Play and Pause are the caller's to try
// (media_keys_kernel_failed when both of the stream's verbs refuse).
media_keys_route_t media_keys_press(media_keys_table_t *t, uint32_t key, uint64_t focus, uint64_t now_us);
void media_keys_kernel_failed(media_keys_table_t *t);

// The next unread press: true and it, or false when none.
bool media_keys_take(media_keys_session_t *s, os64_media_key_t *out);
// A press taken that never reached its reader: counted lost.
void media_keys_undelivered(media_keys_session_t *s);

#endif
