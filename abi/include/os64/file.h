#ifndef OS64_ABI_FILE_H
#define OS64_ABI_FILE_H

// The FILE family's result vocabulary — what open/read/write/seek/sync/close
// on a file handle can answer beyond a count or a position. Pure ABI: the
// same numbers on both sides of the boundary.
//
// The first two are the syscall boundary's house-wide verdicts, named here
// so a caller can read them; the family's own codes start at -3 (net.h's
// arrangement, and the reason: the boundary and a family share one number
// space and do not know about each other, so a family that used -2 for
// something of its own would misname an unreadable argument as that thing).

#define OS64_FILE_ERR_INVALID      (-1)  // boundary-owned: no such handle, or
                                         // a refusal with no better name
#define OS64_FILE_ERR_BAD_POINTER  (-2)  // boundary-owned: a pointer that was
                                         // not the caller's to hand over

// CLOSE: THE HANDLE IS GONE, AND WHAT WAS WRITTEN WAS NOT COMMITTED. A close
// is where two kinds of file do their real work: a FAT file flushes data and
// metadata there (FatFs commits in f_close), and a file that JUDGES what it
// was given says no there (/sys/console/font refuses a font at close, and
// says why when read). Either way the slot was freed before anyone could be
// told, so the answer cannot be "bad handle" — the boundary owns that — and
// a program that ignores close's result is unaffected. One that checks it
// learns that its bytes are not where it thinks they are. Only the LAST
// close of a shared file (spawn redirection shares one open file between
// parent and child) does the work, so only the last close can answer this.
#define OS64_CLOSE_NOT_COMMITTED   (-3)

#endif // OS64_ABI_FILE_H
