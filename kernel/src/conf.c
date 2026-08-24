// conf.c — the config search path. Contract, grammar and the ruling behind
// it are in conf.h; this file is the walker.
//
// Two jobs, and they are deliberately the ONLY two:
//   1. read /etc/os64.conf once and settle an ordered list of directories
//   2. answer "where is <name>?" by walking that list, first hit wins
//
// It does NOT parse anybody's config VALUES. Each reader keeps its own value
// parser — only the FINDING is shared, because finding is the part six
// readers had each spelled differently.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "conf.h"
#include "CONFIG.h"
#include "driver/filesystem/vfs/vfs.h"
#include "kmalloc.h"
#include "memory/vma.h"        // call_in_kernel_context
#include "printd.h"
#include "spinlock.h"
#include "strings/strcmp.h"
#include "strings/strlen.h"
#include "strings/strcpy.h"

#define CONF_ROOT_FILE     "/etc/os64.conf"
#define CONF_DEFAULT_PATH  "/home:/etc"
#define CONF_ROOT_MAX      8192   // the same 8KB cap every os64 config reader uses
#define CONF_NOTES_MAX     16     // lookups remembered for /sys/conf

// The settled ladder. Written once by conf_init (single-threaded, before any
// task can call in), read by everything afterwards.
static char   s_dirs[CONF_MAX_DIRS][CONF_DIR_MAX];
static size_t s_dir_count = 0;
static char   s_source[CONF_PATH_MAX] = "built-in default";

// What each reader actually took — the diagnostic Chris asked for. A fixed
// table rather than a list: the whole point is that the set of config files
// is SMALL, and a table that cannot grow is a table that cannot leak.
typedef struct {
	char name[CONF_NAME_MAX];
	char path[CONF_PATH_MAX];   // empty = nothing answered
} conf_note_t;

static conf_note_t s_notes[CONF_NOTES_MAX];
static size_t      s_note_count = 0;

// Guards the notes only. The ladder is immutable after init and needs none.
static spinlock_t kConfLock = { 0 };

// ── the kernel-context file probe ───────────────────────────────────────────
// The house pattern (syscall_open's, desktop.c's): resolve the mount OUTSIDE
// — it is pure string matching against kernel .data and safe from any CR3 —
// then do the actual I/O on the trampoline, because a filesystem read touches
// driver buffers and MMIO that live only in kKernelPML4. Params come from
// kmalloc (HHDM, visible under any CR3), never from the caller's stack.
typedef struct {
	vfs_filesystem_t *fs;
	const char       *tail;    // points INTO path_copy below
	char              path_copy[CONF_PATH_MAX];
	uint8_t          *buf;     // out: whole-file read, or NULL for a mere probe
	size_t            cap;     // 0 = probe only (open, close, report)
	size_t            len;     // out
	bool              opened;  // out
} conf_io_t;

static void conf_io_kernel(void *arg)
{
	conf_io_t *p = (conf_io_t *)arg;
	p->opened = false;
	p->buf = NULL;
	p->len = 0;

	vfs_filesystem_t *fs = p->fs;
	if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL)
		return;

	vfs_file_t *file = NULL;
	if (fs->fops->open(&file, p->tail, "r", fs) != 0)
		return;
	p->opened = true;

	if (p->cap > 0 && fs->fops->read != NULL) {
		uint8_t *buf = kmalloc(p->cap + 1);
		size_t len = 0;
		if (buf != NULL) {
			bool oversized = false;
			for (;;) {
				if (len >= p->cap) {
					// At the cap. conf_read_file's contract (conf.h) is NULL
					// for a file LARGER than cap, and gui_startup's "start
					// nothing" relies on it — so probe one byte and reject the
					// whole read if the file continues past here, rather than
					// returning a truncated prefix as a success.
					uint8_t probe;
					if (fs->fops->read(file, &probe, 1) > 0)
						oversized = true;
					break;
				}
				int n = fs->fops->read(file, buf + len, p->cap - len);
				if (n <= 0)
					break;
				len += (size_t)n;
			}
			if (oversized) {
				kfree(buf);   // p->buf stays NULL → conf_read_file returns NULL
			} else {
				buf[len] = 0;
				p->buf = buf;
				p->len = len;
			}
		}
	}

	if (fs->fops->close != NULL)
		fs->fops->close(file);
}

// Fill in the routing half and run the I/O half. `direct` skips the
// trampoline — true ONLY from conf_init, which runs in ktask under
// kKernelPML4 where the trampoline's stack swap would be a needless risk and
// a direct call is what every other init-time reader does.
static bool conf_io(const char *path, size_t cap, bool direct,
                    uint8_t **out_buf, size_t *out_len)
{
	if (out_buf)
		*out_buf = NULL;
	if (out_len)
		*out_len = 0;

	conf_io_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return false;

	strncpy(p->path_copy, path, sizeof(p->path_copy));
	p->path_copy[sizeof(p->path_copy) - 1] = 0;

	const char *tail = NULL;
	p->fs = vfs_resolve_mount(p->path_copy, &tail);
	p->tail = tail;
	p->cap = cap;

	if (p->fs == NULL) {
		kfree(p);
		return false;   // nothing mounted there — not an error, just a miss
	}

	if (direct)
		conf_io_kernel(p);
	else
		call_in_kernel_context(conf_io_kernel, p);

	bool opened = p->opened;
	if (out_buf)
		*out_buf = p->buf;
	else if (p->buf != NULL)
		kfree(p->buf);
	if (out_len)
		*out_len = p->len;
	kfree(p);
	return opened;
}

// ── parsing the ladder ──────────────────────────────────────────────────────

static inline bool conf_is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

// Split "dir:dir:dir" into s_dirs. Empty entries are skipped rather than
// refused — a stray colon is a typo, not a reason to lose the whole setting
// — and a trailing slash is trimmed so "/etc/" and "/etc" name one place.
static void conf_set_path(const char *value)
{
	s_dir_count = 0;

	const char *p = value;
	while (*p != '\0' && s_dir_count < CONF_MAX_DIRS) {
		while (conf_is_space(*p))
			p++;
		const char *start = p;
		while (*p != '\0' && *p != ':')
			p++;
		const char *end = p;
		while (end > start && conf_is_space(end[-1]))
			end--;

		size_t len = (size_t)(end - start);
		// A trailing slash would make "<dir>/<name>" grow a double slash.
		// Root is the one directory that IS a slash; it keeps it.
		while (len > 1 && start[len - 1] == '/')
			len--;

		if (len > 0 && len < CONF_DIR_MAX) {
			for (size_t i = 0; i < len; i++)
				s_dirs[s_dir_count][i] = start[i];
			s_dirs[s_dir_count][len] = 0;
			s_dir_count++;
		} else if (len >= CONF_DIR_MAX) {
			// TRIPWIRE, not silence: a directory too long to hold is the kind
			// of thing that would otherwise present as "my config is ignored".
			printd(DEBUG_BOOT, "conf: search path entry %u is too long (>%u) — skipped\n",
			       (uint32_t)s_dir_count, (uint32_t)CONF_DIR_MAX - 1);
		}

		if (*p == ':')
			p++;
	}

	if (*p != '\0')
		printd(DEBUG_BOOT, "conf: search path has more than %u entries — the rest are ignored\n",
		       (uint32_t)CONF_MAX_DIRS);
}

// The dialect walker — `key = value`, '#' comments — shared by every
// kernel-side reader. The kernel's mirror of libos64's os64_conf_read, and
// re-implemented rather than shared with it for the obvious reason: that one
// is ring-3 code and this is the kernel. (Contract in conf.h.)
void conf_parse(char *text, conf_line_fn fn, void *user)
{
	char *line = text;

	while (*line != '\0') {
		char *end = line;
		while (*end != '\0' && *end != '\n')
			end++;
		char saved = *end;
		*end = 0;

		char *hash = line;
		while (*hash != '\0' && *hash != '#')
			hash++;
		*hash = 0;

		char *key = line;
		while (conf_is_space(*key))
			key++;
		char *k = key;
		while (*k != '\0' && !conf_is_space(*k) && *k != '=')
			k++;
		char *key_end = k;
		while (conf_is_space(*k))
			k++;
		if (*k == '=') {
			*key_end = 0;
			char *value = k + 1;
			while (conf_is_space(*value))
				value++;
			size_t vlen = strlen(value);
			while (vlen > 0 && conf_is_space(value[vlen - 1]))
				vlen--;
			value[vlen] = 0;

			fn(key, value, user);
		} else if (key[0] != '\0') {
			// Not `key = value` and not blank. The commonest spelling of this
			// mistake is a value written with no key at all, which is exactly
			// the shape that looks fine and does nothing.
			fn(NULL, key, user);
		}

		if (saved == '\0')
			break;
		line = end + 1;
	}
}

// The root file's own reader, now one ordinary user of the walker above.
static void root_line(const char *key, const char *value, void *user)
{
	bool *found = (bool *)user;
	if (key == NULL)
		return;   // a malformed line in the root file: see the note below
	if (strcmp(key, "conf") == 0) {
		conf_set_path(value);
		*found = true;
	}
	// Unknown keys are NOT an error here. /etc/os64.conf is the system's root
	// config and is expected to accumulate other knobs; a walker that
	// complained about settings it does not own would have to be taught every
	// one of them. (Its READERS should complain about their own unknown keys
	// — that is where the knowledge lives.)
}

uint8_t *conf_read_file(const char *path, size_t cap, size_t *out_len)
{
	uint8_t *buf = NULL;
	size_t   len = 0;
	conf_io(path, cap, false, &buf, &len);
	if (out_len)
		*out_len = len;
	return buf;
}

// ── the public face ─────────────────────────────────────────────────────────

void conf_init(void)
{
	uint8_t *text = NULL;
	size_t   len = 0;
	bool     opened = false;
	bool     from_file = false;

	// direct = true: kernel_init context, ktask, kKernelPML4. See conf.h.
	if (conf_io(CONF_ROOT_FILE, CONF_ROOT_MAX, true, &text, &len)) {
		opened = true;
		if (text != NULL) {
			conf_parse((char *)text, root_line, &from_file);
			kfree(text);
		}
	}

	// THREE OUTCOMES, THREE ANSWERS — because "built-in default" alone cannot
	// distinguish "there is no root file" from "the root file is there and
	// says nothing about `conf`", and those want opposite next moves. (The
	// shipped /etc/os64.conf has its `conf =` line commented out on purpose:
	// the default IS that line, so an untouched system takes the second
	// branch, and a reader of /sys/conf should be able to see that the file
	// was found and simply had no opinion.)
	if (from_file) {
		strncpy(s_source, CONF_ROOT_FILE, sizeof(s_source));
	} else {
		conf_set_path(CONF_DEFAULT_PATH);
		if (opened)
			strncpy(s_source, "built-in default (" CONF_ROOT_FILE " sets no conf)",
			        sizeof(s_source));
		else
			strncpy(s_source, "built-in default (no " CONF_ROOT_FILE ")",
			        sizeof(s_source));
	}
	s_source[sizeof(s_source) - 1] = 0;

	// A root file that exists but names nothing usable would otherwise leave
	// the system with NO ladder at all, which reads from the outside exactly
	// like "configuration stopped working".
	if (s_dir_count == 0) {
		printd(DEBUG_BOOT, "conf: %s set no usable directories — keeping the built-in default\n",
		       CONF_ROOT_FILE);
		conf_set_path(CONF_DEFAULT_PATH);
		strncpy(s_source, "built-in default (" CONF_ROOT_FILE "'s conf was unusable)",
		        sizeof(s_source));
		s_source[sizeof(s_source) - 1] = 0;
	}

	// One line, unconditional at DEBUG_BOOT: the ladder and where it came
	// from. Everything below reports against this.
	char joined[CONF_PATH_MAX];
	size_t at = 0;
	for (size_t i = 0; i < s_dir_count; i++) {
		if (i > 0 && at + 1 < sizeof(joined))
			joined[at++] = ':';
		size_t dl = strlen(s_dirs[i]);
		for (size_t j = 0; j < dl && at + 1 < sizeof(joined); j++)
			joined[at++] = s_dirs[i][j];
	}
	joined[at] = 0;
	printd(DEBUG_BOOT, "conf: search path %s (%s)\n", joined, s_source);
}

// Remember what a lookup answered, for /sys/conf. A repeat lookup of the same
// name OVERWRITES its row rather than adding one: the file a reader is using
// is a fact about now, not a history, and a re-read after a config change
// should show the new answer.
static void conf_note_used(const char *name, const char *path)
{
	uint64_t flags = spinlock_acquire_irqsave(&kConfLock);

	size_t slot = s_note_count;
	for (size_t i = 0; i < s_note_count; i++) {
		if (strcmp(s_notes[i].name, name) == 0) {
			slot = i;
			break;
		}
	}
	if (slot == CONF_NOTES_MAX) {
		spinlock_release_irqrestore(&kConfLock, flags);
		return;   // more config files than the table holds; the log still has them
	}
	if (slot == s_note_count)
		s_note_count++;

	strncpy(s_notes[slot].name, name, CONF_NAME_MAX);
	s_notes[slot].name[CONF_NAME_MAX - 1] = 0;
	if (path != NULL) {
		strncpy(s_notes[slot].path, path, CONF_PATH_MAX);
		s_notes[slot].path[CONF_PATH_MAX - 1] = 0;
	} else {
		s_notes[slot].path[0] = 0;
	}

	spinlock_release_irqrestore(&kConfLock, flags);
}

bool conf_find(const char *name, char *out, size_t cap)
{
	return conf_find_from(name, 0, out, cap) >= 0;
}

// A NAME IS A FILE NAME, NOT A PATH. Refused here so every caller inherits
// the rule: a reader asking for "../../etc/shadow" would be walking the
// ladder somewhere the ladder does not go.
static bool conf_name_ok(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return false;
	for (const char *c = name; *c != '\0'; c++)
		if (*c == '/')
			return false;
	return true;
}

bool conf_path_at(size_t index, const char *name, char *out, size_t cap)
{
	if (out == NULL || cap == 0)
		return false;
	out[0] = 0;
	if (index >= s_dir_count || !conf_name_ok(name))
		return false;

	size_t at = 0;
	size_t dl = strlen(s_dirs[index]);
	for (size_t j = 0; j < dl && at + 1 < cap; j++)
		out[at++] = s_dirs[index][j];
	// Root is "/" and already ends in one; everything else needs the joint.
	if (at > 0 && out[at - 1] != '/' && at + 1 < cap)
		out[at++] = '/';
	size_t nl = strlen(name);
	for (size_t j = 0; j < nl && at + 1 < cap; j++)
		out[at++] = name[j];
	out[at] = 0;

	// Truncation is a REFUSAL, not a shorter path: half a path names a
	// different file, and on a curated tree it may well name a real one.
	if (at != dl + (dl > 0 && s_dirs[index][dl - 1] != '/' ? 1u : 0u) + nl) {
		out[0] = 0;
		return false;
	}
	return true;
}

int conf_find_from(const char *name, size_t from, char *out, size_t cap)
{
	if (out == NULL || cap == 0)
		return -1;
	out[0] = 0;
	if (name == NULL || name[0] == '\0')
		return -1;

	// conf_name_ok holds the rule (a name is a FILE name, never a path);
	// this is where it is announced, loudly, the way unknown open modes are.
	if (!conf_name_ok(name)) {
		printd(DEBUG_BOOT, "conf: refusing lookup of '%s' — a config name is a file name, not a path\n",
		       name);
		return -1;
	}

	// "the misses" for the log line: which directories were asked and said no.
	char misses[CONF_PATH_MAX];
	size_t mat = 0;
	misses[0] = 0;

	for (size_t i = from; i < s_dir_count; i++) {
		char candidate[CONF_PATH_MAX];
		if (!conf_path_at(i, name, candidate, sizeof(candidate)))
			continue;
		size_t at = strlen(candidate);

		if (conf_io(candidate, 0, false, NULL, NULL)) {
			strncpy(out, candidate, cap);
			out[cap - 1] = 0;
			// Only the primary lookup is remembered — an enumeration's later
			// copies would otherwise overwrite the row and claim to be the
			// file this reader took. See conf_find_from in conf.h.
			if (from == 0)
				conf_note_used(name, out);
			if (mat > 0)
				printd(DEBUG_BOOT, "conf: %s <- %s (no %s)\n", name, out, misses);
			else
				printd(DEBUG_BOOT, "conf: %s <- %s\n", name, out);
			return (int)i;
		}

		if (mat > 0 && mat + 2 < sizeof(misses)) {
			misses[mat++] = ',';
			misses[mat++] = ' ';
		}
		for (size_t j = 0; j < at && mat + 1 < sizeof(misses); j++)
			misses[mat++] = candidate[j];
		misses[mat] = 0;
	}

	// A resumed walk that runs out is the ORDINARY end of an enumeration, not
	// a failure worth a line in the boot log or a "(not found)" row.
	if (from == 0) {
		conf_note_used(name, NULL);
		printd(DEBUG_BOOT, "conf: %s NOT FOUND (no %s)\n", name, misses);
	}
	return -1;
}

size_t conf_dir_count(void)
{
	return s_dir_count;
}

const char *conf_dir(size_t index)
{
	return (index < s_dir_count) ? s_dirs[index] : NULL;
}

const char *conf_source(void)
{
	return s_source;
}

// Copy note `index` into the caller's buffers UNDER kConfLock (Codex #29 rd6).
// This used to return raw pointers into s_notes[], which sysfs formatted after
// the call — but conf_note_used mutates those slots under the lock, so a
// concurrent lookup could bump s_note_count before its strncpy finished and a
// reader would format a blank or half-written slot. The whole point of
// /sys/conf is to report config origins accurately, so it must read a
// consistent snapshot: copy while the writer is excluded, then format from the
// caller's own copy.
bool conf_note(size_t index, char *name_out, size_t name_cap,
                             char *path_out, size_t path_cap)
{
	uint64_t flags = spinlock_acquire_irqsave(&kConfLock);
	if (index >= s_note_count) {
		spinlock_release_irqrestore(&kConfLock, flags);
		return false;
	}
	if (name_out && name_cap) {
		strncpy(name_out, s_notes[index].name, name_cap);
		name_out[name_cap - 1] = 0;
	}
	if (path_out && path_cap) {
		strncpy(path_out, s_notes[index].path, path_cap);
		path_out[path_cap - 1] = 0;
	}
	spinlock_release_irqrestore(&kConfLock, flags);
	return true;
}
