#ifndef OS64_IO_H
#define OS64_IO_H

// libos64 raw I/O over kernel handles (LIBOS64.md layer). A handle is a small
// int index into the per-task handle table; 1/2 are the conventional console
// out/err until the table and real file handles land. Buffered FILE* I/O will
// layer OVER this — never compete with it.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>                // os64_linereader_t carries an eof flag
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

// os64_read with a patience limit (the poll/deadline gait; full contract at
// SYSCALL_READ in os64/syscall_numbers.h). timeout_ms means what it says:
//   0                 — never blocks: bytes if some are ready, else
//                       OS64_ERR_TIMEOUT immediately. This is how top
//                       watches for 'q' between refreshes:
//                           char c;
//                           if (os64_read_for(0, &c, 1, 0) == 1 && c == 'q')
//                               break;
//   N                 — up to N ms, then OS64_ERR_TIMEOUT if still byteless:
//                           while ((n = os64_read_for(h, buf, sizeof buf, 1000))
//                                  != OS64_ERR_TIMEOUT)
//                               { ...process reply... }   // ping's whole loop
//   OS64_WAIT_FOREVER — identical to os64_read.
// OS64_ERR_TIMEOUT is its own verdict, never 0 — an empty poll can never
// impersonate end-of-input (the V7 O_NDELAY confusion, refused by design).
// Its true name lives in <os64/syscall_numbers.h>; OS64_NET_ERR_TIMEOUT in
// the dial table is an alias for the same value.
//
// This is the deadline Unix never gave read() — 4.2BSD bolted select() on
// beside it instead (1983); os64 puts the patience where the question is
// asked. Born as ping's demand (silence needed a return value), and 0 meant
// FOREVER for its first few weeks — SO_RCVTIMEO's wart — until the console
// learned patience and top needed zero's honest meaning for its 'q' key.
//
// Granularity is the scheduler tick (10ms), so a timeout_ms of 1..10 is one
// tick of patience, not a microsecond fuse. HONORED BY: the console, and
// dialed net handles (udp/tcp/icmp) — the two branches that grew this
// independently, joined at the merge of 2026-08-05. Every other handle
// REFUSES a finite patience (negative return) rather than silently
// blocking, until a real consumer earns it there.
int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t timeout_ms);

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

// ── os64_linereader — buffered line reading for FILES (2026-08-06) ──────────
//
// os64_readline above is priced for SMALL files: its seek-back honesty costs
// one backward seek per line, and its chunk is re-read from disk each call.
// The day grep met a 46MB log, both bills came due at once — on FAT a
// backward seek walks the cluster chain from the START of the file
// (O(position) per line, O(n²) per file: 4,320 lines in sixty seconds, and
// decelerating), and on ext2 the re-reads hammered the same disk blocks
// hundreds of times each, because os64 has no page cache to absorb them.
// One disease, two pathologies; the cure is the one Mike Lesk shipped in
// 1976 as the portable I/O library (stdio to you): read big chunks ONCE,
// forward only, dispense lines from memory, never seek.
//
// The contract is os64_readline's exactly — same 1/0/negative returns, same
// \r\n handling, same truncate-and-consume rule for over-long lines — so a
// readline loop converts by swapping the open and the call. The reader OWNS
// its handle: it reads ahead, so the handle's position is meaningless to
// anyone else; get lines from the reader or bytes from your own handle,
// never both. For pipes/console keep os64_readline — bytewise is CORRECT
// there, and 64KB of read-ahead would steal the next reader's input.
//
//     os64_linereader_t lr;
//     if (os64_linereader_open(&lr, path) < 0) ...;
//     while (os64_linereader_line(&lr, line, sizeof line) == 1)
//         ...;
//     os64_linereader_close(&lr);
typedef struct {
    int32_t handle;    // owned by the reader from open to close
    char   *chunk;     // the read-ahead buffer (os64_map'd)
    size_t  cap;       // its capacity
    size_t  len;       // valid bytes currently in it
    size_t  pos;       // consumption cursor into it
    bool    eof;       // the underlying file has no more bytes
} os64_linereader_t;

int64_t os64_linereader_open(os64_linereader_t *lr, const char *path);
int64_t os64_linereader_line(os64_linereader_t *lr, char *buf, size_t cap);
void    os64_linereader_close(os64_linereader_t *lr);

// Open the file at `path` (absolute, on the root filesystem) and return a
// handle, or negative on error (no such file, bad mode, out of handles).
// `mode` is a one-letter string:
//   "r"  read what exists            "u"  update what exists (read/write)
//   "a"  append to what exists
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

// A handle on YOUR controlling terminal — whatever handle 0 has become.
// Returns the handle (>= 3), or negative if the handle table is full.
//
// This is the answer to the oldest question a pipeline asks: `ps | less` puts
// a pipe on the pager's handle 0, but a pager needs its DOCUMENT from that
// pipe and its KEYS from the terminal, and redirection can only aim one slot
// at one thing. Unix opens /dev/tty for this; os64 asks for it directly, and
// a devfs later makes /dev/tty a NAME for this call rather than a rival to it.
//
//     int64_t keys = os64_tty_handle();
//     if (keys < 0) keys = OS64_STDIN;     // no room; stdin is the fallback
//     char c;
//     os64_read(keys, &c, 1);              // the next key typed at MY terminal
//
// Ask for it unconditionally rather than only when stdin looks redirected:
// when stdin already IS the terminal, this handle reads the very same input,
// so the behavior is identical and you skip needing an isatty() you don't have.
//
// WHAT IT IS: a second reference to one shared input ring, not a private copy.
// A byte read here is gone from handle 0 if handle 0 is the terminal too.
// Reads are ordinary reads — blocking, SHORT (you get what's available, not a
// filled buffer), 0 at Ctrl+D, and os64_read_for's timeout works on it. Ctrl+C
// never arrives as a byte; it becomes a signal aimed at the terminal's
// foreground task. A background job reads EOF here just as it does on handle 0.
//
// WHICH terminal: yours, resolved at every read. On a virtual terminal that is
// the keyboard while your VT holds the glass — type at another VT and you hear
// nothing, which is the same rule stdin has always followed. Inside a terminal
// window it is that window's pty slave, fed by the terminal app. Your program
// cannot tell the difference and has no reason to want to.
//
// Close it with os64_close like any handle.
int64_t os64_tty_handle(void);

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

// Sync EVERYTHING: every open file of every task, on every mount — the
// engine sync(1) is built on, doing since 1971 what os64_sync above does
// for one handle. This is the only way to make ANOTHER program's still-open
// file readable at true length (only the kernel can reach its handles);
// the canonical customer is `cat ping.log` while ping is still running.
// Returns the number of files synced (0 on an idle system is honest, not
// an error), negative if any individual sync failed.
int64_t os64_sync_all(void);

// The system is going down: the kernel runs the entire descent — retires
// the log daemon, syncs every open file everywhere (the os64_sync_all
// above, done for the last time), flushes the drives' own volatile caches
// (the tier no sync can reach — the reason "sync then power button" was
// always a ritual rather than a guarantee), and powers off. Does not
// return; there is nothing to come back to. See SYSCALL_SHUTDOWN in
// syscall_numbers.h for the order and the lineage.
// `mode` is OS64_SHUTDOWN_POWEROFF or OS64_SHUTDOWN_REBOOT
// (<os64/syscall_numbers.h>, shared with the kernel so neither side owns a
// private copy of what "1" means). ONE call, because the two endings share
// every step but the last instruction — a separate reboot() would be a second
// name for the same descent.
//
// NOTE for anything that wants to be polite about it: the kernel's ladder is
// SIGTERM → grace → SIGKILL, and the grace is short (SHUTDOWN_GRACE_MS,
// CONFIG.h) because by the time this call is made the decision is already
// taken. A countdown for the HUMAN — "going down in 30 seconds" — belongs in
// the utility, above this call, where a person can still stop it.
void os64_shutdown(os64_shutdown_mode_t mode) __attribute__((noreturn));

// Remove a file OR an empty directory (relative paths resolve against the
// cwd) — os64's one removal verb; there is no rmdir, by design (Plan 9's
// remove(), not POSIX's split — see the ABI header for the history). 0 on
// success, negative on failure: a read-only filesystem, no such path, or a
// directory that still has contents. This is the call `rm` is built on —
// its -r is just this verb applied depth-first.
int64_t os64_unlink(const char *path);

// Rename `oldpath` to `newpath` (relative paths resolve against the cwd).
// Both must live on the SAME filesystem — moving between mounts is a copy,
// and belongs in userland where a half-finished one can be cleaned up.
//
// With flags zero, replacement follows the filesystem's legacy behavior:
// ext2 replaces atomically, while FAT removes the destination before moving
// the source and therefore has a window where neither name exists. Refuses
// (negative) rather than surprising you: read-only filesystem,
// missing source, cross-filesystem, a directory on either side of a
// replacement, an open DIRECTORY on either side, or a directory moved into
// its own descendant. Open FILES are fine on both sides — replacing one that
// is being read (or RUN) leaves the reader's copy alive until it closes.
// This is the call `mv` is built on — within one filesystem.
int64_t os64_rename(const char *oldpath, const char *newpath);

// The opt-in safe-publish policies. OS64_RENAME_NOREPLACE atomically refuses
// when newpath exists. OS64_RENAME_REQUIRE_ATOMIC_REPLACE replaces an existing
// destination only on a filesystem that can do so without unlinking it first;
// FAT therefore refuses that case and preserves both files. The two flags are
// mutually exclusive, and unknown combinations are refused.
int64_t os64_rename_with_flags(const char *oldpath, const char *newpath,
                               uint64_t flags);

// Mount a partition into the namespace, or take a mount out of it. `what`
// names the PARTITION — its GPT name ("home") or dashed GUID — never a
// device path; `where` is the mount point (its parent must exist; the point
// itself need not — the mount table is the namespace at that level). A NULL
// `where` mounts at the partition's own GPT label, exactly as a one-token
// mounts.conf line does.
// `flags` is the OS64_MOUNT_* bits — OS64_MOUNT_RO mounts with every write
// verb refused from birth; an unknown bit is refused as BAD_ARGS.
// Returns 0 or a negative os64/mount.h code that says WHICH refusal:
// OS64_UNMOUNT_BUSY and OS64_MOUNT_NOT_FOUND demand different next moves,
// so the number carries the difference. /sys/mounts and /sys/block are the
// eyes; these two are the hands.
int64_t os64_mount(const char *what, const char *where, uint64_t flags);
int64_t os64_unmount(const char *where);

// Create a directory at `path` (relative paths resolve against the cwd).
// 0 on success, negative on failure: a read-only filesystem (ext2, by
// design), a parent that doesn't exist, or a name already taken. Atomic —
// one call, unlike the three-step setuid dance early Unix made of it.
// This is the call `mkdir` is built on; its undo is os64_unlink above —
// the one removal verb covers empty directories, so no rmdir twin needed.
int64_t os64_mkdir(const char *path);

// Convenience: write a NUL-terminated string to the console (handle 1).
int64_t os64_puts(const char *s);

// Write `s` to `handle` with every control byte and backslash spelled as a
// C escape — `\n`, `\t`, `\r`, `\\`, and `\xHH` for the rest — so the bytes
// that would move a cursor arrive as glyphs instead. For LISTINGS whose
// reader is a human or a grep: one entry per line only holds while no
// entry can break a line, and a value captured from `ls` by $(...) carries
// exactly the newlines that would. Returns 0, or the negative verdict of
// the write that failed.
int64_t os64_write_escaped(int32_t handle, const char *s);

// Park a string at an absolute character cell (x, y) on the PHYSICAL console
// — the SCREEN LAYER (née "the widget plane": renamed 2026-08-19 when libui
// claimed the word widget for something else, and the day the layer's whole
// doctrine got proven from the other side — a clock started inside a gterm
// session showed up on the text VTs and nowhere near the window, exactly as
// designed and exactly as surprising as an undocumented-at-the-call-site
// design always is; the identifier carries the doctrine now). This is
// NOT a console write: no cursor motion, no wrap, no scroll, and the string
// clips at the screen edge instead of reflowing anybody's prompt. It draws on
// the machine's actual glass, outside the virtual-terminal stack —
// a VT switch won't disturb it, it deliberately doesn't exist over a
// remote session OR inside a pty window (sessions show session output; the
// machine's glass shows the machine's), and the GUI owning the iron
// suppresses it entirely. Terminal content (full-screen repaints, anything
// that should survive a pipe) is the escape-sequence slice's job, not this.
// Last writer to a cell wins; overlays that share a corner deserve each other.
// Returns 0, or negative (bad coordinates / unreadable string).
int64_t os64_screen_printat(uint32_t x, uint32_t y, const char *s);

// (os64_exit lived here for one glorious scaffolding week — exit is process
// control, not I/O, and it moved home to <os64/proc.h> the day its owner
// went looking for it there and rolled his eyes. 🙄)

// Diagnostic: put a line in the kernel LOG (prefixed "[user] "). Distinct
// from os64_puts (which goes to the console). Where the line ends up follows
// the log itself: the serial wire on a plain boot, or a logd's FILE once a
// daemon has claimed the log — which a headless harness can't read until
// shutdown. If the line's whole job is to be seen from OUTSIDE, live, use
// os64_serial_log below.
void os64_debug_log(const char *s);

// A COMPLAINT A HUMAN IS MEANT TO FIND: formatted, and sent to stderr AND
// the kernel log. A GUI program's stderr is the console it was spawned from,
// which on a GUI boot is a VT nobody is looking at — so a diagnosis printed
// only there is one nobody reads, and the program looks like it failed for
// no reason. Reach for this wherever a program refuses, gives up, or hits a
// ceiling it wants to name. A trailing newline is optional: stderr gets the
// line as written, the log gets it without one.
void os64_complain(const char *fmt, ...);

// The beacon: same log line, but ALSO written directly to the serial wire,
// immediately, no matter who has claimed the log — the same door panic()
// uses. For the handful of markers a verification harness greps for while
// the OS runs (husk's rc breadcrumb, a fixture's checkpoint). Not for
// logging: every call costs a VM exit, which is the exact bill logd exists
// to retire.
void os64_serial_log(const char *s);

#endif // OS64_IO_H
