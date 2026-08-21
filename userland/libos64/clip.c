// clip.c — libos64's clipboard helpers. See os64/clip.h for the contract and
// CLIPBOARD.md for the design.
//
// Every one of these is open/seek/read/write/close against /sys/clipboard and
// nothing else. There is deliberately no clipboard syscall to wrap: the file
// IS the interface, and a program that would rather spell it out longhand
// gets exactly the same clipboard these functions do.

#include "os64/clip.h"
#include "os64/io.h"

int64_t os64_clip_copy(const void *bytes, uint64_t length)
{
	if (bytes == NULL && length > 0)
		return -1;

	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "w");
	if (h < 0)
		return h;

	// The copy is not visible to anyone until close — the kernel seals the
	// whole handle's worth of writes as ONE snarf entry, which is what makes
	// a loop like this a single copy rather than N of them.
	uint64_t sent = 0;
	int64_t  failure = 0;

	while (sent < length)
	{
		int64_t n = os64_write((int32_t)h, (const uint8_t *)bytes + sent,
		                       (size_t)(length - sent));
		if (n <= 0)
		{
			// A refusal from the clipboard has already POISONED this copy
			// kernel-side, so the close below publishes nothing and the
			// previous clipboard survives. No abort verb is needed (or
			// offered) — the refusal is the abort.
			failure = (n < 0) ? n : -1;
			break;
		}
		sent += (uint64_t)n;
	}

	os64_close((int32_t)h);   // the seal
	return failure ? failure : (int64_t)sent;
}

int64_t os64_clip_paste(void *buf, uint64_t cap)
{
	int64_t h = os64_open(OS64_CLIPBOARD_PATH, "r");
	if (h < 0)
		return h;

	// The handle holds ONE clipboard entry for its whole life, so this length
	// and the bytes read below can never disagree — even if somebody copies
	// something else in between. (seek returns the NEW position, so seeking to
	// the end IS asking the size. os64's seek, not Unix's.)
	int64_t length = os64_seek((int32_t)h, 0, OS64_SEEK_END);
	if (length < 0)
	{
		os64_close((int32_t)h);
		return length;
	}

	uint64_t want = (uint64_t)length;
	if (want > cap)
		want = cap;

	if (want > 0 && buf != NULL)
	{
		if (os64_seek((int32_t)h, 0, OS64_SEEK_SET) < 0)
		{
			os64_close((int32_t)h);
			return -1;
		}

		uint64_t got = 0;
		while (got < want)
		{
			// Reads come back SHORT, always — the loop is the contract.
			int64_t n = os64_read((int32_t)h, (uint8_t *)buf + got,
			                      (size_t)(want - got));
			if (n <= 0)
				break;   // 0 is end of the entry; short is not an error
			got += (uint64_t)n;
		}
	}

	os64_close((int32_t)h);
	return length;   // the TRUE length, so a truncated paste knows it was one
}

int64_t os64_clip_length(void)
{
	// stat, not open: the clipboard is the one /sys node whose size is real,
	// precisely so this question costs nothing.
	os64_dirent_t entry;

	if (os64_stat(OS64_CLIPBOARD_PATH, &entry) < 0)
		return -1;
	return (int64_t)entry.size;
}
