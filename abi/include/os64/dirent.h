#ifndef OS64_ABI_DIRENT_H
#define OS64_ABI_DIRENT_H

// The os64 directory entry — what readdir() hands back, one call per entry.
// Part of the ABI (kernel fills it, userland reads it), and ALSO the
// kernel-internal VFS dops->read contract: every filesystem driver translates
// its own on-disk shape (FatFs FILINFO, ext2_dir_entry_2, ...) into THIS,
// inside the driver, where fs-specific knowledge belongs. The fs-specific
// struct never crosses the VFS seam — that's what lets ls work unchanged the
// day the root filesystem stops being FAT.
//
// os64's design, not POSIX's: struct dirent famously guarantees almost
// nothing (d_name and wishes), with size/type needing a follow-up stat() per
// entry. A directory listing wants name+size+kind for every row anyway, so
// os64 delivers them in one go.
//
// This header must stay C-only-safe (no kernel types, no function
// prototypes): it is included by userland (via <os64/io.h>) and by the
// kernel's VFS alike.

#ifndef __ASSEMBLER__

#include <stdint.h>

#define OS64_DIRENT_NAME_MAX 255

// flags bits. DIR is a KIND (what the entry is); MOUNT is an ATTRIBUTE that
// rides on top of it (a mount point always lists as a directory), so a
// reader tests them independently and never as an enumeration.
#define OS64_DE_DIR  0x1   // entry is a directory (absent = regular file)

// ANOTHER FILESYSTEM BEGINS HERE. Set on the entry for a mount point,
// through both doors that can name one: a readdir of the parent, and a stat
// of the point itself.
//
// It exists for the RECURSIVE WALKERS — rm -r, du, cp -r, a future find.
// A mount point has no directory entry on the parent's disk; the namespace
// synthesizes it so `ls /mnt` does not lie, and that synthetic entry is
// exactly the ramp a walker climbs into a filesystem nobody named. The
// kernel cannot catch that on its own: a recursive delete reaches it as a
// series of ordinary unlinks on ordinary paths, indistinguishable from
// someone deleting those files on purpose. So the walker has to decide, and
// this is the fact it decides on — delivered by the same readdir that gave
// it the name, so there is no second question to ask and no window between
// the two answers.
//
// Deliberately NOT Unix's answer. There, a walker compares st_dev across
// the tree behind --one-file-system; st_dev exists because stat had to
// answer "are these two names the same file?" for hard links, and
// mount-crossing is a DERIVED use every tool re-derives. os64 answers the
// question that is actually being asked. (When hard links arrive, the
// "same file?" question gets its own field here and this bit is unaffected
// — one field, one meaning.)
//
// WHAT IT DOES NOT COVER, for whoever adds symbolic links: a symlink can
// point across a mount, and a walker that FOLLOWS one arrives inside
// another filesystem having never seen this bit. The rule that closes that
// door is "a recursive walk does not follow symlinks", not another flag.
#define OS64_DE_MOUNT 0x2  // entry is a mount point — a different filesystem

typedef struct os64_dirent
{
    uint64_t size;                           // file size in bytes (0 for directories)
    uint32_t flags;                          // OS64_DE_* bits
    char     name[OS64_DIRENT_NAME_MAX + 1]; // NUL-terminated entry name (no path)

    // Modification time, seconds since the Unix epoch; 0 = "this filesystem
    // has no time to give you" (procfs, synthetic mount-point entries).
    // The comment that used to sit here said timestamps would "join this
    // struct as epoch seconds when an ls -l wants them, not before" — cp
    // raised its hand on 2026-08-06 and here it is, exactly as promised.
    // ONE field, ONE meaning: mtime is THE time humans ask about ("is my
    // copy stale?", ls -l's column). atime is a polite fiction on most
    // systems and ctime a POSIX subtlety; if either ever earns a seat it
    // gets its OWN field — this one's meaning never changes (the memory.h
    // doctrine, applied to time). Sources and their honesty: ext2 stores
    // epoch seconds natively (straight copy); FAT stores packed local
    // date/time at 2-second resolution since MS-DOS 1.25 (1982), converted
    // as-if-UTC — os64 has no timezone-of-the-disk story yet, and 2s
    // granularity is FAT's truth, not hidden. Appended after name[] so no
    // earlier field moved (the rebuilt-world rule, same as read's arg3).
    uint64_t mtime;
} os64_dirent_t;

// readdir()'s return contract (both the syscall and the VFS dops->read):
//   1 = an entry was produced in *out
//   0 = end of directory (nothing written; calling again keeps returning 0)
//  <0 = error
// Cleaner than FatFs's "FR_OK, now go check whether fname[0] is NUL" and
// POSIX's "NULL might be the end OR an error, check errno to find out".

#endif // __ASSEMBLER__
#endif // OS64_ABI_DIRENT_H
