// libos64 io.c — the friendly veneer over the raw write/exit syscalls.

#include <stdbool.h>       // os64_readline's flags — a .c includes what it
                           // uses too, not what its neighbors happen to drag in
#include <stdarg.h>        // os64_complain's varargs
#include "os64/io.h"
#include "os64/fmt.h"      // os64_complain formats before it splits the line
                           // between stderr and the log
#include "os64/str.h"      // os64_strlen — was a private static here until
                           // env.c needed string helpers too and it graduated
#include "os64/syscall.h"

int64_t os64_write(int32_t handle, const void *buf, size_t len)
{
    return (long)os64_syscall3(SYSCALL_WRITE, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len);
}

int64_t os64_read(int32_t handle, void *buf, size_t len)
{
    // The explicit OS64_WAIT_FOREVER is load-bearing: arg3 is the read's
    // PATIENCE (abi syscall_numbers.h, ruled 2026-08-05 — 0 now means POLL,
    // not forever), and before this stub passed it deliberately, that
    // register was ring-3 garbage the kernel ignored. A rebuilt world
    // always states its patience out loud rather than leaving it to
    // whatever was in r10.
    return (long)os64_syscall4(SYSCALL_READ, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len,
                               OS64_WAIT_FOREVER);
}

int64_t os64_read_for(int32_t handle, void *buf, size_t len, uint64_t timeout_ms)
{
    return (long)os64_syscall4(SYSCALL_READ, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len, timeout_ms);
}

// The two line-reading gaits behind os64_readline (public contract in io.h).
// Identical semantics; they differ only in what a line COSTS:
//
//   bytewise — one byte per read(). The only correct gait for a handle that
//   cannot seek (pipe, console): reading a chunk would steal bytes belonging
//   to whoever reads the handle next, with no way to put them back.
//
//   seekable — read a chunk, find the newline, seek BACK the surplus so the
//   position lands exactly after the '\n'. A few syscalls per line instead
//   of one per byte. Files can always give bytes back; that is what a
//   position IS. (This gait exists because the first top spent ~3 seconds
//   byte-reading 30 /proc files — Chris's diagnosis, 2026-07-28.)

static int64_t readline_bytewise(int32_t handle, char *buf, size_t cap)
{
    size_t stored = 0;        // bytes actually placed in buf (never > cap-1)
    bool sawAny = false;      // did this call consume ANY byte? ("\n" alone
                              // is an empty line — distinct from end of input)
    bool hitNewline = false;
    bool dropped = false;     // over-long line: bytes consumed but not stored

    while (!hitNewline)
    {
        char c;
        int64_t n = os64_read(handle, &c, 1);
        if (n < 0)
            return n;         // the underlying read's error, passed through
        if (n == 0)
            break;            // end of input — deliver what we hold, if anything
        sawAny = true;
        if (c == '\n')
            hitNewline = true;
        else if (stored + 1 < cap)
            buf[stored++] = c;
        else
            dropped = true;   // past cap-1 — consumed so the NEXT call starts
                              // at the next line, never this line's severed tail
    }

    // "\r\n" is one line ending: strip the '\r' — but only when it was the
    // byte immediately before the '\n' AND it made it into buf. If bytes
    // were dropped, buf's last byte is interior data that merely happens to
    // be '\r'; stripping it would eat a data byte.
    if (hitNewline && !dropped && stored > 0 && buf[stored - 1] == '\r')
        stored--;

    buf[stored] = '\0';

    return sawAny ? 1 : 0;
}

static int64_t readline_seekable(int32_t handle, char *buf, size_t cap)
{
    size_t stored = 0;
    bool sawAny = false;
    bool hitNewline = false;
    bool dropped = false;

    while (!hitNewline)
    {
        // Read into the caller's buffer while it has room; once an over-long
        // line fills it, keep consuming the tail through a small local chunk
        // (same truncation contract as bytewise: the rest of the line is
        // consumed, not delivered).
        char waste[64];
        char *dst;
        size_t room;
        if (stored + 1 < cap)
        {
            dst = buf + stored;
            room = cap - 1 - stored;
        }
        else
        {
            dst = waste;
            room = sizeof(waste);
        }

        int64_t n = os64_read(handle, dst, room);
        if (n < 0)
            return n;
        if (n == 0)
            break;
        sawAny = true;

        int64_t nl = -1;
        for (int64_t i = 0; i < n; i++)
            if (dst[i] == '\n') { nl = i; break; }

        if (nl < 0)
        {
            // No newline yet: keep what landed in buf, note what didn't.
            if (dst == waste)
                dropped = true;
            else
                stored += (size_t)n;
            continue;
        }

        // Newline found. Give back everything after it — the seek-back is
        // what makes chunk-reading honest: the handle's position ends up
        // exactly where byte-at-a-time reading would have left it, so mixing
        // readline with raw read()/seek() on the same handle stays sane.
        int64_t surplus = n - (nl + 1);
        if (surplus > 0 && os64_seek(handle, -surplus, OS64_SEEK_CUR) < 0)
            return -1;    // reads but won't seek back? refuse to lose data silently

        if (dst == waste)
        {
            if (nl > 0)
                dropped = true;   // tail bytes before the newline, all dropped
        }
        else
            stored += (size_t)nl;
        hitNewline = true;
    }

    // Same "\r\n" rule as bytewise, same drop guard.
    if (hitNewline && !dropped && stored > 0 && buf[stored - 1] == '\r')
        stored--;

    buf[stored] = '\0';

    return sawAny ? 1 : 0;
}

int64_t os64_readline(int32_t handle, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0)
        return -1;

    // One method, two gaits. The probe asks the only question that matters:
    // "can this handle give bytes back?" A file answers with its position;
    // a pipe or the console refuses, and gets the byte-at-a-time gait that
    // never consumes a byte it doesn't deliver.
    if (os64_seek(handle, 0, OS64_SEEK_CUR) >= 0)
        return readline_seekable(handle, buf, cap);

    return readline_bytewise(handle, buf, cap);
}

int64_t os64_puts(const char *s)
{
    return os64_write(SYSCALL_HANDLE_CONSOLE_OUT, s, os64_strlen(s));
}

int64_t os64_write_escaped(int32_t handle, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    // Batched through a small buffer: one write per 256 bytes rather than
    // one per escape. An escape is at most four bytes, which is the flush
    // margin below.
    char out[256];
    size_t n = 0;

    for (; *s != '\0'; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (n + 4 > sizeof(out))
        {
            int64_t r = os64_write(handle, out, n);
            if (r < 0)
                return r;
            n = 0;
        }
        switch (c)
        {
        case '\n': out[n++] = '\\'; out[n++] = 'n';  break;
        case '\t': out[n++] = '\\'; out[n++] = 't';  break;
        case '\r': out[n++] = '\\'; out[n++] = 'r';  break;
        case '\\': out[n++] = '\\'; out[n++] = '\\'; break;
        default:
            if (c < 0x20 || c == 0x7f)
            {
                out[n++] = '\\';
                out[n++] = 'x';
                out[n++] = hex[c >> 4];
                out[n++] = hex[c & 0xf];
            }
            else
                out[n++] = (char)c;
        }
    }
    if (n > 0)
    {
        int64_t r = os64_write(handle, out, n);
        if (r < 0)
            return r;
    }
    return 0;
}

// The screen layer's one verb (see io.h for the doctrine and the rename
// story). The SYSCALL number and its "printat" table name stay — the wire
// is stable; only the identifier learned to say which layer it addresses.
int64_t os64_screen_printat(uint32_t x, uint32_t y, const char *s)
{
    return (int64_t)os64_syscall3(SYSCALL_PRINTAT, (uint64_t)x, (uint64_t)y,
                                  (uint64_t)s);
}

// flags = 0, EXPLICITLY: the kernel reads arg1 as the flags word now, and a
// syscall1 stub would let whatever garbage ring 3 left in that register
// randomly promote a log line to a serial beacon.
void os64_debug_log(const char *s)
{
    os64_syscall2(SYSCALL_DEBUG_LOG, (uint64_t)s, 0);
}

void os64_complain(const char *fmt, ...)
{
    // Sized to os64_printf's own ceiling, so a complaint is no more lossy
    // than any other line. It matters here because the actionable half of a
    // diagnostic is its SUFFIX — the reason — and one path can be
    // OS64_CONF_PATH_MAX by itself.
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    os64_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    os64_hprintf(OS64_STDERR, "%s", line);
    // The log supplies its own line ending, so hand it the text without one.
    size_t n = os64_strlen(line);
    if (n && line[n - 1] == '\n')
        line[n - 1] = 0;
    os64_debug_log(line);
}

// The beacon variant: same log line, but ALSO written to the serial wire
// immediately and directly, past any logd claim. See io.h for when this is
// the right call (rarely).
void os64_serial_log(const char *s)
{
    os64_syscall2(SYSCALL_DEBUG_LOG, (uint64_t)s, OS64_DEBUG_LOG_SERIAL);
}

int64_t os64_open(const char *path, const char *mode)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)mode);
}

int64_t os64_seek(int32_t handle, int64_t offset, int32_t whence)
{
    return (long)os64_syscall3(SYSCALL_SEEK, (uint64_t)(int64_t)handle,
                               (uint64_t)offset, (uint64_t)(int64_t)whence);
}

int64_t os64_opendir(const char *path)
{
    return (long)os64_syscall2(SYSCALL_OPEN, (uint64_t)path, (uint64_t)"d");
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    return (long)os64_syscall2(SYSCALL_STAT, (uint64_t)path, (uint64_t)entry);
}

int64_t os64_readdir(int32_t handle, os64_dirent_t *entry)
{
    return (long)os64_syscall2(SYSCALL_READDIR, (uint64_t)(int64_t)handle,
                               (uint64_t)entry);
}

int64_t os64_pipe(int32_t h[2])
{
    return (long)os64_syscall1(SYSCALL_PIPE, (uint64_t)h);
}

int64_t os64_tty_handle(void)
{
    return (int64_t)os64_syscall0(SYSCALL_TTY_HANDLE);
}

int64_t os64_close(int32_t handle)
{
    return (long)os64_syscall1(SYSCALL_CLOSE, (uint64_t)(int64_t)handle);
}

int64_t os64_sync(int32_t handle)
{
    return (long)os64_syscall1(SYSCALL_SYNC, (uint64_t)(int64_t)handle);
}

int64_t os64_sync_all(void)
{
    return (int64_t)os64_syscall0(SYSCALL_SYNC_ALL);
}

void os64_shutdown(os64_shutdown_mode_t mode)
{
    // The explicit 0 is arg0 = the VERB (0 = power off; see
    // syscall_numbers.h). Passing it explicitly was foresight when this was
    // written and it PAID on 2026-08-21: verb 1 became real that day, and
    // every binary already built kept halting instead of flipping a coin,
    // because none of them ever left register garbage where the verb goes.
    os64_syscall1(SYSCALL_SHUTDOWN, (uint64_t)mode);
    // The kernel never comes back from that call — but the compiler can't
    // know it from a syscall stub, and noreturn is a promise we must keep
    // even if the impossible happens.
    for (;;) {}
}

// Remove a file or an empty directory. Relative paths resolve against the
// cwd, like open's.
//
// Named for what actually happens rather than for the program that calls it:
// the directory entry is unlinked and the storage follows — for directories
// too, which is why os64 has no rmdir. Returns 0 on success, negative on
// failure — a read-only filesystem (os64's ext2), a path that isn't there,
// or a directory that isn't empty.
int64_t os64_unlink(const char *path)
{
    return (long)os64_syscall1(SYSCALL_UNLINK, (uint64_t)path);
}

// Give a file a different name, possibly in a different directory of the
// same filesystem. Relative paths resolve against the cwd, like open's.
//
// The guarantee worth knowing: replacing an existing regular file happens in
// ONE motion — `newpath` never stops resolving, not even for an instant.
// That is what makes the safe-publish recipe work:
//
//     h = os64_open("report.part", "w");   ...write, verify...
//     os64_rename("report.part", "report");
//
// A crash anywhere in that sequence leaves either the old `report` intact or
// the new one complete, never a half-written impostor wearing the name. Unix
// had no such call for its first decade (link-then-unlink, with the window
// in the middle); 4.2BSD added rename(2) to close it, and this is that.
//
// Returns 0 on success, negative on refusal: a read-only filesystem, a
// source that isn't there, the two paths on DIFFERENT filesystems (that's a
// copy, not a rename — do it in userland), a destination that is a directory
// or that a directory would replace, an open DIRECTORY on either side, or a
// directory renamed into its own descendant. See SYSCALL_RENAME in the ABI
// header for why each of those refuses instead of surprising you.
//
// Open FILES are explicitly fine on both sides. Renaming a file somebody is
// reading does not disturb them (a reader holds an inode, not a name), and
// REPLACING a file somebody is reading leaves their copy alive and nameless
// until they close it — which is how a program's own binary can be replaced
// underneath it while it runs.
int64_t os64_rename(const char *oldpath, const char *newpath)
{
    return (long)os64_syscall2(SYSCALL_RENAME, (uint64_t)oldpath, (uint64_t)newpath);
}

// Create a directory. Relative paths resolve against the cwd, like open's.
//
// One atomic call — the kernel owes us that much since 4.2BSD showed it was
// possible. Returns 0 on success, negative on failure — a read-only
// filesystem (os64's ext2), a missing parent, or a name already in use.
int64_t os64_mkdir(const char *path)
{
    return (long)os64_syscall1(SYSCALL_MKDIR, (uint64_t)path);
}
