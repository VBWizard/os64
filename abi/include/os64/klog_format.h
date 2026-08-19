#ifndef OS64_ABI_KLOG_FORMAT_H
#define OS64_ABI_KLOG_FORMAT_H

// os64/klog_format.h — how one log entry becomes one line of text.
//
// WHY THIS EXISTS. The layout "%lu (0x%04lx) AP%u: %s" used to be spelled in
// FOUR places — logd (the file), the kernel drainer (serial), and printd's two
// direct-to-serial paths. Four copies of one decision, across the ring 0/3
// boundary, which is a drift waiting for its first edit. This is the single
// copy. (Same instinct as the exception-unification arc: one prologue, one
// reporter.)
//
// MECHANISM, NOT POLICY. os64/klog.h is emphatic that where entries go is
// userland's business, and that "the format of a timestamp is policy too".
// So this header does no calendar math, reads no config, and prefers no
// layout: the CALLER hands in a format string and an already-broken-down
// wall clock, and this renders. The kernel gets its format from the cmdline
// (LOGFMT=, the only config channel that exists before a filesystem does);
// logd gets its from /etc/logd.conf. One renderer, two policies.
//
// WHY A FORMAT STRING rather than a table of named layouts: because the named
// layouts ARE format strings (see the OS64_LOGFMT_* defaults below), a config
// that wants to invent its own doesn't need a new mechanism — it stops naming
// a string and starts spelling one. The named formats stay as the friendly
// front end and the thing a bad string falls back to.

#include <stdint.h>
#include <stddef.h>

// The escapes. Kept short and %-shaped (rather than {braced}) so the renderer
// stays a few dozen lines: this code runs on the path that reports everything
// else going wrong, and it should be too simple to break.
//
//   %d   date, YYYY-MM-DD          %k   ticks since boot
//   %t   time, HH:MM:SS            %c   core (the AP number, bare)
//   %T   thread id, 4 hex digits   %g   category tag (the DEBUG_* flag)
//   %l   level/priority digit      %m   the message itself
//   %%   a literal '%'
//
// %m is not required to be last, but everything after it on a long line is
// still emitted — the caller's buffer is the only limit.

// The formats that ship. `classic` is byte-for-byte what os64 has always
// printed, kept because it is genuinely the right one for hardcore debugging
// and because a format change should never be able to cost you the old view.
#define OS64_LOGFMT_CLASSIC "%k (0x%T) AP%c: %m"
// The day-to-day one: a wall clock (which ticks alone can never give you),
// ticks and core demoted into one parenthesis, no thread id.
#define OS64_LOGFMT_DAILY   "%t (%k,%c): %m"
// Everything the entry carries, for when you do not yet know what matters.
#define OS64_LOGFMT_FULL    "%d %t (%k,%c,%T) %g: %m"

// The named formats, resolved. ONE vocabulary for the whole system: the
// kernel's LOGFMT= and logd's /etc/logd.conf both spell "daily" and both mean
// this string, because two tables of the same names is how "daily" comes to
// mean two different layouts. Returns NULL for an unknown name — callers keep
// what they had and say so.
static inline const char *os64_logfmt_by_name(const char *name)
{
	if (name == NULL)
		return (const char *)0;
	// Hand-rolled compare: this header is included by freestanding kernel code
	// that has no libc, and by userland that has its own. Three names do not
	// justify making either side agree on a strcmp.
	const char *const names[] = { "classic", "daily", "full" };
	const char *const fmts[]  = { OS64_LOGFMT_CLASSIC, OS64_LOGFMT_DAILY, OS64_LOGFMT_FULL };
	for (unsigned i = 0; i < 3; i++) {
		const char *a = name, *b = names[i];
		while (*a && *a == *b) { a++; b++; }
		if (*a == '\0' && *b == '\0')
			return fmts[i];
	}
	return (const char *)0;
}

// ── category names (%g) ─────────────────────────────────────────────────────
// printd stores the INDEX of the lowest DEBUG_* bit a line was logged under.
// The numbers are the kernel's (CONFIG.h), but logd has to render them too and
// cannot include a kernel header — so the vocabulary lives here, in the
// contract, and the kernel STATIC-ASSERTS against it (see log.c). Renumber a
// flag without updating this table and the build stops.
//
// Tags are short and upper-case so they line up in a column and survive being
// grepped for. Index 0 is a known wart, documented rather than papered over:
// printd stores 0 both for DEBUG_EXCEPTIONS *and* for a line logged with no
// category bit at all, so "EXC" is a guess in the second case. The cure is to
// store index+1 and reserve 0 for "none"; it is a one-line change nobody has
// been asked to approve yet.
enum {
	OS64_LOGCAT_EXCEPTIONS = 0, OS64_LOGCAT_BOOT = 1, OS64_LOGCAT_SMP = 2,
	OS64_LOGCAT_PCI_DISCOVERY = 3, OS64_LOGCAT_PCI = 4, OS64_LOGCAT_HARDDRIVE = 5,
	OS64_LOGCAT_AHCI = 6, OS64_LOGCAT_MEMMAP = 7, OS64_LOGCAT_ACPI = 8,
	OS64_LOGCAT_PAGING = 9, OS64_LOGCAT_ALLOCATOR = 10, OS64_LOGCAT_DEMAND_PAGING = 11,
	OS64_LOGCAT_NVME = 12, OS64_LOGCAT_VFS = 13, OS64_LOGCAT_THREAD = 14,
	OS64_LOGCAT_TASK = 15, OS64_LOGCAT_SCHEDULER = 16, OS64_LOGCAT_SIGNALS = 17,
	OS64_LOGCAT_LOGGING = 18, OS64_LOGCAT_TESTS = 19, OS64_LOGCAT_SYSCALL = 20,
	OS64_LOGCAT_GUI = 21, OS64_LOGCAT_APPLICATION = 22, OS64_LOGCAT_TASKSWITCH = 23,
	OS64_LOGCAT_PIPE = 24, OS64_LOGCAT_SHUTDOWN = 25, OS64_LOGCAT_USB = 26,
	OS64_LOGCAT_DIAG = 27, OS64_LOGCAT_NET = 28, OS64_LOGCAT_SYSTEM = 29, 
    OS64_LOGCAT_SPECIAL = 125,
};

// Returns NULL for an index with no name, which makes %g fall back to the
// number — an unnamed category should look like a gap in this table, not like
// a category that doesn't exist.
static inline const char *os64_logcat_name(uint8_t category)
{
	switch (category) {
	case OS64_LOGCAT_EXCEPTIONS:    return "EXC";
	case OS64_LOGCAT_BOOT:          return "BOOT";
	case OS64_LOGCAT_SMP:           return "SMP";
	case OS64_LOGCAT_PCI_DISCOVERY: return "PCID";
	case OS64_LOGCAT_PCI:           return "PCI";
	case OS64_LOGCAT_HARDDRIVE:     return "DISK";
	case OS64_LOGCAT_AHCI:          return "AHCI";
	case OS64_LOGCAT_MEMMAP:        return "MMAP";
	case OS64_LOGCAT_ACPI:          return "ACPI";
	case OS64_LOGCAT_PAGING:        return "PAGE";
	case OS64_LOGCAT_ALLOCATOR:     return "ALLOC";
	case OS64_LOGCAT_DEMAND_PAGING: return "DPAGE";
	case OS64_LOGCAT_NVME:          return "NVME";
	case OS64_LOGCAT_VFS:           return "VFS";
	case OS64_LOGCAT_THREAD:        return "THRD";
	case OS64_LOGCAT_TASK:          return "TASK";
	case OS64_LOGCAT_SCHEDULER:     return "SCHED";
	case OS64_LOGCAT_SIGNALS:       return "SIG";
	case OS64_LOGCAT_LOGGING:       return "LOG";
	case OS64_LOGCAT_TESTS:         return "TEST";
	case OS64_LOGCAT_SYSCALL:       return "SYS";
	case OS64_LOGCAT_GUI:           return "GUI";
	case OS64_LOGCAT_APPLICATION:   return "APP";
	case OS64_LOGCAT_TASKSWITCH:    return "SWTCH";
	case OS64_LOGCAT_PIPE:          return "PIPE";
	case OS64_LOGCAT_SHUTDOWN:      return "SHUT";
	case OS64_LOGCAT_USB:           return "USB";
	case OS64_LOGCAT_DIAG:          return "DIAG";
	case OS64_LOGCAT_NET:           return "NET";
	case OS64_LOGCAT_SPECIAL:       return "SPEC";
    case OS64_LOGCAT_SYSTEM:        return "SYSTEM";
    default:
        return (const char *)0;
    }
}

// Does this format reference a wall clock (%d or %t)? Callers ask ONCE, when
// the format is chosen, so the per-line path never pays for an epoch→calendar
// breakdown that `classic` would not print. (Both sinks need this answer, and
// two copies of it would be one copy too many.)
static inline int os64_logfmt_uses_clock(const char *fmt)
{
	if (fmt == (const char *)0)
		return 0;
	for (const char *p = fmt; *p; p++) {
		if (*p != '%')
			continue;
		p++;
		if (*p == 'd' || *p == 't')
			return 1;
		if (*p == '\0')
			break;
	}
	return 0;
}

// Wall clock, already broken down by the caller. The ABI deliberately does no
// epoch arithmetic (kernel and userland each have their own, and neither
// belongs in a contract header). Callers should compute this ONCE per second
// and reuse it across a batch rather than per entry — it changes at 1Hz and a
// drain pass can carry thousands of lines.
typedef struct os64_logtime
{
	uint16_t year;
	uint8_t  mon;   // 1-12
	uint8_t  day;   // 1-31
	uint8_t  hour;  // 0-23
	uint8_t  min;   // 0-59
	uint8_t  sec;   // 0-59
	uint8_t  valid; // 0 = no clock yet; %d/%t render as "-" rather than lying
} os64_logtime_t;

// The fields of one entry, flattened. Takes the VALUES rather than a struct
// pointer so the kernel (which has log_entry_t) and userland (which has
// os64_logent_t) can both call it without either learning the other's layout —
// the same reason those two structs are deliberately separate in klog.h.
typedef struct os64_logline
{
	uint64_t ticks;
	uint64_t threadID;
	uint16_t core;
	uint8_t  level;
	uint8_t  category;
	const char *message;
	const char *category_name;   // NULL renders the number instead
} os64_logline_t;

// ── the renderer ────────────────────────────────────────────────────────────
// Writes at most `outlen` bytes INCLUDING the NUL, and always NUL-terminates
// (given outlen > 0). Returns the number of bytes written, not counting the
// NUL — so a caller batching lines into one buffer just advances by it.
//
// Truncation is silent BY DESIGN at this layer: a log line that does not fit
// is still a log line, and the alternative (refusing to render) would lose the
// message entirely to save its prefix. Callers who care size the buffer.

static inline size_t os64_logfmt_putc(char *out, size_t outlen, size_t at, char c)
{
	if (at + 1 < outlen)
		out[at] = c;
	return at + 1;
}

static inline size_t os64_logfmt_puts(char *out, size_t outlen, size_t at, const char *s)
{
	if (s == NULL)
		return at;
	while (*s)
		at = os64_logfmt_putc(out, outlen, at, *s++);
	return at;
}

// Unsigned decimal, no padding.
static inline size_t os64_logfmt_putu(char *out, size_t outlen, size_t at, uint64_t v)
{
	char tmp[24];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n > 0)
		at = os64_logfmt_putc(out, outlen, at, tmp[--n]);
	return at;
}

// Zero-padded unsigned decimal, for clock fields.
static inline size_t os64_logfmt_putu2(char *out, size_t outlen, size_t at,
                                       uint64_t v, int width)
{
	char tmp[24];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n < width)
		tmp[n++] = '0';
	while (n > 0)
		at = os64_logfmt_putc(out, outlen, at, tmp[--n]);
	return at;
}

// Zero-padded lowercase hex — %T's 4 digits are what `classic` has always
// printed, and widening it silently would break every eye trained on it.
static inline size_t os64_logfmt_putx(char *out, size_t outlen, size_t at,
                                      uint64_t v, int width)
{
	static const char kHex[] = "0123456789abcdef";
	char tmp[24];
	int n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = kHex[v & 0xF];
		v >>= 4;
	}
	while (n < width)
		tmp[n++] = '0';
	while (n > 0)
		at = os64_logfmt_putc(out, outlen, at, tmp[--n]);
	return at;
}

static inline size_t os64_logfmt_render(char *out, size_t outlen,
                                        const char *fmt,
                                        const os64_logline_t *e,
                                        const os64_logtime_t *now)
{
	size_t at = 0;
	if (out == NULL || outlen == 0)
		return 0;
	if (fmt == NULL)
		fmt = OS64_LOGFMT_CLASSIC;

	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			at = os64_logfmt_putc(out, outlen, at, *p);
			continue;
		}
		switch (*++p) {
		case 'd':
			if (now && now->valid) {
				at = os64_logfmt_putu2(out, outlen, at, now->year, 4);
				at = os64_logfmt_putc(out, outlen, at, '-');
				at = os64_logfmt_putu2(out, outlen, at, now->mon, 2);
				at = os64_logfmt_putc(out, outlen, at, '-');
				at = os64_logfmt_putu2(out, outlen, at, now->day, 2);
			} else {
				// No clock yet (the earliest boot lines predate it). Say so
				// with a placeholder of the same width rather than printing a
				// confident 1970 — a wrong timestamp is worse than none.
				at = os64_logfmt_puts(out, outlen, at, "----------");
			}
			break;
		case 't':
			if (now && now->valid) {
				at = os64_logfmt_putu2(out, outlen, at, now->hour, 2);
				at = os64_logfmt_putc(out, outlen, at, ':');
				at = os64_logfmt_putu2(out, outlen, at, now->min, 2);
				at = os64_logfmt_putc(out, outlen, at, ':');
				at = os64_logfmt_putu2(out, outlen, at, now->sec, 2);
			} else {
				at = os64_logfmt_puts(out, outlen, at, "--:--:--");
			}
			break;
		case 'k': at = os64_logfmt_putu(out, outlen, at, e->ticks); break;
		case 'c': at = os64_logfmt_putu(out, outlen, at, e->core); break;
		case 'T': at = os64_logfmt_putx(out, outlen, at, e->threadID, 4); break;
		case 'l': at = os64_logfmt_putu(out, outlen, at, e->level); break;
		case 'g':
			if (e->category_name)
				at = os64_logfmt_puts(out, outlen, at, e->category_name);
			else
				at = os64_logfmt_putu(out, outlen, at, e->category);
			break;
		case 'm': at = os64_logfmt_puts(out, outlen, at, e->message); break;
		case '%': at = os64_logfmt_putc(out, outlen, at, '%'); break;
		case '\0':
			// A trailing '%' — malformed, but the line still matters more
			// than the pedantry. Emit it literally and stop.
			at = os64_logfmt_putc(out, outlen, at, '%');
			p--;   // let the loop's ++ find the NUL
			break;
		default:
			// Unknown escape: emit it verbatim so a typo in a config file is
			// VISIBLE in the log rather than silently swallowing characters.
			// (os64_logfmt_valid() is what refuses one before it ever gets
			// here — this is the belt to that suspenders.)
			at = os64_logfmt_putc(out, outlen, at, '%');
			at = os64_logfmt_putc(out, outlen, at, *p);
			break;
		}
	}

	out[at < outlen ? at : outlen - 1] = '\0';
	return at < outlen ? at : outlen - 1;
}

// Validate a format before adopting it. A config file is written by a human
// at 2am; a format string that renders garbage would corrupt the one tool
// they are using to find out what went wrong. Callers check FIRST and keep
// their previous format on a refusal, saying so loudly.
// Returns 1 if every escape is known, 0 otherwise.
static inline int os64_logfmt_valid(const char *fmt)
{
	if (fmt == NULL)
		return 0;
	int saw_message = 0;
	for (const char *p = fmt; *p; p++) {
		if (*p != '%')
			continue;
		switch (*++p) {
		case 'm': saw_message = 1; break;
		case 'd': case 't': case 'k': case 'c':
		case 'T': case 'l': case 'g': case '%': break;
		default: return 0;   // unknown escape, or a trailing '%'
		}
	}
	// A format with no %m renders every line as decoration and drops the
	// thing being logged. That is never what anyone meant.
	return saw_message;
}

#endif // OS64_ABI_KLOG_FORMAT_H
