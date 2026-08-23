#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// clipboard.h — the system's ONE clipboard, kernel half. Design: CLIPBOARD.md
// (ratified 2026-08-20), which carries the arguments; this header carries the
// contract.
//
// WHY THE KERNEL OWNS IT
// A clipboard is shared state between programs that do not know each other
// exist — a scribe window, a terminal, a shell pipeline. That is the same
// reason the pipe ring lives in kernel memory (pipe.h): the arbiter of shared
// bytes must be the one party every participant already trusts. Userland gets
// at it through /sys/clipboard, the FILE (sysfs.c) — which is why copying is
// `> /sys/clipboard` and pasting is `cat /sys/clipboard`, and why every text
// utility in the OS was clipboard-aware the day the file appeared. The lineage
// is Plan 9's: rio served its snarf buffer as /dev/snarf, and X11's two
// buffers (PRIMARY vs CLIPBOARD) are the mistake being declined here.
//
// ONE BUFFER, AND ENTRIES ANYWAY
// There is exactly one live snarf at a time — an idle selection clobbers an
// explicit copy, ruled and accepted. But the store keeps *entries*, not "a
// buffer", because history is a design constraint TODAY and a feature LATER:
// the day it arrives, the v1 policy line ("seal the new, release the old")
// becomes "keep the last N" and nothing else about this file changes.
//
// IMMUTABLE + REFCOUNTED
// A sealed entry never changes a byte. A reader ACQUIREs it (one ref) and
// reads straight out of it for as long as its handle lives — no copy, even at
// 16MB — and RELEASEs at close. "Release the old" is therefore a ref drop, not
// a free: a `cat` in flight when somebody else copies keeps reading the entry
// it opened, whole and self-consistent. That is synthfs's snapshot doctrine
// (an internally consistent file beats a fresh one) bought for free.

// The ceiling. Generous — a clipboard should hold a whole log excerpt — but a
// clipboard is not a filesystem, and an unbounded one is a memory leak with a
// friendly API (pipe.h's argument, same shape). Crossing it POISONS the copy
// in progress: the seal publishes nothing and the previous snarf survives
// untouched, because a truncated paste that LOOKS complete is the failure
// "tripwires over silence" exists to prevent.
#define CLIPBOARD_MAX_BYTES (16 * 1024 * 1024)

// The type seat, reserved and unset in v1: everything is bytes-presumed-text.
// When images or richer payloads arrive they ride THIS field instead of
// forcing a second clipboard — X11's second-system mistake, declined in
// advance (CLIPBOARD.md).
#define CLIP_TYPE_TEXT 0

// One snarf. Sealed once, then read-only forever.
typedef struct snarf_entry
{
	uint8_t *bytes;    // kmalloc'd; NULL iff length == 0 (an empty copy is a
	                   // legitimate copy — `grep nothing > /sys/clipboard`
	                   // empties the clipboard, exactly as it would a file)
	size_t   length;
	uint64_t tick;     // when it was sealed — history's timestamp, already here
	uint32_t type;     // CLIP_TYPE_* — the seat above
	int32_t  refs;     // the store holds 1; each open reader holds 1 more
} snarf_entry_t;

// A copy in progress. Opaque: it belongs to the open write handle and to
// nobody else, which is what makes "a multi-write copy is one snarf" true.
typedef struct snarf_pending snarf_pending_t;

// Begin a copy. NULL if memory is short (the caller should refuse the open).
snarf_pending_t *clipboard_begin(void);

// Append to a copy in progress. 0 = accepted, -1 = REFUSED (over the ceiling,
// or out of memory) — and a refusal poisons the pending entry, so every later
// append fails too and the seal publishes nothing. A tool that ignores write
// return values therefore still cannot publish half a copy.
int clipboard_append(snarf_pending_t *pending, const void *bytes, size_t length);

// Finish a copy: publish it as the newest entry and release the old one.
// A poisoned pending is discarded instead, leaving the previous snarf in
// place. Frees the pending either way — it never survives this call.
//
// SEAL-ON-DEATH IS DELIBERATE (ruled 2026-08-21): the handle closer cannot
// tell "the program returned" from "the program was killed", and a killed
// `grep > somefile` leaves a partial file on disk without anyone finding it
// surprising. The clipboard does not invent a rule the rest of the tree
// doesn't have.
void clipboard_seal(snarf_pending_t *pending);

// Throw a copy away UNPUBLISHED: free the pending and whatever it collected,
// and leave the store exactly as it was. Frees the pending, like seal does.
//
// This is NOT the counterpart of seal for a program that changed its mind —
// seal-on-death above says a copy that reached the store gets published, and
// that ruling stands. It is for the OPEN that never completed: clipboard_begin
// succeeded, something after it failed, and open() is about to return -1. The
// caller never got a handle, so no copy was ever made, and sealing there would
// publish an empty entry and ERASE the user's clipboard as the reward for an
// allocation failure. (Codex review, 2026-08-22.)
void clipboard_discard(snarf_pending_t *pending);

// Take a reference on the newest sealed entry, or NULL if nothing has ever
// been copied (a never-used clipboard reads as an empty file, not an error).
snarf_entry_t *clipboard_acquire(void);

// Drop a reference taken by clipboard_acquire(). Frees at zero. NULL is fine.
void clipboard_release(snarf_entry_t *entry);

// How many bytes the newest entry holds, without taking a reference — what
// stat() reports, so `ls -l /sys` tells the truth and userland can size a
// paste before reading it.
size_t clipboard_length(void);

#endif
