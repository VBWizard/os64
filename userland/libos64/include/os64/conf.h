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
#define OS64_CONF_NO_FILE    (-1)
#define OS64_CONF_TRUNCATED  (-2)
#define OS64_CONF_MAX        8192   // the buffer; 8KB of `key = value` is a lot of opinions
int64_t os64_conf_read(const char *path, os64_conf_fn fn, void *user);

// The persistence gradient, walked for you: try each path in order, read the
// FIRST that opens, return its result (or OS64_CONF_NO_FILE if none did).
// `*which` (optional) receives the index of the file that answered. This is
// the ladder husk climbs for husk.rc and logd for logd.conf — /home's copy
// (the user's, on its own partition) before /etc's (the system's, rewritten
// by every build). Two config files that disagree about precedence is a trap
// nobody deserves at 2am, so the ladder lives here, once.
int64_t os64_conf_read_first(const char *const *paths, size_t count,
                             os64_conf_fn fn, void *user, size_t *which);

#endif
