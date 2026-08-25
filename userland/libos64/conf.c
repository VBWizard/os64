// conf.c — the `key = value` reader. Contract and dialect in os64/conf.h.
//
// Lifted from logd.c's logd_conf_value on 2026-08-22, the day os64get became
// the second program with a config file. The shape is the same: read the
// whole file to EOF (a short read is legal and a file's meaning can live in
// its last line), then walk it a line at a time — chop the comment, find the
// key, insist on the '=', trim the value. What changed is that every line is
// HANDED OUT rather than matched against one wanted key, so a caller whose
// file is a list of rules (os64get) gets them in order, and a caller who
// wants one setting (logd) picks it out in the callback.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "os64/conf.h"
#include "os64/io.h"
#include "os64/mem.h"   // os64_malloc — the buffer belongs to the CALL, not the program
#include "os64/str.h"   // os64_streq_nocase (settings keys), os64_strcopy
#include "os64/fmt.h"   // os64_snprintf — the per-saver temp name
#include "os64/proc.h"  // os64_taskid — half of what makes that temp name unique
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"   // SYSCALL_CONF_RESOLVE — the kernel walks the ladder

int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *user)
{
	int64_t fd = os64_open(path, "r");
	if (fd < 0)
		return OS64_CONF_NO_FILE;

	// ONE BUFFER PER CALL, from the heap. It was `static` when this code was
	// logd's and logd read one file once — 8KB really would blow a user stack
	// (logd learned that first), and a static looked like the only other
	// door. It isn't, and a static here is wrong twice over now that the
	// reader is a library: two THREADS reading configs at the same time
	// (os64/thread.h — ring-3 threads share every global) would trample each
	// other's bytes, and so would one thread whose CALLBACK reads a config of
	// its own, which is a step os64_resolve is one consumer away from taking.
	// The walk below NUL-terminates in place and puts the byte back, so the
	// damage would be intermittent and look like a config file that
	// occasionally forgets a line. (Codex review, 2026-08-22.)
	char *buf = (char *)os64_malloc(OS64_CONF_MAX);
	if (buf == NULL) {
		os64_close((int32_t)fd);
		return OS64_CONF_NO_MEMORY;
	}

	size_t got = 0;
	bool truncated = false;
	bool io_error = false;
	for (;;) {
		int64_t n = os64_read((int32_t)fd, buf + got, OS64_CONF_MAX - 1 - got);
		// An error is not an ending (Codex #29 rd12) — same distinction the
		// write path makes, and for a related reason. Nothing has been handed
		// to the callback yet (parsing happens after this loop), so failing
		// here means the caller gets an ERROR rather than a partial file it
		// would treat as the whole truth. A reader that silently drops the
		// tail of hosts, or of gui.conf's start list, is a machine behaving
		// differently for a reason nobody can see.
		if (n < 0) { io_error = true; break; }
		if (n == 0)
			break;                          // genuine end of file
		got += (size_t)n;
		if (got >= OS64_CONF_MAX - 1) {
			// Buffer full — but a file of EXACTLY OS64_CONF_MAX-1 bytes fits,
			// so probe one more byte before crying truncation (Codex #29 rd3).
			// The write path and the kernel reader already do this; without it
			// a valid maximum-size file is falsely rejected and a caller like
			// gclock reports it exceeds the limit. Set truncated only if there
			// really is more.
			char probe;
			int64_t pr = os64_read((int32_t)fd, &probe, 1);
			if (pr > 0)
				truncated = true;
			else if (pr < 0)
				io_error = true;            // still do not know — do not guess
			break;
		}
	}
	os64_close((int32_t)fd);
	if (io_error) {
		os64_free(buf);
		return OS64_CONF_IO_ERROR;
	}
	buf[got] = '\0';

	int64_t delivered = 0;
	size_t i = 0;
	while (buf[i] != '\0') {
		// One line: find its bounds, then chop the comment off.
		size_t start = i;
		while (buf[i] != '\0' && buf[i] != '\n')
			i++;
		size_t end = i;
		if (buf[i] == '\n')
			i++;
		for (size_t c = start; c < end; c++)
			if (buf[c] == '#') { end = c; break; }

		// key: the first token
		size_t k = start;
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t ks = k;
		while (k < end && buf[k] != ' ' && buf[k] != '\t' && buf[k] != '=')
			k++;
		size_t ke = k;
		if (ks == ke)
			continue;   // blank, or comment-only

		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		if (k >= end || buf[k] != '=') {
			// Not `key = value`. Hand the caller the line as written (less
			// its comment and trailing whitespace) and let it complain in
			// its own voice.
			size_t le = end;
			while (le > ks && (buf[le - 1] == ' ' || buf[le - 1] == '\t' || buf[le - 1] == '\r'))
				le--;
			char saved = buf[le];
			buf[le] = '\0';
			bool go = fn(NULL, &buf[ks], user);
			buf[le] = saved;
			if (!go) {
				os64_free(buf);
				return delivered;
			}
			continue;
		}
		k++;   // past '='

		// value: to end of line, trimmed both sides, never split on spaces.
		while (k < end && (buf[k] == ' ' || buf[k] == '\t'))
			k++;
		size_t vs = k, ve = end;
		while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t' || buf[ve - 1] == '\r'))
			ve--;

		// Terminate both in place for the call, then put the bytes back:
		// the walk above still needs the line intact (ke may equal vs's
		// preceding '=', never the same byte as ve, so the two NULs are
		// independent).
		char savedK = buf[ke], savedV = buf[ve];
		buf[ke] = '\0';
		buf[ve] = '\0';
		bool go = fn(&buf[ks], &buf[vs], user);
		buf[ke] = savedK;
		buf[ve] = savedV;
		delivered++;
		if (!go) {
			os64_free(buf);
			return delivered;
		}
	}

	os64_free(buf);
	return truncated ? OS64_CONF_TRUNCATED : delivered;
}

static int64_t conf_resolve(const char *name, size_t from, bool any,
                            char *path_out, size_t cap)
{
	if (path_out == NULL || cap == 0)
		return OS64_CONF_NO_FILE;
	path_out[0] = 0;

	// The kernel answers with the matching ladder index PLUS ONE, so any
	// success is >= 1 and no error code can be mistaken for a position. Hand
	// it back unchanged as the next call's `from` to reach the copy after it.
	int64_t rc = (int64_t)os64_syscall6(SYSCALL_CONF_RESOLVE, (uint64_t)name,
	                                    (uint64_t)path_out, (uint64_t)cap,
	                                    (uint64_t)from, any ? 1u : 0u, 0);
	if (rc < 1)
		return OS64_CONF_NO_FILE;
	return rc;
}

int64_t os64_conf_find_from(const char *name, size_t from,
                            char *path_out, size_t cap)
{
	return conf_resolve(name, from, false, path_out, cap);
}

int64_t os64_conf_find(const char *name, char *path_out, size_t cap)
{
	return (os64_conf_find_from(name, 0, path_out, cap) >= 1) ? 0
	                                                          : OS64_CONF_NO_FILE;
}

int64_t os64_conf_find_read(const char *name, os64_conf_fn fn, void *user,
                            char *path_out, size_t cap)
{
	char scratch[OS64_CONF_PATH_MAX];
	char *where = (path_out != NULL && cap > 0) ? path_out : scratch;
	size_t room = (path_out != NULL && cap > 0) ? cap : sizeof(scratch);

	if (os64_conf_find(name, where, room) != 0)
		return OS64_CONF_NO_FILE;
	return os64_conf_read(where, fn, user);
}

int64_t os64_conf_read_first(const char *const *paths, size_t count,
                             os64_conf_fn fn, void *user, size_t *which)
{
	for (size_t i = 0; i < count; i++) {
		// Open first, read only if it opens — os64_conf_read does exactly
		// that and reports NO_FILE for the misses, so "first that opens"
		// is just "first that doesn't say NO_FILE".
		int64_t rc = os64_conf_read(paths[i], fn, user);
		if (rc != OS64_CONF_NO_FILE) {
			if (which)
				*which = i;
			return rc;
		}
	}
	return OS64_CONF_NO_FILE;
}

// ── one setting, by name ────────────────────────────────────────────────────

typedef struct {
	const char *want;              // the key being looked for
	char       *out;
	size_t      cap;
	bool        found;
	bool        cut;               // the value did not fit `out` — see below
} conf_get_t;

static bool conf_get_line(const char *key, const char *value, void *user)
{
	conf_get_t *g = (conf_get_t *)user;
	if (key == NULL)
		return true;               // a malformed line is not this call's business
	// Case-folded because a key reached through os64_conf_get is a SETTING
	// name. (os64_streq_nocase's header carries the argument for why folding
	// is the caller's choice and never the parser's.)
	if (!os64_streq_nocase(key, g->want))
		return true;
	// os64_strcopy answers with the length the value WANTED, not the length it
	// got — so `>= cap` is how you learn it was cut, and discarding the answer
	// is how this call used to hand back half a setting and call it success
	// (2026-08-24). ASSIGNED, not OR'd: last-one-wins means only the LAST
	// match's fate matters, and a long early value replaced by a short later
	// one is a clean read.
	g->cut   = (os64_strcopy(g->out, g->cap, value) >= g->cap);
	g->found = true;
	return true;                   // keep going: LAST one wins, as everywhere
}

int64_t os64_conf_get(const char *name, const char *key, char *out, size_t cap)
{
	if (out == NULL || cap == 0)
		return OS64_CONF_NO_FILE;
	out[0] = 0;

	char path[OS64_CONF_PATH_MAX];
	if (os64_conf_find(name, path, sizeof(path)) != 0)
		return OS64_CONF_NO_FILE;

	conf_get_t g = { .want = key, .out = out, .cap = cap, .found = false, .cut = false };
	int64_t rc = os64_conf_read(path, conf_get_line, &g);
	if (rc < 0 && rc != OS64_CONF_TRUNCATED)
		return rc;                 // NO_FILE / NO_MEMORY: nothing was read
	if (rc == OS64_CONF_TRUNCATED) {
		// The file was too big to read whole, so "last one wins" is a promise
		// we cannot keep: a later duplicate of `key` may sit in the tail we
		// never saw, which would OVERRIDE whatever we found in the prefix
		// (Codex #29 rd4). Refuse rather than hand back a value a fuller read
		// would have replaced. (The read/write/kernel paths already surface
		// truncation; this is the single-setting reader honouring it too.)
		out[0] = 0;
		return OS64_CONF_TRUNCATED;
	}
	if (!g.found) {
		out[0] = 0;
		return OS64_CONF_NO_KEY;
	}
	if (g.cut) {
		// The value was longer than the caller's buffer. Same verdict, same
		// reasoning as the file-truncation case above: a partial answer that
		// LOOKS complete is worse than no answer, because only one of them
		// gets checked. Half of `position = 900,540` is a valid-looking
		// coordinate pointing somewhere else. (conf.h carries the argument.)
		out[0] = 0;
		return OS64_CONF_TRUNCATED;
	}
	return 0;
}

bool os64_conf_get_bool(const char *value, bool *out)
{
	if (value == NULL || out == NULL)
		return false;
	if (os64_streq_nocase(value, "on") || os64_streq_nocase(value, "yes") ||
	    os64_streq_nocase(value, "true") || os64_streq_nocase(value, "1")) {
		*out = true;
		return true;
	}
	if (os64_streq_nocase(value, "off") || os64_streq_nocase(value, "no") ||
	    os64_streq_nocase(value, "false") || os64_streq_nocase(value, "0")) {
		*out = false;
		return true;
	}
	return false;                  // not a boolean — the caller keeps its default AND says so
}

// ── writing settings back ───────────────────────────────────────────────────
//
// The three rules are argued in os64/conf.h; this is how they are kept.
//
// The merge walks the EXISTING text line by line and rebuilds it: a line
// whose key matches one of the pairs is re-emitted with the new value and
// nothing else about it changes; every other line — comments, blanks,
// settings this call has never heard of — is copied through byte for byte;
// pairs that matched no line are appended at the end. That is why your
// comments survive a save, and it is the only reason a machine-written config
// is still a file you can edit by hand.

// Append a byte to the growing output, tracking overflow rather than
// truncating: a config file that got shorter because it did not fit is a
// config file with settings missing, and the caller must be told.
typedef struct {
	char  *buf;
	size_t cap;
	size_t len;
	bool   overflow;
} conf_out_t;

static void out_ch(conf_out_t *o, char c)
{
	if (o->len + 1 >= o->cap) {
		o->overflow = true;
		return;
	}
	o->buf[o->len++] = c;
}

static void out_str(conf_out_t *o, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		out_ch(o, s[i]);
}

// Write `key = value`, and — when replacing an existing line (`orig` non-NULL)
// — PRESERVE that line's inline comment (Codex #29 rd6). The merge contract
// promises comments are copied through untouched, but reformatting a matched
// line as bare key/value dropped a trailing `# ...`, silently destroying
// user-authored context (`position = 10,20 # laptop layout`). The comment
// text is kept verbatim from the first '#' to end-of-line; only the run of
// space before it is normalised to one. `orig` is NULL for a freshly-appended
// setting, which has no original line and so no comment.
static void out_setting(conf_out_t *o, const char *key, const char *value,
                        const char *orig, size_t orig_len)
{
	out_str(o, key, os64_strlen(key));
	out_str(o, " = ", 3);
	out_str(o, value, os64_strlen(value));
	if (orig != NULL) {
		size_t c = 0;
		while (c < orig_len && orig[c] != '#')
			c++;
		if (c < orig_len) {                 // an inline comment: carry it through
			out_ch(o, ' ');
			out_str(o, &orig[c], orig_len - c);
		}
	}
	out_ch(o, '\n');
}

// Is this line `key = ...` for one of the pairs? Returns its index, or -1.
// The key is compared WITHOUT CASE, matching os64_conf_get: a setting written
// `Titlebar` is the same setting as `titlebar`, and a save must update the
// line that is already there rather than appending a second one beside it.
static int64_t line_pair(const char *line, size_t len,
                         const os64_conf_pair_t *pairs, size_t count)
{
	size_t k = 0;
	while (k < len && (line[k] == ' ' || line[k] == '\t'))
		k++;
	size_t ks = k;
	while (k < len && line[k] != ' ' && line[k] != '\t' && line[k] != '=' && line[k] != '#')
		k++;
	size_t ke = k;
	if (ks == ke)
		return -1;                          // blank or comment-led
	while (k < len && (line[k] == ' ' || line[k] == '\t'))
		k++;
	if (k >= len || line[k] != '=')
		return -1;                          // not a setting line

	for (size_t p = 0; p < count; p++) {
		const char *want = pairs[p].key;
		size_t wl = os64_strlen(want);
		if (wl != ke - ks)
			continue;
		size_t i = 0;
		while (i < wl) {
			char a = line[ks + i], b = want[i];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
			i++;
		}
		if (i == wl)
			return (int64_t)p;
	}
	return -1;
}

// Can this pair survive a round trip through the dialect? The full argument
// is rule 4 in os64/conf.h; the short version is that the writer used to
// treat caller data as file SYNTAX, so a '\n' in a value wrote a second
// setting line and a '#' in one was eaten on the way back in.
//
// The check is deliberately about the three characters the FORMAT reserves,
// not about "unsafe" characters in general: a value may hold spaces, '=',
// quotes, slashes, anything else — the reader takes a value to end of line
// and never splits it — and an EMPTY value is a real setting. Refusing more
// than the format requires would be its own kind of lie about the dialect.
static bool setting_is_writable(const os64_conf_pair_t *p)
{
	const char *k = p->key;
	if (k == NULL || k[0] == '\0')
		return false;                       // no key: writes " = v", reads as nothing
	for (const char *c = k; *c != '\0'; c++)
		if (*c == '\n' || *c == '\r' || *c == '#' || *c == '=' ||
		    *c == ' '  || *c == '\t')
			return false;                   // a key that can never match itself again

	const char *v = p->value;
	if (v == NULL)
		return false;                       // not the same as empty; nothing to write
	for (const char *c = v; *c != '\0'; c++)
		if (*c == '\n' || *c == '\r' || *c == '#')
			return false;                   // a second line, or a value the reader eats
	return true;
}

int64_t os64_conf_write(const char *name, const os64_conf_pair_t *pairs, size_t count)
{
	if (pairs == NULL || count == 0)
		return 0;                           // nothing asked, nothing broken
	if (count > OS64_CONF_WRITE_MAX)
		return OS64_CONF_TOO_MANY;          // refused whole, never written in part

	// BEFORE anything is resolved, opened or allocated: nothing this call
	// cannot read back gets written, and one bad pair refuses the whole save
	// (rule 4 in os64/conf.h — same whole-refusal doctrine as TOO_MANY above,
	// because a save that quietly dropped one of its settings is the silent
	// config failure this arc exists to abolish).
	for (size_t p = 0; p < count; p++)
		if (!setting_is_writable(&pairs[p]))
			return OS64_CONF_BAD_SETTING;

	// RULE 1: the TOP of the ladder, never where we read from. `true` asks
	// the kernel for the path this name WOULD have at position 0 — the file
	// does not exist yet the first time, which is exactly the case a probing
	// resolve cannot answer.
	char path[OS64_CONF_PATH_MAX];
	if (conf_resolve(name, 0, true, path, sizeof(path)) < 1)
		return OS64_CONF_NO_FILE;

	// Room for the old text plus every pair appended, plus the ".new" name.
	size_t room = OS64_CONF_MAX * 2;
	char *old = (char *)os64_malloc(OS64_CONF_MAX);
	char *neu = (char *)os64_malloc(room);
	if (old == NULL || neu == NULL) {
		if (old) os64_free(old);
		if (neu) os64_free(neu);
		return OS64_CONF_NO_MEMORY;
	}

	// Read whatever is there. ABSENT IS FINE — this is a first save, and the
	// merge below simply has nothing to merge with.
	//
	// AN ERROR IS NOT AN ENDING (Codex #29 rd12). This loop used to break on
	// `n <= 0`, folding "the file ended" together with "the read failed" —
	// and those demand opposite responses. On EOF the merge below is right.
	// On an ERROR, `old` holds only the prefix that arrived before the
	// failure, and carrying on publishes that prefix over the user's real
	// config: rule 3's atomic rename then commits the loss cleanly and
	// completely. The buffer-full case three lines down already understood
	// this exact danger ("would silently destroy the tail this routine
	// promises to preserve") — it was guarded for the size door and not for
	// the error door.
	size_t got = 0;
	bool too_big = false;
	bool io_error = false;
	int64_t fd = os64_open(path, "r");
	if (fd >= 0) {
		for (;;) {
			int64_t n = os64_read((int32_t)fd, old + got, OS64_CONF_MAX - 1 - got);
			if (n < 0) { io_error = true; break; }
			if (n == 0)
				break;                      // genuine end of file
			got += (size_t)n;
			if (got >= OS64_CONF_MAX - 1) {
				// Buffer full. Is there MORE? Publishing only this prefix
				// would silently destroy the tail this routine promises to
				// preserve, so probe one byte and refuse if it is there.
				// The probe gets the same three-way answer: >0 means the file
				// continues (too big), 0 means it ended exactly here (fine),
				// and <0 means we STILL do not know — which is not a licence
				// to publish.
				char probe;
				int64_t pr = os64_read((int32_t)fd, &probe, 1);
				if (pr > 0)
					too_big = true;
				else if (pr < 0)
					io_error = true;
				break;
			}
		}
		os64_close((int32_t)fd);
	}
	if (io_error) {
		// Nothing has been written yet — no temp opened, no rename — so the
		// existing config is untouched, which is the entire point of failing
		// here rather than three steps later.
		os64_free(old);
		os64_free(neu);
		return OS64_CONF_IO_ERROR;
	}
	if (too_big) {
		os64_free(old);
		os64_free(neu);
		return OS64_CONF_TRUNCATED;
	}
	old[got] = '\0';

	// RULE 2: MERGE. Rewrite line by line, replacing only what we own.
	// count is <= OS64_CONF_WRITE_MAX (refused above), so every pair has a
	// mark and none can be quietly skipped.
	bool written[OS64_CONF_WRITE_MAX];
	size_t marks = count;
	for (size_t i = 0; i < marks; i++)
		written[i] = false;

	conf_out_t o = { .buf = neu, .cap = room, .len = 0, .overflow = false };
	size_t i = 0;
	while (i < got) {
		size_t start = i;
		while (i < got && old[i] != '\n')
			i++;
		size_t end = i;
		if (i < got)
			i++;                            // step past the newline

		int64_t p = line_pair(&old[start], end - start, pairs, count);
		if (p >= 0 && (size_t)p < marks && !written[p]) {
			// Replacing this line — hand out_setting the ORIGINAL so it can
			// carry through any inline comment (the merge contract).
			out_setting(&o, pairs[p].key, pairs[p].value, &old[start], end - start);
			written[p] = true;
		} else if (p >= 0 && (size_t)p < marks) {
			// A REPEATED key we already rewrote: drop the duplicate rather
			// than leaving a second line that would win the next read and
			// undo the save. (Last-one-wins is the reader's rule; a writer
			// that ignored it would silently save nothing.)
		} else {
			out_str(&o, &old[start], end - start);
			out_ch(&o, '\n');
		}
	}

	// Pairs that matched no existing line go at the end — no original line,
	// so no comment to carry (NULL).
	for (size_t p = 0; p < marks; p++)
		if (!written[p])
			out_setting(&o, pairs[p].key, pairs[p].value, NULL, 0);

	if (o.overflow) {
		os64_free(old);
		os64_free(neu);
		return OS64_CONF_TRUNCATED;         // refuse to publish a short file
	}
	neu[o.len] = '\0';

	// RULE 3: publish ATOMICALLY — write beside it, then rename over. os64's
	// rename replaces atomically (the 2026-08-16 ruling), so a crash between
	// these two calls leaves the OLD config whole rather than a half-written
	// one.
	// The temp name is PER-SAVER, not a shared "<path>.new" (Codex #29 rd7):
	// two processes saving the same file both opened the identical temp, and
	// writer B could truncate it after A closed but before A renamed — so A
	// published an inode B was still filling, a partial file defeating the
	// atomic-write guarantee. The rename over `path` is still the atomic
	// publish; only the temp needs to be nobody else's.
	//
	// TWO parts, because the task id alone is not enough (Codex #29 rd8): it
	// names the TASK, and every thread of a program shares it — so two
	// threads saving the same file collided on the identical temp, the very
	// race rd7 closed, one level down. The sequence number is what separates
	// threads. RELAXED is the right order: we need each caller to come away
	// with a DIFFERENT number, not to publish anything alongside it, and
	// nothing else reads this counter. (GCC atomics rather than an os64 lock
	// — heap.c's own lock is built the same way, in this same library.)
	static uint32_t s_temp_seq;
	uint32_t seq = __atomic_fetch_add(&s_temp_seq, 1u, __ATOMIC_RELAXED);

	char temp[OS64_CONF_PATH_MAX];
	int64_t tlen = os64_snprintf(temp, sizeof(temp), "%s.%lu.%u.new",
	                             path, (unsigned long)os64_taskid(), seq);
	if (tlen < 0 || (size_t)tlen >= sizeof(temp)) {
		os64_free(old);
		os64_free(neu);
		return OS64_CONF_TRUNCATED;
	}

	int64_t out_fd = os64_open(temp, "w");
	if (out_fd < 0) {
		os64_free(old);
		os64_free(neu);
		return OS64_CONF_NO_FILE;
	}
	size_t put = 0;
	bool failed = false;
	while (put < o.len) {
		int64_t n = os64_write((int32_t)out_fd, neu + put, o.len - put);
		if (n <= 0) { failed = true; break; }
		put += (size_t)n;
	}
	// COMMIT BEFORE PUBLISHING (Codex #29 rd14). Every write() above was
	// checked, but "the bytes were accepted" is not "the bytes are on the
	// disk" — on FAT the flush happens at close, and a close that fails
	// while writing metadata cannot tell us so: handle_file_object_close() is
	// void, and file_close_in_kernel() discards the filesystem's answer
	// outright, so checking os64_close's return here would be theatre.
	//
	// os64_sync IS a real commit point and DOES report: syscall_sync runs the
	// filesystem's sync under kKernelPML4 and turns a negative result into an
	// error the caller can see. So the temp is committed and CHECKED before
	// the rename, and a failure lands in `failed` — which unlinks the temp and
	// leaves the original untouched, exactly as a write failure does.
	//
	// On ext2 this is a no-op that returns success, because ext2 writes are
	// full write-through and the promise is already kept (CLAUDE.md). It costs
	// one syscall on the path that is not the risky one, to be correct on the
	// path that is — and /home being ext2 today is a fact about this boot, not
	// a guarantee about the ladder, which is configurable.
	if (!failed && os64_sync((int32_t)out_fd) != 0)
		failed = true;

	os64_close((int32_t)out_fd);
	os64_free(old);
	os64_free(neu);

	if (failed) {
		os64_unlink(temp);                  // no half-file left lying beside the real one
		return OS64_CONF_NO_FILE;
	}
	if (os64_rename(temp, path) != 0) {
		os64_unlink(temp);
		return OS64_CONF_NO_FILE;
	}
	return 0;
}

int64_t os64_conf_set(const char *name, const char *key, const char *value)
{
	os64_conf_pair_t one = { .key = key, .value = value };
	return os64_conf_write(name, &one, 1);
}
