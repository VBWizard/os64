#ifndef OS64_CONF_H
#define OS64_CONF_H

// os64/conf.h — the `key = value` file, read once, in one place.
//
// os64's configuration dialect was settled by /etc/logd.conf on 2026-08-18
// and its header made a promise: "whatever this file does, the next daemon
// that needs configuring will copy". The next one was os64get (2026-08-22,
// /etc/os64get.conf — where a fetched file goes, and where its archive copy
// lives), and copying a parser is how two files come to disagree about what
// a comment is. So the reader moved here, the day its second consumer
// arrived — consumer-driven, like everything else in this library.
//
// THE DIALECT, in full:
//
//   key = value        whitespace anywhere, '#' starts a comment
//
// No sections, no quoting, no escapes, no continuations. A value runs to the
// end of its line, trimmed both sides and NOT split on spaces (a logd format
// string is full of them). A key is one token: anything up to whitespace or
// '='. Keys may repeat; the reader delivers every line it finds, in file
// order, and "last one wins" is the caller's rule to apply or not.
//
// THE CALLBACK, not a lookup. logd wants one key; os64get wants every rule
// in the file, in order, because the ORDER of its rules is a precedence.
// A per-line callback serves both without the library holding an opinion
// about what a file means:
//
//   key != NULL  a well-formed setting: key and value, both NUL-terminated,
//                valid only for the duration of the call (copy what you keep)
//   key == NULL  a line that was not `key = value` (the commonest mistake is
//                writing the value alone); `value` is the offending text.
//                Complain however your program complains — logd writes it to
//                the top of the log file, a utility says it on stderr.
//
// Return true to continue, false to stop reading.
typedef bool (*os64_conf_fn)(const char *key, const char *value, void *user);

// Read `path` and deliver its lines. Returns the number of well-formed
// settings delivered (0 for an empty file), OS64_CONF_NO_FILE if the file
// could not be opened, OS64_CONF_TRUNCATED if it was larger than the reader's
// buffer — whatever fit WAS delivered, and the caller should say so, because
// "my setting is at the bottom of the file and nothing happens" is a
// miserable afternoon (logd's lesson, kept).
// ...and OS64_CONF_NO_MEMORY if the reader could not get its buffer. The
// buffer is per-CALL heap (2026-08-22): a static one is shared by every
// thread in the program and by any callback that reads a config of its own,
// and this reader NUL-terminates in place, so sharing it means lines that
// occasionally arrive with a hole in them. 8KB per call, for the duration of
// the call, is the cheaper mistake.
#define OS64_CONF_NO_FILE    (-1)
#define OS64_CONF_TRUNCATED  (-2)
#define OS64_CONF_NO_MEMORY  (-3)
#define OS64_CONF_MAX        8192   // the buffer; 8KB of `key = value` is a lot of opinions
int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *user);

// THE SEARCH PATH (2026-08-23). Ask the system where the config file called
// `name` lives — "logd.conf", a FILE name and never a path — and receive the
// first path in the search path that has it.
//
// The ladder itself is /etc/os64.conf's `conf = /home:/etc` and the WALK is
// the KERNEL's (SYSCALL_CONF_RESOLVE). That is not an accident of where the
// code ended up: six programs each carried a private copy of the same
// "/home then /etc" sequence until the afternoon Chris ruled this into
// existence, and a ladder every reader is supposed to obey must be parsed by
// exactly ONE thing or it is not one ladder. The kernel already walks it for
// its own readers (the desktop background), so ring 3 asks that walker
// rather than growing a second one.
//
// You get the diagnostic for free: because the kernel resolved it,
// `cat /sys/conf` shows the ladder AND which file every reader actually took,
// and each resolve prints a line in the boot log at DEBUG_BOOT.
//
// Returns 0 and fills `path_out` on success; OS64_CONF_NO_FILE if no
// directory in the search path has it (use your defaults) or if the buffer is
// too small for the answer — a truncated path is refused rather than handed
// back, because half a path opens nothing, or something else.
#define OS64_CONF_PATH_MAX 256
int64_t os64_conf_find(const char *name, char *path_out, size_t cap);

// The same walk, RESUMABLE — for the reader that wants every copy on the
// ladder rather than the first. Returns a positive cursor (the matching
// position plus one) to hand back as the next call's `from`, or
// OS64_CONF_NO_FILE when nothing is left:
//
//   size_t from = 0;
//   char path[OS64_CONF_PATH_MAX];
//   int64_t next;
//   while ((next = os64_conf_find_from("hosts", from, path, sizeof path)) >= 1) {
//       ...read path...
//       from = (size_t)next;
//   }
//
// Almost nothing wants this: a config file is normally a SETTINGS file, and
// first-hit-wins is the whole point of the ladder — your copy replaces the
// system's. `hosts` is the exception Chris ruled on 2026-08-22, MERGED so
// your machine names layer over the system's list instead of erasing it. It
// is a database, not a setting.
int64_t os64_conf_find_from(const char *name, size_t from,
                            char *path_out, size_t cap);

// find + read, which is what every caller actually wants. `path_out` may be
// NULL if you do not care WHICH file answered (the log and /sys/conf both
// know); otherwise it receives the path that was read.
int64_t os64_conf_find_read(const char *name, os64_conf_fn fn, void *user,
                            char *path_out, size_t cap);

// The persistence gradient, walked for you: try each path in order, read the
// FIRST that opens, return its result (or OS64_CONF_NO_FILE if none did).
// `*which` (optional) receives the index of the file that answered.
//
// PREFER os64_conf_find_read for a system config file. This one survives for
// the caller with a list of its OWN — husk's lifeboat spellings, which are
// deliberately outside the search path because they exist for the day the
// root carrying /etc/os64.conf is broken.
int64_t os64_conf_read_first(const char *const *paths, size_t count,
                             os64_conf_fn fn, void *user, size_t *which);

// ── one setting, by name ────────────────────────────────────────────────────

// Find `name` on the search path and copy the value of `key` into `out`.
// Returns 0 on success, OS64_CONF_NO_FILE if the file isn't there,
// OS64_CONF_NO_KEY if it is but says nothing about `key`, or the reader's
// error otherwise. `out` is left empty on every failure.
//
// THE KEY IS MATCHED WITHOUT REGARD TO CASE (os64_streq_nocase), because a
// key reached through this call is a SETTING name, where case is noise. That
// is a property of this entry point, not of the dialect — a reader whose keys
// are data (os64get's filenames) uses the callback and compares them itself.
//
// LAST ONE WINS, and this is for SETTINGS ONLY. A repeated key here silently
// keeps the last value, which is right for `format = ` and wrong for a key
// whose repetition IS the meaning: gui.conf's `start` and os64get's rules are
// ordered lists, and those keep os64_conf_read's callback.
#define OS64_CONF_NO_KEY (-4)
int64_t os64_conf_get(const char *name, const char *key,
                      char *out, size_t cap);

// Interpret a config value as a boolean: `on`/`off`, `yes`/`no`,
// `true`/`false`, `1`/`0`, any case. Returns whether the value was UNDERSTOOD
// and writes the answer to *out; leaves *out untouched otherwise.
//
// WHY AN OUT-PARAMETER AND NOT A PLAIN bool RETURN. There are three answers
// here, not two: true, false, and "that is not a boolean at all"
// (`titlebar = purple`). A function returning bool collapses the third into
// the second, so a typo silently becomes `false` and the setting quietly does
// the opposite of what was written — which is exactly the class of failure
// this whole config arc exists to abolish. With the answer in *out, the
// return value means "did I understand it", so a caller keeps its default AND
// says why. Tripwires over silence.
bool os64_conf_get_bool(const char *value, bool *out);

// ── writing settings back ───────────────────────────────────────────────────

typedef struct {
    const char *key;
    const char *value;
} os64_conf_pair_t;

// Update `count` settings in the config file called `name` and publish the
// result. Returns 0, or a negative OS64_CONF_* on failure.
//
// Three rules, and each of them is load-bearing:
//
// 1. IT WRITES TO THE TOP OF THE SEARCH PATH, not to the file it read. A
//    program that read /etc/gclock.conf must NOT write back there: /etc is
//    the SYSTEM's, rewritten by every build, so settings saved there vanish
//    on the next `make` (the persistence doctrine, CLAUDE.md). The user's
//    copy is the first directory in the ladder — /home by default — and once
//    written it wins the next read, which is precisely the behaviour wanted.
//
// 2. IT MERGES, IT DOES NOT CLOBBER. An existing file is rewritten line by
//    line: a line whose key matches gets its value replaced, everything else
//    — comments, blank lines, ordering, settings this call knows nothing
//    about — is copied through untouched, and keys that were not present are
//    appended. Every program that rewrites your config and eats your comments
//    is a small betrayal; this is the forty lines that decline to commit one.
//
// 3. IT PUBLISHES ATOMICALLY: the new text is written to `<path>.new` and
//    RENAMED over the target. os64's rename replaces atomically (syscall 43,
//    ruled 2026-08-16), and write-a-temp-then-publish is the case it exists
//    for. A crash mid-write leaves the OLD config intact rather than a
//    truncated one — a config file is exactly the thing you cannot afford to
//    find half-written after a bad day.
// More settings than this in ONE save is refused (OS64_CONF_TOO_MANY) rather
// than partly written: the merge tracks which pairs it has placed in a fixed
// array, and a writer that silently dropped the seventeenth would be the
// exact silent-config-failure this arc exists to abolish. Raise the number if
// a real caller ever needs more.
#define OS64_CONF_WRITE_MAX 16
#define OS64_CONF_TOO_MANY  (-5)
int64_t os64_conf_write(const char *name,
                        const os64_conf_pair_t *pairs, size_t count);

// One setting, same three rules. `os64_conf_write` with a count of one.
int64_t os64_conf_set(const char *name, const char *key, const char *value);

#endif
