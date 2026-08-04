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
int64_t os64_write(int32_t handle, const void *buf, size_t len);

// Read up to `len` bytes from `handle` into `buf`. Blocks until at least one
// byte is available, then returns the count read (>= 1), or negative on error.
// handle 0 (stdin) reads the console keyboard — UNLESS the shell redirected it
// to a pipe, which the program neither knows nor cares about.
//
// Returns SHORT: you get what is available now, not a filled buffer. Returns
// 0 at END OF INPUT (all writers on the other end of the pipe have closed).
// That 0 is how a filter knows its input is finished — the canonical loop is:
//     while ((n = os64_read(0, buf, sizeof buf)) > 0) { ...process n bytes... }
int64_t os64_read(int32_t handle, void *buf, size_t len);

// Read one LINE from `handle` into `buf` (cap bytes INCLUDING the
// terminator): everything up to and including the next '\n', with the line
// ending stripped — both '\n' and "\r\n", so a file that once passed through
// Windows reads the same as one that never did. Always NUL-terminates.
// Returns:
//   1  — *buf holds a line (possibly empty — a blank line is still a line)
//   0  — end of input (a final line with no trailing newline is delivered
//        as a line first; THEN you get the 0)
//  <0  — error from the underlying read (bad handle, etc.)
//
// The same shape as os64_readdir, on purpose: a directory produces entries,
// a text file produces lines, and the loop is the same species — the spine
// of parsing any /proc file:
//     char line[256];
//     int64_t h = os64_open("/proc/7/status", NULL);
//     while (os64_readline(h, line, sizeof line) == 1)
//         ...line is "key\tvalue", parse in place...
//     os64_close(h);
//
// A line longer than cap-1 is delivered truncated and the REST OF THE LINE
// IS CONSUMED: the next call returns the next line, never the severed tail
// of this one. (Getting the tail of a long line served back as a bonus
// "line" is the classic fgets footgun — size the buffer for the longest
// line you believe in, and the loop stays honest either way.)
//
// Cost: on a FILE, a line is a few syscalls regardless of length — a chunk
// is read and the surplus past the newline is SEEKED BACK, so the position
// lands exactly after the '\n' (mixing readline with raw read()/seek() on
// the same handle stays coherent). On a PIPE or the CONSOLE — which cannot
// seek backward — it reads one byte per syscall, because a chunk would
// STEAL bytes that belong to whoever reads the handle next. The Bourne
// shell has read its input a byte at a time since 1977 for exactly this
// reason — it is why `read` inside a shell script works instead of the
// script eating the data. The gait is picked per call by asking the handle
// whether it can seek; callers never care.
int64_t os64_readline(int32_t handle, char *buf, size_t cap);

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
int64_t os64_open(const char *path, const char *mode);

// Move a file handle's position. `whence` says what `offset` is measured
// from: OS64_SEEK_SET (start), OS64_SEEK_CUR (here), OS64_SEEK_END (end —
// offset 0 gives the file's size, negative offsets back up from it).
// Returns the NEW absolute position, or negative on error. Only files have
// a position — seeking a pipe or the console is an error.
int64_t os64_seek(int32_t handle, int64_t offset, int32_t whence);

// Open the DIRECTORY at `path` for listing. Returns a handle for
// os64_readdir(), released with plain os64_close() — a directory is just
// another thing a handle can be. (Under the hood this is os64_open with
// mode "d": one open, one handle table, one close.)
int64_t os64_opendir(const char *path);

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
int64_t os64_readdir(int32_t handle, os64_dirent_t *entry);

// stat is readdir for exactly one name: fill *entry for whatever `path`
// names — file OR directory — without opening it. Same os64_dirent_t that
// readdir yields (one vocabulary for "an entry"; POSIX's separate struct
// stat was a fork os64 declines). Relative paths resolve against cwd.
// Returns 0 with *entry filled, or negative if nothing lives at `path`.
//
// The question it answers is "what is this?" — the spine of `ls file`:
//     os64_dirent_t e;
//     if (os64_stat(arg, &e) < 0)        ...no such thing...
//     else if (e.flags & OS64_DE_DIR)    ...opendir/readdir loop...
//     else                               ...print e.name, e.size directly...
int64_t os64_stat(const char *path, os64_dirent_t *entry);

// Create a pipe. h[0] = read end, h[1] = write end. Returns 0, or negative.
//
// You now hold BOTH ends. Hand one to each child via os64_spawn_redirected()
// and then CLOSE YOUR OWN TWO COPIES — this is not optional bookkeeping. The
// reader on the far end sees end-of-input only when the LAST write end closes,
// so a shell that keeps its copy of the write end open leaves the reader
// waiting forever for an EOF that can never come. That is the single most
// common way a hand-written shell hangs.
int64_t os64_pipe(int32_t h[2]);

// Give up a handle. For a pipe end this is SIGNALLING, not bookkeeping:
// dropping the last write end is what delivers end-of-input to the reader, and
// dropping the last read end is what kills a writer that is producing into the
// void.
int64_t os64_close(int32_t handle);

// Commit a written file to the device — its bytes AND the directory entry
// recording its new length. Until this (or a close) happens, a file you
// are appending to reads as EMPTY to every other program, because that is
// where FAT keeps the length. Returns 0, or negative for a handle that
// isn't a file. A program that writes a file others read WHILE it holds
// it open — a log daemon, a status file — needs this; a program that
// writes and closes does not.
int64_t os64_sync(int32_t handle);

// Remove a file OR an empty directory (relative paths resolve against the
// cwd) — os64's one removal verb; there is no rmdir, by design (Plan 9's
// remove(), not POSIX's split — see the ABI header for the history). 0 on
// success, negative on failure: a read-only filesystem, no such path, or a
// directory that still has contents. This is the call `rm` is built on —
// its -r is just this verb applied depth-first.
int64_t os64_unlink(const char *path);

// Create a directory at `path` (relative paths resolve against the cwd).
// 0 on success, negative on failure: a read-only filesystem (ext2, by
// design), a parent that doesn't exist, or a name already taken. Atomic —
// one call, unlike the three-step setuid dance early Unix made of it.
// This is the call `mkdir` is built on; its undo is os64_unlink above —
// the one removal verb covers empty directories, so no rmdir twin needed.
int64_t os64_mkdir(const char *path);

// Convenience: write a NUL-terminated string to the console (handle 1).
int64_t os64_puts(const char *s);

// Park a string at an absolute character cell (x, y) on the PHYSICAL console
// — the widget plane, for status widgets like clock's corner readout. This is
// NOT a console write: no cursor motion, no wrap, no scroll, and the string
// clips at the screen edge instead of reflowing anybody's prompt. It draws on
// the machine's actual glass, outside the (future) virtual-terminal stack —
// a VT switch won't disturb it, and it deliberately doesn't exist over a
// remote session. Terminal content (full-screen repaints, anything that
// should survive a pipe) is the escape-sequence slice's job, not this.
// Last writer to a cell wins; widgets that share a corner deserve each other.
// Returns 0, or negative (bad coordinates / unreadable string).
int64_t os64_printat(uint32_t x, uint32_t y, const char *s);

// (os64_exit lived here for one glorious scaffolding week — exit is process
// control, not I/O, and it moved home to <os64/proc.h> the day its owner
// went looking for it there and rolled his eyes. 🙄)

// Diagnostic: print a string to the kernel serial log (prefixed "[user] ").
// Distinct from os64_puts (which goes to the console) — this is for test
// output an offscreen/headless run can capture.
void os64_debug_log(const char *s);

#endif // OS64_IO_H
