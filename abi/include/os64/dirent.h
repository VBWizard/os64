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

// flags bits
#define OS64_DE_DIR  0x1   // entry is a directory (absent = regular file)

typedef struct os64_dirent
{
    uint64_t size;                           // file size in bytes (0 for directories)
    uint32_t flags;                          // OS64_DE_* bits
    char     name[OS64_DIRENT_NAME_MAX + 1]; // NUL-terminated entry name (no path)
    // Future (deliberately absent until someone needs them): timestamps —
    // FAT stores them in its own packed format; they join this struct as
    // epoch seconds when an `ls -l` wants them, not before.
} os64_dirent_t;

// readdir()'s return contract (both the syscall and the VFS dops->read):
//   1 = an entry was produced in *out
//   0 = end of directory (nothing written; calling again keeps returning 0)
//  <0 = error
// Cleaner than FatFs's "FR_OK, now go check whether fname[0] is NUL" and
// POSIX's "NULL might be the end OR an error, check errno to find out".

#endif // __ASSEMBLER__
#endif // OS64_ABI_DIRENT_H
