#ifndef OS64_CLIP_H
#define OS64_CLIP_H

// libos64 clipboard helpers — the system snarf buffer, for programs that
// would rather call a function than open a path. Design: CLIPBOARD.md.
//
// THERE IS NO SYSCALL HERE, and that is the point. The clipboard is the FILE
// /sys/clipboard: copying is `something > /sys/clipboard` and pasting is
// `cat /sys/clipboard`, so every text utility in the OS was clipboard-aware
// the day the file appeared — no new verb, no new ABI, pipes and redirection
// already work. (Plan 9 got here first: rio served its snarf buffer as
// /dev/snarf for exactly this reason.) These three functions are a courtesy
// wrapper over open/read/write/close for apps like scribe, which have bytes
// in hand rather than a pipeline.
//
// ONE buffer, system-wide: a terminal selection and a scribe Ctrl+C feed the
// same snarf, and the newer one wins. That is ruled, not accidental —
// X11 shipped PRIMARY *and* CLIPBOARD and spent forty years watching people
// paste the wrong one.

#include <stddef.h>
#include <stdint.h>

// The path itself, for code that wants to hand it to os64_open directly (or
// print it in a help screen). It is a FILE and always will be — history, when
// it arrives, gets its own name beside this one.
#define OS64_CLIPBOARD_PATH "/sys/clipboard"

// Copy `length` bytes onto the clipboard, replacing whatever was there.
// Returns the number of bytes copied, or negative on failure — and on failure
// THE PREVIOUS CLIPBOARD SURVIVES UNTOUCHED (the kernel refuses a copy over
// its 16MB ceiling rather than storing a truncated one).
//
// A length of 0 is a legitimate copy: it CLEARS the clipboard, exactly as
// `> /sys/clipboard` with no output would.
int64_t os64_clip_copy(const void *bytes, uint64_t length);

// Paste into `buf`, at most `cap` bytes. Returns the clipboard's TRUE length
// — which may be MORE than `cap`, and that is the useful part: the caller
// learns it was truncated and can size a buffer and ask again. (The snprintf
// convention; os64_clip_length() is the same question asked without a read.)
// 0 means the clipboard is empty. Negative means the paste failed.
//
// `buf` may be NULL with cap 0 to ask only for the length.
int64_t os64_clip_paste(void *buf, uint64_t cap);

// How many bytes are on the clipboard right now, without reading them.
// Negative if the clipboard could not be reached at all.
int64_t os64_clip_length(void);

#endif
