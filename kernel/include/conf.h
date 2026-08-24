#ifndef CONF_H
#define CONF_H

#include <stdbool.h>
#include <stddef.h>

// conf.h — THE CONFIG SEARCH PATH: one setting that says where config files
// are looked for, first to last, and every reader obeys it.
//
// THE PROBLEM IT RETIRES (ruled by Chris 2026-08-23, the afternoon
// /home/desktop.conf became the sixth file to carry its own private copy of
// the same ladder): logd walked /home then /etc, husk walked /home then /etc
// then two lifeboat spellings, os64get walked /home then /etc then the cwd,
// the resolver walked /etc only, desktop walked /home then /etc. Same idea,
// five spellings, and the seventh file would have added a sixth.
//
// WHY A FILE AND NOT THE ENVIRONMENT. Chris considered the environment and
// rejected it: it freezes at spawn, it is per-process, and the kernel's own
// readers (desktop, and the resolver's hosts file if it ever moves in here)
// have no environment at all. A file tells the truth at READ time — the
// /sys/gui ruling (GRAPHICS.md) applied to configuration.
//
// THE ROOT FILE has a fixed address, because something has to be findable
// before a search path exists:
//
//   /etc/os64.conf        conf = /home/conf:/etc
//
// Colon-separated, first hit wins — PATH's shape (1979, and still the right
// one for "a list of places, in order"). An absent file or absent key means
// the built-in default "/home:/etc", which is what every reader did before
// this existed, so nothing moves until somebody writes the line. Other
// system-wide knobs that do not belong in the environment will accumulate in
// that file over time; that is the point of having a root file at all.
//
// /home/conf, NOT /home/.config. Dotfiles were never designed: an early Unix
// `ls` skipped "." and ".." by testing only the first character, so every
// name beginning with '.' vanished, and hiding files that way became a habit
// by accident (Rob Pike's account). os64's readdir never delivers "." or
// ".." and has no dot magic to hide behind, so a visible directory in a
// curated tree is the honest shape.

// How many directories a search path may name. Eight is not a ration — it is
// a tripwire: a config ladder longer than that is a symptom, not a setting.
#define CONF_MAX_DIRS   8
#define CONF_DIR_MAX    96    // one directory in the ladder
#define CONF_PATH_MAX   256   // a resolved "<dir>/<name>"
#define CONF_NAME_MAX   48    // "desktop.conf" and its cousins

// Read /etc/os64.conf and settle the search path. Call ONCE, from
// kernel_init, AFTER the root filesystem mounts and the secondary partitions
// auto-mount — the order matters and is why this cannot run earlier: the
// default ladder names /home, which is a mount, and a walker that ran before
// it would conclude the user's directory does not exist.
//
// CONTEXT: this one runs in ktask under kKernelPML4 (kernel_init's context),
// where a direct VFS call is what every other init-time reader does. Every
// LATER entry point takes the call_in_kernel_context trampoline, because by
// then the caller can be any task at all.
void conf_init(void);

// Find the config file called `name` ("desktop.conf"), walking the ladder and
// returning the first path that opens. Writes the winning path into `out` and
// returns true; returns false (and leaves `out` empty) if no directory has
// it. Safe from any task context — the probe takes the kernel-context
// trampoline, the mount routing does not need it.
//
// Every call is REMEMBERED for /sys/conf and announced once at DEBUG_BOOT
// ("conf: desktop.conf <- /home/conf/desktop.conf"). Chris asked for that
// explicitly: "for some time I'll want to be able to verify where each conf
// file is coming from." DEBUG_BOOT because he always has it on, and because
// which config file won is boot news, not subsystem debug.
bool conf_find(const char *name, char *out, size_t cap);

// The same walk, resumable: search from ladder position `from` onward, and
// return the index that answered (or -1 for none). Pass the previous return
// value PLUS ONE to get the next copy.
//
// FOR THE READER THAT WANTS EVERY COPY, NOT THE FIRST. Almost every config
// file is a settings file, where first-hit-wins is the whole point — your
// /home/logd.conf replaces the system's. `hosts` is not one: Chris ruled it
// MERGED on 2026-08-22, /home/hosts layering ON TOP of /etc/hosts so your
// machine names sit over the system's list rather than erasing it. It is a
// database, not a setting, and the distinction is real enough that the
// walker serves both rather than forcing the resolver to keep a private
// ladder — which is the exact disease this file cures.
//
// Only a `from` of 0 is remembered for /sys/conf: the first copy is the one
// worth calling "the file this reader took", and an enumeration would
// otherwise leave the last copy standing there claiming to be it.
int conf_find_from(const char *name, size_t from, char *out, size_t cap);

// Where `name` WOULD live at ladder position `index`, whether or not anything
// is there. No probe, no note, no log line — pure string arithmetic.
//
// FOR THE WRITER. A program that read /etc/gclock.conf must not save back
// there: /etc is the SYSTEM's and every build rewrites it, so settings stored
// there evaporate on the next `make` (the persistence doctrine). Settings are
// written to the TOP of the ladder — index 0, /home by default — which is
// also the copy that wins the next read. Read from wherever; write to slot 0.
bool conf_path_at(size_t index, const char *name, char *out, size_t cap);

// What /sys/conf publishes. `index` walks the remembered lookups; returns
// false once past the end. `path` is empty for a name nothing answered.
// ── reading a config file, kernel side ──────────────────────────────────────
// The two halves every kernel-side reader needs, so that none of them has to
// grow a third copy of either. (desktop.c had the first; the root file's own
// parser was the second; gui/startup.c would have been the third.)

// Whole file into a kmalloc'd, NUL-terminated buffer — caller kfree()s it.
// NULL if absent, unreachable, or larger than `cap`. Takes the
// call_in_kernel_context trampoline, so it is safe from any task.
uint8_t *conf_read_file(const char *path, size_t cap, size_t *out_len);

// Walk `key = value` lines, '#' comments, whitespace-tolerant — os64's whole
// config dialect, and the kernel's mirror of libos64's os64_conf_read.
// `text` is modified in place (the walk NUL-terminates each token).
//
// key == NULL means a line that was not `key = value`; `value` is the
// offending text. Complain however your reader complains — the commonest
// mistake is writing the value with no key, and silence there costs an
// afternoon (logd's lesson, kept).
//
// KEYS ARE CASE-SENSITIVE AND LOWERCASE BY CONVENTION. Every config file in
// os64 uses lowercase keys; a reader that compares against a capitalized
// spelling matches nothing and falls back to its defaults without a word
// about why. (gclock.conf shipped `Position` against a `"position"` compare
// on 2026-08-23 and every setting in it was silently ignored.)
typedef void (*conf_line_fn)(const char *key, const char *value, void *user);
void conf_parse(char *text, conf_line_fn fn, void *user);

size_t conf_dir_count(void);
const char *conf_dir(size_t index);
const char *conf_source(void);   // "/etc/os64.conf", or "built-in default"
// Copies note `index` into the caller's buffers (a locked snapshot — see the
// definition). Returns false once past the last note.
bool conf_note(size_t index, char *name_out, size_t name_cap,
                             char *path_out, size_t path_cap);

#endif // CONF_H
