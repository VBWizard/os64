#ifndef OS64_IO_H
#define OS64_IO_H

// libos64 raw I/O over kernel handles (LIBOS64.md layer). A handle is a small
// int index into the per-task handle table; 1/2 are the conventional console
// out/err until the table and real file handles land. Buffered FILE* I/O will
// layer OVER this — never compete with it.

#include <stddef.h>
#include <stdint.h>
#include "os64/syscall_numbers.h"   // OS64_SEEK_* — the seek() vocabulary
#include "os64/dirent.h"            // os64_dirent_t — what readdir() delivers

// The three standard streams, named — because os64_hprintf(2, ...) made two
// of this OS's owners say "shame on both of us" in the same afternoon. The
// numbers are ABI (they'll never change); the names are for reading.
#define OS64_STDIN   0
#define OS64_STDOUT  1
#define OS64_STDERR  2

// Write `len` bytes of `buf` to `handle`. Returns bytes written, or a
// negative value on error (the in-band status half of the ABI; no errno).
long os64_write(int handle, const void *buf, size_t len);

// Read up to `len` bytes from `handle` into `buf`. Blocks until at least one
// byte is available, then returns the count read (>= 1), or negative on error.
// handle 0 (stdin) reads the console keyboard — UNLESS the shell redirected it
// to a pipe, which the program neither knows nor cares about.
//
// Returns SHORT: you get what is available now, not a filled buffer. Returns
// 0 at END OF INPUT (all writers on the other end of the pipe have closed).
// That 0 is how a filter knows its input is finished — the canonical loop is:
//     while ((n = os64_read(0, buf, sizeof buf)) > 0) { ...process n bytes... }
long os64_read(int handle, void *buf, size_t len);

// Open the file at `path` (absolute, on the root filesystem) and return a
// handle, or negative on error (no such file, bad mode, out of handles).
// `mode` is a one-letter string:
//   "r"  read what exists            "a"  append to what exists
//   "w"/"c"  create (or truncate) for writing
// Pass NULL for "r" — reading is what almost every open is.
//
// The handle you get back is the SAME species as 0/1/2 and pipe ends: it
// plugs into os64_read/os64_write/os64_close unchanged, and into
// os64_spawn_redirected — put a file handle in a child's slot 0 and you have
// `program < file` with zero new mechanism. A file read returns short counts
// near the end and 0 AT the end, so the canonical filter loop needs no
// file-awareness whatsoever. (That is the cat model working as designed.)
long os64_open(const char *path, const char *mode);

// Move a file handle's position. `whence` says what `offset` is measured
// from: OS64_SEEK_SET (start), OS64_SEEK_CUR (here), OS64_SEEK_END (end —
// offset 0 gives the file's size, negative offsets back up from it).
// Returns the NEW absolute position, or negative on error. Only files have
// a position — seeking a pipe or the console is an error.
long os64_seek(int handle, long offset, int whence);

// Open the DIRECTORY at `path` for listing. Returns a handle for
// os64_readdir(), released with plain os64_close() — a directory is just
// another thing a handle can be. (Under the hood this is os64_open with
// mode "d": one open, one handle table, one close.)
long os64_opendir(const char *path);

// Produce the next entry of an open directory: name, size, and an
// OS64_DE_DIR flag in one call (see <os64/dirent.h> — no follow-up stat
// needed to know what you're looking at). Returns:
//   1  — *entry was filled in
//   0  — end of directory (calling again keeps returning 0)
//  <0  — error (bad handle, or the handle isn't a directory)
//
// The canonical listing loop — the spine of ls:
//     long d = os64_opendir("/bin");
//     os64_dirent_t e;
//     while (os64_readdir(d, &e) == 1)
//         ...e.name, e.size, (e.flags & OS64_DE_DIR)...
//     os64_close(d);
long os64_readdir(int handle, os64_dirent_t *entry);

// Create a pipe. h[0] = read end, h[1] = write end. Returns 0, or negative.
//
// You now hold BOTH ends. Hand one to each child via os64_spawn_redirected()
// and then CLOSE YOUR OWN TWO COPIES — this is not optional bookkeeping. The
// reader on the far end sees end-of-input only when the LAST write end closes,
// so a shell that keeps its copy of the write end open leaves the reader
// waiting forever for an EOF that can never come. That is the single most
// common way a hand-written shell hangs.
long os64_pipe(int h[2]);

// Give up a handle. For a pipe end this is SIGNALLING, not bookkeeping:
// dropping the last write end is what delivers end-of-input to the reader, and
// dropping the last read end is what kills a writer that is producing into the
// void.
long os64_close(int handle);

// Convenience: write a NUL-terminated string to the console (handle 1).
long os64_puts(const char *s);

// (os64_exit lived here for one glorious scaffolding week — exit is process
// control, not I/O, and it moved home to <os64/proc.h> the day its owner
// went looking for it there and rolled his eyes. 🙄)

// Diagnostic: print a string to the kernel serial log (prefixed "[user] ").
// Distinct from os64_puts (which goes to the console) — this is for test
// output an offscreen/headless run can capture.
void os64_debug_log(const char *s);

#endif // OS64_IO_H
