#ifndef OS64_AV_ERRNO_H
#define OS64_AV_ERRNO_H
/* Private to the FFmpeg port. os64 has no errno (DIVERGENCES.md's table:
 * in-band errors only), but FFmpeg spells its error codes as AVERROR(E...),
 * the negated POSIX number, so the numbers are needed and are Linux's,
 * which is what FFmpeg's own av_strerror table and every FFmpeg user
 * expect. `errno` itself is one word, shared by every thread: strto* set it
 * on overflow and the shut file doors (av_port.c) on a refused open, and
 * FFmpeg reads it right after such a call, to name the error. */
#include "av_libc.h"
#define errno avlibc_errno
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ENOLCK 37
#define ENOSYS 38
#define ENOTEMPTY 39
#define EILSEQ 84
#define ENOTSUP 95
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ECONNRESET 104
#define ENOBUFS 105
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#endif
