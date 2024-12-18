#ifndef EXT2_VFS_H
#define EXT2_VFS_H
#include "vfs.h"

#define DISK_SECTOR_SIZE 512

int ext2_get_superblock(vfs_filesystem_t* device);

#endif
