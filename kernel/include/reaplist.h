#ifndef REAPLIST_H
#define REAPLIST_H

// The list of ended threads whose stacks are still to be released, and each
// thread's place in that lifecycle (THREAD_STACKS.md § The reap). Pure: no
// kernel headers and no lock. The caller holds the one lock that guards a
// list (kReapLock) across every call that takes the list, so
// tools/test_reaplist_host.sh can run it beside a model.
//
//   NONE     never handed: the thread runs, or its stacks go at burial
//   PENDING  handed and on the list, stacks still mapped
//   RUNNING  claimed: its claimer is releasing the stacks, off the list
//   DONE     released; never on the list again
//
// A node goes NONE -> PENDING -> RUNNING -> DONE and no other way. Each
// call refuses, changing nothing, a node not in the state it starts from,
// so a node is released at most once whoever reaches it.

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	REAP_NONE = 0,
	REAP_PENDING,
	REAP_RUNNING,
	REAP_DONE,
} reap_state_t;

// What a task holds of the list: its threads handed and not yet DONE. Written
// under the list's lock, read without it as a hint (a creator's backpressure).
typedef struct
{
	uint32_t pending;
} reap_owner_t;

typedef struct reap_node
{
	struct reap_node *next;
	reap_owner_t *owner;
	uint8_t state;   // reap_state_t
} reap_node_t;

typedef struct
{
	reap_node_t *head, *tail;   // oldest first
	uint32_t queued;            // PENDING nodes on the list
	uint32_t queued_most, window_queued_most;
	uint64_t handed, claimed_next, claimed_named, finished, refused;
	uint64_t window_handed, window_finished;
} reaplist_t;

typedef struct
{
	uint32_t queued, queued_most, window_queued_most;
	uint64_t handed, claimed_next, claimed_named, finished, refused;
	uint64_t window_handed, window_finished;
} reaplist_counts_t;

// NONE -> PENDING: on the list's tail, counted against `owner`.
bool reaplist_hand(reaplist_t *l, reap_node_t *n, reap_owner_t *owner);

// The oldest PENDING node -> RUNNING, off the list; NULL when it is empty.
reap_node_t *reaplist_claim_next(reaplist_t *l);

// This node, PENDING -> RUNNING, off the list. False for any other state.
bool reaplist_claim(reaplist_t *l, reap_node_t *n);

// RUNNING -> DONE, no longer counted against its owner.
bool reaplist_finish(reaplist_t *l, reap_node_t *n);

// The counts, since the list began and since the last read; the read starts
// the window again.
void reaplist_read(reaplist_t *l, reaplist_counts_t *out);

#endif
