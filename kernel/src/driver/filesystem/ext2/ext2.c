#include "driver/filesystem/ext2/ext2.h"
#include "driver/filesystem/ext2/ext2_fs.h"
#include "driver/filesystem/ext2/ext2blockgroup.h"
#include "driver/filesystem/ext2/ext2dir.h"
#include "driver/filesystem/ext2/ext2inode.h"
#include "driver/filesystem/ext2/ext2errors.h"
#include "kmalloc.h"

int ext2_get_superblock(block_device_t* block_device)
{
	int start_sector = block_device->partition_table->parts[0]->partStartSector;

	ext2_super_block_t* SuperBlock = kmalloc(sizeof(ext2_super_block_t));
	block_device->ops->read(block_device->device, start_sector +  (1024/DISK_SECTOR_SIZE),  SuperBlock, sizeof(ext2_super_block_t) / 512);
    if (SuperBlock->s_magic!=EXT2_SUPER_MAGIC)
        return ERROR_BAD_EXT2_MAGIC;
    else
        return 0;
}
