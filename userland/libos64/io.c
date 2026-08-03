// libos64 io.c — the friendly veneer over the raw write/exit syscalls.

#include <stdbool.h>       // os64_readline's flags — a .c includes what it
                           // uses too, not what its neighbors happen to drag in
#include "os64/io.h"
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
    // The explicit 0 is load-bearing: arg3 is the read DEADLINE now, and
    // before this stub passed it deliberately, that register was ring-3
    // garbage the kernel ignored. A rebuilt world always says "forever"
    // out loud rather than leaving patience to whatever was in r10.
    return (long)os64_syscall4(SYSCALL_READ, (uint64_t)handle,
                               (uint64_t)buf, (uint64_t)len, 0);
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

int64_t os64_printat(uint32_t x, uint32_t y, const char *s)
{
    return (int64_t)os64_syscall3(SYSCALL_PRINTAT, (uint64_t)x, (uint64_t)y,
                                  (uint64_t)s);
}

void os64_debug_log(const char *s)
{
    os64_syscall1(SYSCALL_DEBUG_LOG, (uint64_t)s);
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

int64_t os64_close(int32_t handle)
{
    return (long)os64_syscall1(SYSCALL_CLOSE, (uint64_t)(int64_t)handle);
}
