// The reap list's lifecycle (reaplist.h). Pure; the caller holds the lock.

#include "reaplist.h"

#include <stddef.h>

bool reaplist_hand(reaplist_t *l, reap_node_t *n, reap_owner_t *owner)
{
	if (n->state != REAP_NONE)
	{
		l->refused++;
		return false;
	}
	n->state = REAP_PENDING;
	n->owner = owner;
	n->next = NULL;
	if (l->tail != NULL)
		l->tail->next = n;
	else
		l->head = n;
	l->tail = n;
	l->queued++;
	if (l->queued > l->queued_most)
		l->queued_most = l->queued;
	if (l->queued > l->window_queued_most)
		l->window_queued_most = l->queued;
	l->handed++;
	l->window_handed++;
	__atomic_add_fetch(&owner->pending, 1, __ATOMIC_RELAXED);
	return true;
}

// Off the list, `prev` being the node before it (NULL at the head).
static void unlink_after(reaplist_t *l, reap_node_t *prev, reap_node_t *n)
{
	if (prev != NULL)
		prev->next = n->next;
	else
		l->head = n->next;
	if (l->tail == n)
		l->tail = prev;
	n->next = NULL;
	n->state = REAP_RUNNING;
	l->queued--;
}

reap_node_t *reaplist_claim_next(reaplist_t *l)
{
	reap_node_t *n = l->head;
	if (n == NULL)
		return NULL;
	unlink_after(l, NULL, n);
	l->claimed_next++;
	return n;
}

bool reaplist_claim(reaplist_t *l, reap_node_t *n)
{
	if (n->state != REAP_PENDING)
	{
		l->refused++;
		return false;
	}
	reap_node_t *prev = NULL;
	for (reap_node_t *at = l->head; at != NULL; prev = at, at = at->next)
		if (at == n)
		{
			unlink_after(l, prev, n);
			l->claimed_named++;
			return true;
		}
	// PENDING and not on this list: a node handed to another list.
	l->refused++;
	return false;
}

bool reaplist_finish(reaplist_t *l, reap_node_t *n)
{
	if (n->state != REAP_RUNNING)
	{
		l->refused++;
		return false;
	}
	n->state = REAP_DONE;
	__atomic_sub_fetch(&n->owner->pending, 1, __ATOMIC_RELAXED);
	l->finished++;
	l->window_finished++;
	return true;
}

void reaplist_read(reaplist_t *l, reaplist_counts_t *out)
{
	out->queued = l->queued;
	out->queued_most = l->queued_most;
	out->window_queued_most = l->window_queued_most;
	out->handed = l->handed;
	out->claimed_next = l->claimed_next;
	out->claimed_named = l->claimed_named;
	out->finished = l->finished;
	out->refused = l->refused;
	out->window_handed = l->window_handed;
	out->window_finished = l->window_finished;
	l->window_queued_most = l->queued;
	l->window_handed = 0;
	l->window_finished = 0;
}
