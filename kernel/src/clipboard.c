// clipboard.c — the system's ONE clipboard. See clipboard.h for the contract
// and CLIPBOARD.md for the rulings; this file is the store itself.
//
// The whole store is two globals and a lock. That is not a placeholder — a
// clipboard IS one pointer to the newest thing somebody copied, and the
// entry/refcount machinery around it exists so that pointer can be swapped
// while readers are mid-read, and so history (later) is "stop dropping the
// last ref" rather than a rewrite.
//
// LOCK DISCIPLINE, stated once because it is the only subtle thing here:
// allocate and copy OUTSIDE the lock, swap pointers UNDER it, free the loser
// OUTSIDE it. kmalloc takes the allocator's own spinlock (kMemoryStatusLock,
// which page-fault paths also take), and MEMORY.md's rule is never to call
// allocate/free while holding another lock a fault path might want. Since the
// pending entry belongs to exactly one handle until the instant it is
// published, every byte of copying happens with no lock held at all — the
// lock covers a pointer swap and a refcount, nothing more.

#include "clipboard.h"
#include "memory/kmalloc.h"
#include "memcpy.h"
#include "spinlock.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — the glass half of a loud refusal
#include "CONFIG.h"

extern volatile uint64_t kTicksSinceStart;

// The store. kClipboardCurrent is the newest SEALED entry and the store holds
// one reference to it; NULL means nothing has ever been copied.
static spinlock_t     kClipboardLock    = 0;
static snarf_entry_t *kClipboardCurrent = NULL;

// A copy in progress — private to one open write handle (clipboard.h).
struct snarf_pending
{
	uint8_t *bytes;
	size_t   length;     // bytes accumulated so far
	size_t   cap;        // bytes allocated
	bool     poisoned;   // a refusal happened; this copy will never publish
};

// The first allocation of a copy. Most copies are a line or a paragraph, so
// start small and double — the same allocate-copy-free growth synth_text_t
// uses, for the same reason (kmalloc has no realloc).
#define CLIP_INITIAL_CAP 4096

// DEBUG_CLIPBOARD (bit 30, %g name "CLIP") since 2026-08-21. These lines rode
// DEBUG_VFS for exactly as long as /sys/clipboard was the only door — honest
// while every byte really was filesystem traffic. The text-console selection
// (vt_select.c) snarfs with no file anywhere in sight, so the bit was earned
// and added where it must always be added: CONFIG.h, klog_format.h's %g
// table, and log.c's static asserts, in one change.

snarf_pending_t *clipboard_begin(void)
{
	// Zeroed by the allocator (every allocation is — MEMORY.md), so the
	// pending starts empty, uncapped and unpoisoned with no field writes.
	return (snarf_pending_t *)kmalloc(sizeof(snarf_pending_t));
}

int clipboard_append(snarf_pending_t *pending, const void *bytes, size_t length)
{
	if (pending == NULL || bytes == NULL)
		return -1;

	// Once refused, always refused: the whole point of poisoning is that a
	// tool which ignores write() return values cannot go on to publish the
	// surviving fragment as if it were the copy.
	if (pending->poisoned)
		return -1;

	if (length == 0)
		return 0;

	size_t need = pending->length + length;

	if (need > CLIPBOARD_MAX_BYTES || need < pending->length /* overflow */)
	{
		// LOUD, on the glass as well as the wire. Whether the user learns of
		// this cannot depend on `grep` having good manners about its write
		// return value, and a 16MB refusal is rare and always user-caused.
		printd(DEBUG_CLIPBOARD, "CLIPBOARD: refused a copy of %lu bytes (ceiling %lu) — "
		       "the clipboard still holds what it held\n",
		       (unsigned long)need, (unsigned long)CLIPBOARD_MAX_BYTES);
		printf("CLIPBOARD: copy refused - over the %lu MB limit. Clipboard unchanged.\n",
		       (unsigned long)(CLIPBOARD_MAX_BYTES / (1024 * 1024)));
		goto poison;
	}

	if (need > pending->cap)
	{
		size_t newCap = pending->cap ? pending->cap : CLIP_INITIAL_CAP;
		while (newCap < need)
			newCap *= 2;
		if (newCap > CLIPBOARD_MAX_BYTES)
			newCap = CLIPBOARD_MAX_BYTES;   // `need` already fits; don't overshoot

		uint8_t *grown = (uint8_t *)kmalloc(newCap);
		if (grown == NULL)
		{
			printd(DEBUG_CLIPBOARD, "CLIPBOARD: out of memory growing a copy to %lu bytes\n",
			       (unsigned long)newCap);
			printf("CLIPBOARD: copy refused - out of memory. Clipboard unchanged.\n");
			goto poison;
		}
		if (pending->length > 0)
			memcpy(grown, pending->bytes, pending->length);
		if (pending->bytes != NULL)
			kfree(pending->bytes);   // kfree(NULL) PANICS in os64 — always guard
		pending->bytes = grown;
		pending->cap   = newCap;
	}

	memcpy(pending->bytes + pending->length, bytes, length);
	pending->length = need;
	return 0;

poison:
	// Let go of the fragment immediately. Nothing will ever read it, and
	// holding megabytes hostage until close would be a second insult.
	if (pending->bytes != NULL)
	{
		kfree(pending->bytes);
		pending->bytes = NULL;
	}
	pending->length   = 0;
	pending->cap      = 0;
	pending->poisoned = true;
	return -1;
}

void clipboard_seal(snarf_pending_t *pending)
{
	if (pending == NULL)
		return;

	if (pending->poisoned)
	{
		printd(DEBUG_CLIPBOARD, "CLIPBOARD: discarding a poisoned copy; the previous "
		       "snarf survives\n");
		kfree(pending);   // bytes were already freed at the refusal
		return;
	}

	snarf_entry_t *entry = (snarf_entry_t *)kmalloc(sizeof(snarf_entry_t));
	if (entry == NULL)
	{
		// The one case where a copy silently fails to publish, and it is the
		// case where saying so may also fail. Say it anyway.
		printd(DEBUG_CLIPBOARD, "CLIPBOARD: out of memory sealing a %lu byte copy\n",
		       (unsigned long)pending->length);
		if (pending->bytes != NULL)
			kfree(pending->bytes);
		kfree(pending);
		return;
	}

	// The pending's buffer becomes the entry's, verbatim and un-copied. It is
	// immutable from this line on.
	entry->bytes  = pending->bytes;
	entry->length = pending->length;
	entry->tick   = kTicksSinceStart;
	entry->type   = CLIP_TYPE_TEXT;
	entry->refs   = 1;                 // the store's own reference

	uint64_t flags = spinlock_acquire_irqsave(&kClipboardLock);
	snarf_entry_t *old = kClipboardCurrent;
	kClipboardCurrent  = entry;
	spinlock_release_irqrestore(&kClipboardLock, flags);

	// v1 POLICY, and the single line history changes: release the old. Any
	// reader still holding it keeps reading it; the last one out frees it.
	// "Keep the last N" replaces this one statement — nothing above it moves.
	clipboard_release(old);

	kfree(pending);

	printd(DEBUG_CLIPBOARD, "CLIPBOARD: sealed %lu bytes at tick %lu\n",
	       (unsigned long)entry->length, (unsigned long)entry->tick);
}

snarf_entry_t *clipboard_acquire(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&kClipboardLock);
	snarf_entry_t *entry = kClipboardCurrent;
	if (entry != NULL)
		entry->refs++;
	spinlock_release_irqrestore(&kClipboardLock, flags);
	return entry;
}

void clipboard_release(snarf_entry_t *entry)
{
	if (entry == NULL)
		return;

	uint64_t flags = spinlock_acquire_irqsave(&kClipboardLock);
	int32_t refs = --entry->refs;
	spinlock_release_irqrestore(&kClipboardLock, flags);

	if (refs > 0)
		return;

	// Last one out. Freeing happens with no lock held — see the file header.
	if (entry->bytes != NULL)
		kfree(entry->bytes);
	kfree(entry);
}

size_t clipboard_length(void)
{
	uint64_t flags = spinlock_acquire_irqsave(&kClipboardLock);
	size_t length = kClipboardCurrent ? kClipboardCurrent->length : 0;
	spinlock_release_irqrestore(&kClipboardLock, flags);
	return length;
}
