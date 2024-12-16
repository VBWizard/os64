#include "CONFIG.h"
#include "driver/filesystem/ext2/ext2_fs.h"
#include "kmalloc.h"
#include "vfs.h"
#include "ext2_vfs.h"
//#include <linux/stat.h>
#include "stat.h"
#include "BasicRenderer.h"
#include "memset.h"
#include "memcpy.h"

//TODO: Get sector size from hardware driver
#define SB_LOCATION (1024/DISK_SECTOR_SIZE)

int ext2_vfs_read_block(vfs_block_device_t* device, int blockNo, void* buffer, int length)
{
	int readLen = length / DISK_SECTOR_SIZE;
	if (length % DISK_SECTOR_SIZE)
		readLen++;
	uint64_t sectorNo = device->block_device_info->block_device->partition_table->parts[device->partNumber]->partStartSector + 
	((blockNo * device->blockSize) / DISK_SECTOR_SIZE);
	device->block_device_info->block_device->ops->read(device->block_device_info, 
				sectorNo,  
				buffer, 
				length / DISK_SECTOR_SIZE);
	return 0;
}

ext2_inode_t* ext2_get_inode(ext2_super_block_t* sb, void* inodeTable, int inodeToAccess) {
        // Calculate the address of the current inode
        void* inode_ptr = (uint8_t*)inodeTable + (inodeToAccess * sb->s_inode_size);

        // Cast to your struct to interpret the data
        ext2_inode_t* inode = (ext2_inode_t*)inode_ptr;

        // Now you can access the inode fields
		return inode_ptr;
}

// Function to retrieve a directory entry by index
ext2_dir_entry_2_t* ext2_get_directory_entry(void* dir_data, uint32_t dir_size, uint32_t index) {
    void* current = dir_data;                // Start at the beginning of the directory block
    void* end = (uint8_t*)dir_data + dir_size; // Calculate the end of the directory block
    uint32_t current_index = 0;             // Track the current entry index

    while (current < end) {
        // Cast current pointer to a directory entry
        ext2_dir_entry_2_t* entry = (ext2_dir_entry_2_t*)current;

        // Ensure the entry is valid
        if (entry->inode != 0) {
            if (current_index == index) {
                return entry; // Found the requested entry
            }
            current_index++;
        }

        // Move to the next directory entry using rec_len
        current = (uint8_t*)current + entry->rec_len;
    }

    // Return NULL if the requested index is out of bounds
    return NULL;
}

// Function to count only the populated (active) entries in a directory
uint32_t ext2_get_directory_entry_count(void* dir_data, uint32_t dir_size) {
    void* current = dir_data;                // Start at the beginning of the directory block
    void* end = (uint8_t*)dir_data + dir_size; // Calculate the end of the directory block
    uint32_t count = 0;                      // Initialize the populated entry count

    while (current < end) {
        // Cast current pointer to a directory entry
        ext2_dir_entry_2_t* entry = (ext2_dir_entry_2_t*)current;

        // Increment the count only if the entry is populated (inode != 0)
        if (entry->inode != 0) {
            count++;
        }

        // Move to the next directory entry using rec_len
        current = (uint8_t*)current + entry->rec_len;
    }

    return count;
}

int ext2_get_superblock(vfs_block_device_t* device)
{
	int start_sector = device->block_device_info->block_device->partition_table->parts[device->partNumber]->partStartSector;
	device->super_block = kmalloc(sizeof(ext2_super_block_t));
	ext2_super_block_t* sb = (ext2_super_block_t*)device->super_block;
	//Can't use read_block b/c we don't know the block size yet
	device->block_device_info->block_device->ops->read(device->block_device_info, start_sector +  SB_LOCATION ,  device->super_block, sizeof(ext2_super_block_t) / DISK_SECTOR_SIZE);

	if (sb->s_magic!=EXT2_SUPER_MAGIC)
        return ERROR_BAD_EXT2_MAGIC;

    device->blockSize = 1024 << sb->s_log_block_size;

	//Read the block group descriptor
	uint32_t num_block_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;
	uint32_t blockToRead = start_sector + SB_LOCATION / DISK_SECTOR_SIZE + device->blockSize / DISK_SECTOR_SIZE;
	device->block_group_descriptor = kmalloc(sizeof(ext2_group_desc_t));
	device->block_device_info->block_device->ops->read(device->block_device_info, blockToRead, device->block_group_descriptor, 1);
	ext2_group_desc_t* bgd = (ext2_group_desc_t*)device->block_group_descriptor;

	uint8_t* bg_block_bitmap = kmalloc(512);
	uint8_t* bg_inode_bitmap = kmalloc(512);
	ext2_inode_t* bg_inode_table = kmalloc(sb->s_inodes_per_group * sb->s_inode_size);
	ext2_vfs_read_block(device, bgd->bg_block_bitmap, bg_block_bitmap, 512);
	ext2_vfs_read_block(device, bgd->bg_inode_bitmap, bg_inode_bitmap, 512);
	ext2_vfs_read_block(device, bgd->bg_inode_table, bg_inode_table, sb->s_inodes_per_group * sb->s_inode_size);

	device->root_dir_inode = ext2_get_inode(sb, bg_inode_table, 1 /*sb->s_first_ino*/);

	ext2_dir_entry_2_t* rootDir = kmalloc(device->blockSize);

	ext2_vfs_read_block(device, ((ext2_inode_t*)(device->root_dir_inode))->i_block[0], rootDir, device->blockSize);
	uint32_t cnt=ext2_get_directory_entry_count(rootDir, device->blockSize);
	ext2_dir_entry_2_t* currEntry ;
	char filename[255];
	for (uint32_t idx = 0; idx < cnt;idx++)
	{
		currEntry = ext2_get_directory_entry(rootDir, device->blockSize, idx);
		memset(&filename,0,255);
		memcpy(&filename, currEntry->name, currEntry->name_len);
		printf ("%s\n",&filename);
	}

	return 0;

}
