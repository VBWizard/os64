// logd — the permanent log daemon. Ring 3, by design.
//
// The kernel keeps a per-core ring of log entries and knows how to merge
// them in time order. It does NOT know where logs belong — that is policy,
// and policy is a userland problem. So this program asks for entries,
// formats them, and appends them to a file whose path is an argument. The
// kernel's own logd stops writing to serial the moment this one starts
// reading, and takes it back within seconds if this one dies.
//
//     logd /fat/os64.log &
//
// APPEND, never truncate: a log that survives the boot it describes is the
// entire point. If the system freezes, the file is the evidence, and a
// truncate-on-start would destroy it on exactly the boot you needed it.
// (Rebuilding the image replaces the disk, which is the natural reset.)
//
// WHY NOT THE ROOT FILESYSTEM: root is ext2 here, and os64's ext2 is
// read-only by design — so the default lands on /fat, which is where the
// writable filesystem is mounted. Nothing about that is permanent; pass a
// path and it goes there.
//
// KNOWN GAP: there is no mkdir syscall yet, so a path in a directory that
// does not already exist will fail to open. Hence a file at the root of
// the mount by default rather than /fat/log/os64.log.

#include "os64/os64.h"

#define BATCH 64
#define DEFAULT_PATH "/fat/os64.log"

// One formatted line's worth of room. The kernel splits anything longer
// across continuation entries, so this bounds a single entry, not a message.
#define LINE_MAX 512

// The batch is formatted into ONE buffer and written with ONE syscall.
//
// It used to be a write() per LINE, which at DEBUG_SCHEDULER|DEBUG_DETAILED
// meant 64 syscalls per batch, each carrying a kernel bounce-buffer copy and
// a full trip down the FAT write path for a few dozen bytes. That daemon cost
// ~20% of a core; the bytes were never the problem, the boundary crossings
// were.
//
// In BSS, NOT on the stack: 32KB would blow the 16KB user stack outright (see
// the note on OS64_PRINTF_MAX in libos64/fmt.c). One process, one thread, so
// a file-scope buffer needs no protection — if logd ever grows threads, this
// becomes per-thread or it becomes a bug.
static char gOut[BATCH * LINE_MAX];

// Idle sleep between empty polls, milliseconds. Log lines are not
// latency-critical (the kernel's own daemon batched at 2/second), and the
// rings hold ~56,000 entries per core, so a tenth of a second of slack
// costs nothing and keeps this process at approximately zero CPU.
#define IDLE_SLEEP_MS 100

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : DEFAULT_PATH;

	// "a" = append: every write lands at the end, across restarts, so the
	// file accumulates the machine's history rather than the last boot's.
	//
	// HELD OPEN ONLY WHILE BUSY (since 2026-08-07, the day FF_FS_LOCK
	// landed): FatFs's lock control makes a write-open EXCLUSIVE, so a
	// permanently-held log meant `cat os64.log` returned FR_LOCKED for the
	// whole uptime — the log was unreadable precisely while it was most
	// interesting. Now the file opens at the first batch of a busy streak
	// and closes on the first EMPTY poll: under a firehose it stays open
	// (batched economics preserved — see the sync discussion below), and
	// on a quiet system it is closed within one idle poll of the last
	// line, so readers get in. Bonus: rm'ing the log during a quiet
	// moment now SUCCEEDS (nothing holds it) and the next streak simply
	// recreates it — log rotation by accident of honesty.
	int64_t fd = os64_open(path, "a");
	if (fd < 0)
	{
		os64_printf("logd: cannot open %s (is the filesystem writable?)\n", path);
		return 1;
	}

	// A boot banner, so a file spanning many boots can be read: without
	// it, a reader has no way to tell where one machine-lifetime ends and
	// the next begins — the ticks counter simply starts over at zero.
	os64_ticks_t t;
	char line[512];
	os64_ticks(&t);
	int32_t n = os64_snprintf(line, sizeof(line),
	                          "\n===== logd attached (tick %lu, %u ticks/sec) =====\n",
	                          t.ticks, t.per_second);
	os64_write((int32_t)fd, line, (size_t)n);
	os64_sync((int32_t)fd);   // the banner should be visible immediately
	// Banner delivered and the path proven writable — release the file
	// until there is something to say (the open-while-busy policy above).
	os64_close((int32_t)fd);
	fd = -1;

	os64_printf("logd: appending kernel log to %s\n", path);

	// When the file was last committed to disk, in ticks. See the sync
	// discussion at the bottom of the loop.
	uint64_t lastSync = t.ticks;

	os64_logent_t batch[BATCH];
	int attached = 0;   // has klog_read ever succeeded for THIS daemon?
	for (;;)
	{
		int64_t got = os64_klog_read(batch, BATCH);
		if (got < 0 && !attached)
		{
			// Refused before ever succeeding: another daemon holds the
			// log claim (the kernel's sink is exclusive — two readers
			// would each get a random half). A second logd idling here
			// forever would just be a mystery process, so say why and go.
			os64_printf("logd: the kernel log is already claimed by another reader — exiting\n");
			if (fd >= 0)
				os64_close((int32_t)fd);
			return 1;
		}
		if (got <= 0)
		{
			// Nothing waiting (or a refusal AFTER we attached — transient
			// kernel unhappiness; sleeping is still the right move, and
			// the kernel resumes serial by itself if we stay quiet).
			//
			// Going idle: commit and RELEASE the file (open-while-busy —
			// see the fd comment at the top). The close carries the
			// directory-entry commit with it, so the sync clock below
			// only matters within a busy streak.
			if (fd >= 0)
			{
				os64_sync((int32_t)fd);
				os64_close((int32_t)fd);
				fd = -1;
			}
			os64_sleep(IDLE_SLEEP_MS);
			continue;
		}
		attached = 1;

		size_t used = 0;
		for (int64_t i = 0; i < got; i++)
		{
			size_t room = sizeof(gOut) - used;

			// Continuation chunks are the tail of a long line that the
			// kernel split across entries — they get NO prefix, or a
			// 300-character message would sprout timestamps mid-sentence.
			if (batch[i].continued)
				n = os64_snprintf(gOut + used, room, "%s", batch[i].message);
			else
				n = os64_snprintf(gOut + used, room, "%lu (0x%04lx) AP%u: %s",
				                  batch[i].ticks, batch[i].threadID,
				                  (uint32_t)batch[i].core, batch[i].message);

			if (n <= 0)
				continue;
			// snprintf returns the length it WANTED (C99), so a long entry
			// reports more than it stored. Believing it would walk `used`
			// past the end of the buffer and hand write() a length covering
			// memory we never filled.
			used += ((size_t)n < room) ? (size_t)n : (room > 0 ? room - 1 : 0);
		}

		// A FAILED WRITE MUST END THIS DAEMON. It must not be swallowed.
		//
		// The kernel decides whether to drain to serial by watching whether
		// anyone is READING the rings (klog_read stamps a heartbeat). So a
		// logd that keeps reading while its writes fail is the worst possible
		// citizen: it consumes entries, drops them on the floor, and keeps the
		// kernel convinced the log is in good hands. Silent loss — the one
		// thing "never drop a byte" actually forbids.
		//
		// Chris proved it inside five minutes on 2026-08-03 by turning on
		// DETAILED *and* EXTRA_DETAILED and filling a 64MB /home to the byte
		// (66,056,704 of them).
		//
		// So: say what happened on the console, and LEAVE. Stopping the reads
		// lets the heartbeat go stale, and within LOG_SINK_TIMEOUT_TICKS the
		// kernel announces the silence and takes serial back. The log survives
		// on the wire instead of vanishing into a full disk.
		if (used > 0)
		{
			// First batch of a busy streak: take the file back (it was
			// released at the last idle poll — open-while-busy policy).
			// A failed OPEN gets the same treatment as a failed write:
			// leave loudly so the kernel reclaims serial, never consume
			// entries we can't land.
			if (fd < 0)
			{
				fd = os64_open(path, "a");
				if (fd < 0)
				{
					os64_printf("logd: cannot reopen %s — releasing the log sink\n", path);
					return 1;
				}
				os64_ticks(&t);
				lastSync = t.ticks;
			}
			int64_t wrote = os64_write((int32_t)fd, gOut, used);
			if (wrote != (int64_t)used)
			{
				os64_printf("logd: write to %s failed (%ld of %lu bytes) — disk full?\n",
				            path, (long)wrote, (unsigned long)used);
				os64_printf("logd: releasing the log sink; the kernel resumes serial in a moment\n");
				os64_sync((int32_t)fd);
				os64_close((int32_t)fd);
				return 1;
			}
		}

		// Commit — but on a CLOCK, not on every batch.
		//
		// Some sync is mandatory: without it the file's LENGTH lives only in
		// the filesystem's memory until the daemon exits, so `cat` on a live
		// log prints nothing at all — the log looks broken while working
		// perfectly (Chris found exactly this, five cats in a row,
		// 2026-08-01). But each sync rewrites the directory entry, and under
		// a DETAILED firehose the batches arrive fast enough that syncing
		// every one turns metadata into the dominant cost.
		//
		// Once a second is the compromise: a reader is never more than a
		// second behind, and the directory-entry writes are bounded by TIME
		// instead of by traffic — the busier the log, the more the cost
		// amortizes, which is exactly backwards from where it was.
		os64_ticks(&t);
		if (fd >= 0 && t.ticks - lastSync >= t.per_second)
		{
			os64_sync((int32_t)fd);
			lastSync = t.ticks;
		}
	}

	// Not reached: a log daemon has no natural end. It stops when the
	// system does, or when something kills it — and if something does,
	// the kernel notices the silence and puts logging back on serial,
	// which means the death notice itself is never lost.
}
