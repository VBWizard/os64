#ifndef EXT2_VFS_H
#define EXT2_VFS_H
#include "vfs.h"

#define DISK_SECTOR_SIZE 512

// The ext2 op tables come in TWO pairs since 2026-08-04 (the write era):
//
//   ext2_fops/ext2_dops       — READ-ONLY (write slots NULL, refused at the
//                               syscall layer AND at the stray-write
//                               tripwire). Used for an explicitly read-only
//                               or compatibility-demoted mount.
//   ext2_rw_fops/ext2_rw_dops — the writable superset (ext2_write.c). Used
//                               by every writable ext2 mount. Call
//                               ext2_rw_tables_init() once before first use —
//                               C static init can't alias another table's
//                               members, so a tiny constructor fills them.
//
// Wire a pair into a vfs_filesystem_t and call ext2_initialize_filesystem
// (also reachable as fops->initialize) to mount; it may strip the write
// slots back off a mount whose ro_compat features we don't maintain.
// ext2_initialize_filesystem itself is declared in vfs.h alongside the other
// filesystem entry points.
extern vfs_file_operations_t ext2_fops;
extern vfs_directory_operations_t ext2_dops;
extern vfs_file_operations_t ext2_rw_fops;
extern vfs_directory_operations_t ext2_rw_dops;
void ext2_rw_tables_init(void);

// Free-space counters, straight off the mounted superblock. Public because
// "did this operation give back exactly what it took?" is a question that
// cannot be answered from outside the driver otherwise — and the orphan
// chain (inodes freed at LAST CLOSE rather than at unlink) is invisible by
// construction, so its test measures instead of trusting. `df` and a future
// /sys entry are the other obvious customers. Both return 0 for a mount that
// is not ext2, which is a safe answer to ask of any filesystem.
uint32_t ext2_free_inodes(vfs_filesystem_t *fs);
uint32_t ext2_free_blocks(vfs_filesystem_t *fs);

// Where storage landed, for a caller that must predict an operation's
// command stream from the layout instead of assuming one: a release
// publishes one block group's bitmap at a time, so a file that crosses a
// group boundary is freed in more steps than one that does not. The free
// count of `group` (0 for a group that does not exist or a mount that is
// not ext2), and the
// group holding each of `path`'s first `count` data blocks (UINT32_MAX for
// a hole). The second returns -1 when the path does not resolve or the
// mount is not ext2.
uint32_t ext2_group_free_blocks(vfs_filesystem_t *fs, uint32_t group);
int      ext2_file_block_groups(vfs_filesystem_t *fs, const char *path,
                                uint32_t *groups, uint32_t count);

#endif
