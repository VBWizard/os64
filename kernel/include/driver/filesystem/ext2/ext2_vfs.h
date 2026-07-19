#ifndef EXT2_VFS_H
#define EXT2_VFS_H
#include "vfs.h"

#define DISK_SECTOR_SIZE 512

// The ext2 op tables (read-only driver — see ext2.c). Wire them into a
// vfs_filesystem_t and call ext2_initialize_filesystem (also reachable as
// fops->initialize) to mount. ext2_initialize_filesystem itself is declared
// in vfs.h alongside the other filesystem entry points.
extern vfs_file_operations_t ext2_fops;
extern vfs_directory_operations_t ext2_dops;

#endif
