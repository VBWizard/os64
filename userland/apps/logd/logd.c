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
// WHERE THE LOG BELONGS: /home — the persistent partition (the 2026-08-07
// persistence doctrine: root is the system's to overwrite, /home is the
// user's and survives builds and refreshes; a log that outlives the boot
// it describes belongs on the surviving side). Root became writable the
// same day, so the DEFAULT below is only the fallback for boots without
// a /home; the boot entries all pass LOGD=/home/os64.log explicitly.
//
// KNOWN GAP: there is no mkdir syscall yet, so a path in a directory that
// does not already exist will fail to open. Hence a file at the root of
// the mount by default rather than /fat/log/os64.log.

#include "os64/os64.h"
#include "os64/klog_format.h"   // the ONE renderer, shared with the kernel

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

// The FILE's line format. Deliberately independent of the kernel's serial
// format (log.h's kLogFormat): serial is where you debug and wants every raw
// field; the file is what you read afterwards and can afford a wall clock.
// This is what /etc/logd.conf will set — until that lands it is the layout
// os64 has always written, so nothing about the file changes today.
static const char *gFormat = OS64_LOGFMT_CLASSIC;

// Broken-down wall clock for %d/%t, refreshed once per drain rather than once
// per line (it moves at 1Hz; a batch can carry hundreds of lines). Zeroed
// means .valid == 0, which renders placeholders instead of a confident 1970 —
// correct until the clock is actually wired in the next slice.
static os64_logtime_t gClock;
static bool gClockWanted = false;      // does gFormat actually ask for %d/%t?
static char gFormatBuf[160];           // holds a LITERAL format read from a file

// ── logd.conf — os64's first configuration file ─────────────────────────────
//
// THE SYNTAX IS DELIBERATELY BORING, because it is a precedent: the next
// daemon that needs a config will copy whatever this one does. So:
//
//     # comments run to end of line
//     format = daily                 # a name...
//     format = %t (%k,%c): %m        # ...or a layout spelled out
//
// key, '=', value; whitespace anywhere; '#' starts a comment; unknown keys are
// warned about and skipped rather than fatal (a config file from a NEWER os64
// should not stop an older one from starting). No sections, no quoting, no
// escapes, no line continuations — none of which anything has asked for, and
// every one of which is easier to add later than to remove.
//
// THE SEARCH IS A PERSISTENCE GRADIENT, first hit wins — the same ladder husk
// climbs for husk.rc, and deliberately the same order, because two config
// files that disagree about which location outranks which is a trap nobody
// deserves at 2am:
//
//   /home/logd.conf   YOURS. Its own partition, survives builds and root
//                     refreshes untouched (the persistence doctrine).
//   /etc/logd.conf    the SYSTEM's, shipped by the build on the ext2 root —
//                     which means `make` rewrites it, so edits here are the
//                     build's to keep, not yours.
//   LOGFMT=<name>     inherited from the kernel cmdline (argv[2]) when no
//                     config file exists at all, so one flag can set both
//                     sinks when that is all somebody wanted.
//   classic           the floor: the layout os64 has always printed.
// Sized for a file a HUMAN wrote, which in this house means one that explains
// itself: the shipped /etc/logd.conf is 3KB of comments around a single
// setting. The first version of this reader used 1KB and read nothing but the
// legend — the config was too well documented to work. Overrunning it now
// says so rather than silently ignoring the tail, because "my setting is at
// the bottom of the file and nothing happens" is a miserable afternoon.
#define LOGD_CONF_MAX 8192

// How long to wait between knocks while the log file is unwritable. Slower
// than the idle poll on purpose: nothing is being consumed during a back-off,
// so there is no backlog pressure — only a door to keep trying.
#define BACKOFF_SLEEP_MS 1000

// True while the file cannot be written and we have deliberately stopped
// reading the kernel rings. See the back-off comment in the drain loop.
static bool gBackingOff = false;

static const char *const kConfPaths[] = { "/home/logd.conf", "/etc/logd.conf" };

// Complaints about the config, held until the log file is open.
//
// They used to go only to os64_printf — the console — which is precisely
// where they are useless: a config problem is discovered by someone READING
// THE LOG and wondering why their setting did nothing, and the console line
// explaining it scrolled past during boot. (Chris, 2026-08-18, wrote a file
// containing just the format string with no `format =` key. logd said so, to
// a screen nobody was watching, then quietly used /etc's default — and the
// log looked plausible enough to be confusing.) So: say it in BOTH places,
// and in the log say it at the top where the boot begins.
static char gComplaints[768];
static size_t gComplaintsUsed = 0;

static void logd_complain(const char *a, const char *b, const char *c)
{
	os64_printf("logd: %s%s%s\n", a, b ? b : "", c ? c : "");
	int32_t n = os64_snprintf(gComplaints + gComplaintsUsed,
	                          sizeof(gComplaints) - gComplaintsUsed,
	                          "logd: %s%s%s\n", a, b ? b : "", c ? c : "");
	if (n > 0) {
		size_t room = sizeof(gComplaints) - gComplaintsUsed;
		gComplaintsUsed += ((size_t)n < room) ? (size_t)n : (room ? room - 1 : 0);
	}
}

// Pull the value of `key` out of a config file. Returns true only if the file
// existed AND carried that key.
static bool logd_conf_value(const char *path, const char *key,
                            char *out, size_t outlen)
{
	int64_t fd = os64_open(path, "r");
	if (fd < 0)
		return false;

	// Read to EOF, not to "whatever one call returned": a short read is legal
	// and a config file's meaning can live in its last line.
	static char buf[LOGD_CONF_MAX];   // static: 8KB would blow the user stack
	size_t got = 0;
	for (;;) {
		int64_t n = os64_read((int32_t)fd, buf + got, sizeof(buf) - 1 - got);
		if (n <= 0)
			break;
		got += (size_t)n;
		if (got >= sizeof(buf) - 1) {
			logd_complain(path, " is larger than the config reader's buffer — "
			              "the tail was not read", NULL);
			break;
		}
	}
	os64_close((int32_t)fd);
	if (got == 0)
		return false;
	buf[got] = '\0';

	bool found = false;
	size_t i = 0;
	while (buf[i] != '\0') {
		// One line at a time: find its bounds, then chop the comment off.
		size_t start = i;
		while (buf[i] != '\0' && buf[i] != '\n')
			i++;
		size_t end = i;
		if (buf[i] == '\n')
			i++;
		for (size_t c = start; c < end; c++)
			if (buf[c] == '#') { end = c; break; }

		// key
		size_t k = start;
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t ks = k;
		while (k < end && buf[k] != ' ' && buf[k] != '\t' && buf[k] != '=')
			k++;
		size_t ke = k;
		if (ks == ke)
			continue;   // blank line, or a line that was only a comment

		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		if (k >= end || buf[k] != '=') {
			buf[ke] = '\0';
			// Name the shape we wanted: the commonest mistake is writing the
			// VALUE alone (a bare format string) and omitting the key.
			logd_complain(path, ": expected 'key = value' — ignored: ", &buf[ks]);
			continue;
		}
		k++;   // past '='

		// value: everything to end-of-line, trimmed both sides. NOT split on
		// spaces — a format string is full of them, and that is the whole
		// reason literal layouts live in a file instead of on the cmdline.
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t vs = k, ve = end;
		while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t' || buf[ve - 1] == '\r'))
			ve--;

		// key match?
		size_t klen = ke - ks;
		const char *kp = key;
		size_t n = 0;
		while (n < klen && kp[n] != '\0' && kp[n] == buf[ks + n])
			n++;
		if (n == klen && kp[n] == '\0') {
			size_t vlen = ve - vs;
			if (vlen >= outlen)
				vlen = outlen - 1;
			for (size_t c = 0; c < vlen; c++)
				out[c] = buf[vs + c];
			out[vlen] = '\0';
			found = true;   // last one wins, so a file can override itself
		} else {
			buf[ke] = '\0';
			logd_complain(path, ": unknown setting, ignored: ", &buf[ks]);
		}
	}
	return found;
}

// Work out the wall-clock second a line was logged in, from its ticks and a
// (now_epoch, now_ticks) reference taken once per batch. Fills `out`.
//
// Cached on the epoch second, because consecutive lines overwhelmingly share
// one: at 100 ticks to the second, a burst of sixty lines is usually a single
// timestamp, and the calendar conversion is the only real arithmetic here.
static void logd_clock_for(os64_logtime_t *out, uint64_t entryTicks,
                           int64_t nowSec, const os64_ticks_t *nowTicks)
{
	static int64_t cachedSec = -1;
	static os64_logtime_t cached;

	uint32_t per = nowTicks->per_second ? nowTicks->per_second : 100;
	// Ticks only ever move forward, but guard the subtraction anyway: a line
	// logged between our os64_ticks() call and this one would go negative and
	// wrap into a timestamp from the far future.
	uint64_t behind = (nowTicks->ticks > entryTicks) ? (nowTicks->ticks - entryTicks) : 0;
	int64_t when = (int64_t)nowSec - (int64_t)(behind / per);

	if (when == cachedSec) {
		*out = cached;
		return;
	}

	os64_date_t d;
	if (os64_localtime(when, &d) < 0) {
		out->valid = 0;
		return;
	}
	cached.year  = (uint16_t)d.year;
	cached.mon   = (uint8_t)d.month;
	cached.day   = (uint8_t)d.day;
	cached.hour  = (uint8_t)d.hour;
	cached.min   = (uint8_t)d.minute;
	cached.sec   = (uint8_t)d.second;
	cached.valid = 1;
	cachedSec = when;
	*out = cached;
}

// Walk the gradient and adopt a format. Silent on success — the boot log is
// not improved by a daemon announcing its own configuration — and loud on
// every way of getting it wrong, because a format is the instrument you read
// all the other failures through.
static void logd_choose_format(int argc, char **argv)
{
	char value[sizeof(gFormatBuf)];

	for (unsigned i = 0; i < sizeof(kConfPaths) / sizeof(kConfPaths[0]); i++) {
		if (!logd_conf_value(kConfPaths[i], "format", value, sizeof(value)))
			continue;

		const char *named = os64_logfmt_by_name(value);
		if (named != NULL) {
			gFormat = named;
			return;
		}
		if (os64_logfmt_valid(value)) {
			// A literal layout. Copy it: `value` is about to go out of scope,
			// and gFormat outlives this function by the whole uptime.
			size_t c = 0;
			while (value[c] != '\0' && c < sizeof(gFormatBuf) - 1) {
				gFormatBuf[c] = value[c];
				c++;
			}
			gFormatBuf[c] = '\0';
			gFormat = gFormatBuf;
			return;
		}
		logd_complain(kConfPaths[i],
		              ": format is neither a known name (classic, daily, full) "
		              "nor a valid layout (needs %m) — using classic: ", value);
		return;   // a file that names a format WRONGLY is not a file to fall
		          // through: the operator meant something, and quietly using
		          // an inherited format would hide the typo.
	}

	// No config file anywhere: inherit the kernel's LOGFMT=, if it passed one.
	if (argc > 2 && argv[2] != NULL && argv[2][0] != '\0') {
		const char *named = os64_logfmt_by_name(argv[2]);
		if (named != NULL)
			gFormat = named;
	}
}

// Idle sleep between empty polls, milliseconds. Log lines are not
// latency-critical (the kernel's own daemon batched at 2/second), and the
// rings hold ~56,000 entries per core, so a tenth of a second of slack
// costs nothing and keeps this process at approximately zero CPU.
#define IDLE_SLEEP_MS 100

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : DEFAULT_PATH;

	// Before the first line is written: settle which layout this file gets.
	logd_choose_format(argc, argv);
	gClockWanted = os64_logfmt_uses_clock(gFormat) != 0;

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

	// NO ATTACH BANNER on a first attach (retired 2026-08-18, Chris's call).
	// It existed to separate boots in a file that spans many of them — but
	// the kernel's own first line does that better and for free: every boot
	// opens with "***** OS64 - system booting at <date> *****", which is
	// distinctive, searchable ("booting"), and carries the date the banner
	// never did. A second marker underneath it was ceremony.
	//
	// A RE-attach is different and still announced — but by the KERNEL, as an
	// ordinary log entry, because only the kernel can tell "the first daemon
	// of this boot" from "a daemon that replaced one that died" (a restarted
	// logd is a fresh process with no memory of the last one). That notice is
	// the counterpart to "userland log sink went quiet", and the pair reads
	// as a story: died here, came back there.
	os64_ticks_t t;
	os64_ticks(&t);
	// Anything the config had to say still goes into the file, at the top,
	// where the person wondering "why did my format do nothing?" is looking.
	// The console copy already happened and already scrolled away.
	if (gComplaintsUsed > 0)
	{
		os64_write((int32_t)fd, gComplaints, gComplaintsUsed);
		os64_sync((int32_t)fd);
	}
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
	// Formatted-but-unwritten bytes. Declared OUT here, not per iteration,
	// because a back-off must be able to HOLD a batch it has already taken
	// from the rings: those entries are gone from the kernel's copy, so
	// dropping them would be exactly the silent loss the policy forbids.
	size_t used = 0;
	for (;;)
	{
		// BACKED OFF: the file is unwritable and we have already released the
		// sink by simply not reading. Do NOT call klog_read here — that call
		// IS the claim, and claiming the log while unable to store it is the
		// silent-loss failure this daemon exists to avoid. Just knock on the
		// file until it opens.
		if (gBackingOff)
		{
			os64_sleep(BACKOFF_SLEEP_MS);
			int64_t probe = os64_open(path, "a");
			if (probe < 0)
				continue;
			fd = probe;
			gBackingOff = false;
			// LOUD on the way back in, too — Chris's requirement, and the
			// right one: an outage nobody was told about ending is just as
			// invisible as one nobody was told about starting.
			os64_printf("logd: %s is writable again — resuming, %lu held byte(s) first "
			            "(the kernel hands the log sink back on the next read)\n",
			            path, (unsigned long)used);
			// "Held byte(s) FIRST" is a load-bearing word: the flush must
			// happen BEFORE the next klog_read, because gOut is sized for one
			// batch with headroom, not two — a held batch plus a fresh one can
			// overrun it, and the renderer truncates silently past the end, so
			// the tail of the new batch would be consumed from the rings and
			// never written. Exactly the silent loss this policy forbids
			// (found in review, 2026-08-18: the first version printed the
			// promise and skipped the flush).
			if (used > 0)
			{
				int64_t flushed = os64_write((int32_t)fd, gOut, used);
				if (flushed != (int64_t)used)
				{
					// Same policy as the main write path below: the file
					// OPENED but the disk refused the bytes — that is not a
					// blocker to wait out, it is a failed write, and a failed
					// write ends the daemon loudly.
					os64_printf("logd: write to %s failed (%ld of %lu bytes) — disk full?\n",
					            path, (long)flushed, (unsigned long)used);
					os64_printf("logd: releasing the log sink; the kernel resumes serial in a moment\n");
					os64_sync((int32_t)fd);
					os64_close((int32_t)fd);
					return 1;
				}
				used = 0;
			}
			os64_ticks(&t);
			lastSync = t.ticks;
		}

		int64_t got = os64_klog_read(batch, BATCH);
		if (got == OS64_KLOG_RETIRED)
		{
			// The system is going down and the rings are empty — this
			// daemon has been handed every byte it will ever get (the
			// kernel only speaks RETIRED on an empty poll, so nothing is
			// left behind). Commit the file, close it, go home. The
			// kernel's shutdown descent is watching the claim (released
			// before we even saw this) and waits a grace period for this
			// close to land before it syncs and flushes the disks.
			os64_printf("logd: retired for shutdown — log committed\n");
			if (fd >= 0)
			{
				os64_sync((int32_t)fd);
				os64_close((int32_t)fd);
			}
			return 0;
		}
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

		// THE CLOCK IS PER LINE, DERIVED FROM ITS TICKS — not the time this
		// batch happened to be drained.
		//
		// The distinction is not pedantry: when logd attaches it replays the
		// ENTIRE boot backlog, so stamping lines with drain-time made every
		// line of a 7-second boot claim the same instant, seven seconds after
		// it happened. The banner said 20:56:23 while every line around it
		// said 20:56:30, which is the log disagreeing with itself.
		//
		// Nothing new is needed to fix it: an entry carries its ticks, and
		// os64_ticks gives both the current tick and the tick rate, so
		//     when(entry) = now - (ticks_now - entry.ticks) / per_second
		// recovers each line's real second. os64/klog.h anticipated exactly
		// this — "a daemon that wants wall-clock stamps calls os64_time() and
		// does its own arithmetic", because the format of a timestamp is
		// policy and policy lives out here.
		os64_time_t nowWall = {0};
		os64_ticks_t nowTicks = {0};
		if (gClockWanted)
		{
			os64_time(&nowWall);
			os64_ticks(&nowTicks);
		}

		for (int64_t i = 0; i < got; i++)
		{
			size_t room = sizeof(gOut) - used;
			int32_t n;   // (was declared by the retired attach banner above)

			// Continuation chunks are the tail of a long line that the
			// kernel split across entries — they get NO prefix, or a
			// 300-character message would sprout timestamps mid-sentence.
			if (batch[i].continued) {
				n = os64_snprintf(gOut + used, room, "%s", batch[i].message);
			} else {
				// The shared renderer — the same code the kernel's serial
				// drainer runs, so the two sinks can never drift apart in
				// layout, only in the format each was CONFIGURED with.
				os64_logline_t line = {
					.ticks         = batch[i].ticks,
					.threadID      = batch[i].threadID,
					.core          = batch[i].core,
					.level         = batch[i].level,
					.category      = batch[i].category,
					.message       = batch[i].message,
					.category_name = os64_logcat_name(batch[i].category),
				};
				if (gClockWanted)
					logd_clock_for(&gClock, batch[i].ticks, nowWall.epoch, &nowTicks);
				n = (int32_t)os64_logfmt_render(gOut + used, room, gFormat,
				                                &line, &gClock);
			}

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
			//
			// WITH PATIENCE. FF_FS_LOCK makes opens exclusive both ways, so
			// a reader mid-`wc` on a 50MB log holds the file for a few
			// seconds — and the first version of this reopen treated that
			// as fatal and died with a full batch in hand (found the same
			// evening the policy landed: reading the log could assassinate
			// the log daemon). A LOCKED open is transient; retry on the
			// idle cadence for a bounded window and only then conclude the
			// blocker is a wedge, confess the loss, and release the sink.
			if (fd < 0)
			{
				for (int tries = 0; fd < 0 && tries < 50; tries++)
				{
					fd = os64_open(path, "a");
					if (fd < 0)
						os64_sleep(IDLE_SLEEP_MS);
				}
				if (fd < 0)
				{
					// BACK OFF — do not die (changed 2026-08-18). Exiting was
					// half right: releasing the sink IS correct, because a
					// logd that keeps reading while it cannot write is
					// deleting the log (the paragraph above). But EXITING
					// makes a transient blocker permanent — it needs a human
					// to notice and restart the daemon, and the machine most
					// likely to hit it is the one nobody is sitting at.
					//
					// Instead: stop reading (the heartbeat lapses, the kernel
					// announces the silence and takes over within
					// LOG_SINK_TIMEOUT_TICKS) and keep trying the open on a
					// slow cadence. When the blocker lets go we resume, the
					// kernel hands the sink back, and it says so in the log —
					// the re-attach notice exists for exactly this moment.
					//
					// The batch in hand is NOT lost: `used` bytes stay in gOut
					// and are written the moment the file reopens. Those
					// entries are already consumed from the rings, so dropping
					// them would be the silent loss this whole policy exists
					// to prevent.
					os64_printf("logd: %s has been unwritable for 5s — releasing the log sink "
					            "and retrying (%lu bytes held, kernel takes the log meanwhile)\n",
					            path, (unsigned long)used);
					gBackingOff = true;
					continue;   // top of the loop: no klog_read while backed off
				}
				os64_ticks(&t);
				lastSync = t.ticks;
			}
			int64_t wrote = os64_write((int32_t)fd, gOut, used);
			// (used is cleared below, only once the bytes are on disk)
			if (wrote != (int64_t)used)
			{
				os64_printf("logd: write to %s failed (%ld of %lu bytes) — disk full?\n",
				            path, (long)wrote, (unsigned long)used);
				os64_printf("logd: releasing the log sink; the kernel resumes serial in a moment\n");
				os64_sync((int32_t)fd);
				os64_close((int32_t)fd);
				return 1;
			}
			used = 0;   // safely stored — the buffer is free again
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
