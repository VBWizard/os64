/* THE REPLAY RUNNER'S DOOR TO THE HOST'S FILES (tools/test_yonder_replay_fs.c,
 * its own translation unit): every file and directory the runner reads or
 * writes goes through these, and each answers one of three, which its
 * caller must meet: the thing was done; it is ABSENT, which is an answer
 * only where the caller says nothing there is legitimate; or a FAULT, with
 * why, which the caller makes an UNJUDGED page or a refused run, never an
 * empty answer. A kept file that is a symbolic link is a fault: nothing
 * yonder keeps is one, and a link would bind a record to bytes it never
 * had. tools/test_yonder_replay_host.c poisons, after including this
 * header, the host's file functions that unit could reach. */
#ifndef TEST_YONDER_REPLAY_FS_H
#define TEST_YONDER_REPLAY_FS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { RP_FS_OK, RP_FS_ABSENT, RP_FS_FAULT } RpFs;
/* Why a call faulted: the path and what the host said. */
typedef struct { char why[1024]; } RpFault;

/* A whole file into `*bytes` (malloc'd, NUL-terminated past `*len`); a
 * symbolic link is not followed, and is a fault. */
RpFs rp_fs_read(const char *path, char **bytes, size_t *len, RpFault *f);
/* `len` bytes made the whole of `path`, WHOLE OR NOT AT ALL: written to a
 * temporary file beside it, flushed to the disk, and renamed over it, the
 * temporary removed when any step faults, so a write cut short (a full
 * disk, a size limit) leaves whatever was there before, never part of a
 * file. The directory is not flushed after the rename, so a power lost
 * just then may still leave part of a record; the `end` line a replay's
 * record ends with, which every reader requires, is what refuses it. */
RpFs rp_fs_write(const char *path, const void *bytes, size_t len, RpFault *f);
/* What is at `path`: a directory (`*dir` true) or anything else; a
 * symbolic link is a fault. */
RpFs rp_fs_kind(const char *path, bool *dir, RpFault *f);
/* A directory made, or one already there. */
RpFs rp_fs_mkdir(const char *path, RpFault *f);
/* A file or an empty directory removed. */
RpFs rp_fs_remove(const char *path, RpFault *f);
/* A path's permission bits set: the self-test makes a directory that can
 * be searched and not listed. */
RpFs rp_fs_mode(const char *path, unsigned mode, RpFault *f);
/* Whether two existing paths are one directory (`*same`), by device and
 * inode, so a bind mount or a link is the same directory. */
RpFs rp_fs_same(const char *a, const char *b, bool *same, RpFault *f);
/* The names in `dir` that end in `suffix`, sorted, into `*names` (each and
 * the array malloc'd; rp_fs_names_free lets them go). A directory that is
 * there and cannot be listed whole is a fault. */
RpFs rp_fs_list(const char *dir, const char *suffix, char ***names, size_t *n, RpFault *f);
void rp_fs_names_free(char **names, size_t n);

#endif
