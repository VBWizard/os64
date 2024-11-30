#ifndef EXT2_H
#define EXT2_H

#include "driver/filesystem/ext2/ext2_fs.h"
#include "vfs.h"

#define READ_BUFFER_BLOCK_COUNT 256
#define FILE_HANDLE_MAX_COUNT 1024
#define SESSION_MAX_COUNT 100
#define DISK_SECTOR_SIZE 512
#define BASE_OFFSET 4096  /* location of the super-block in the first group */
#define BLOCK_OFFSET_FIRSTGROUP(block) (BASE_OFFSET + (block-1)*block_size)
#define BLOCK_OFFSET(block,block_size) (BASE_OFFSET + (block-1)*block_size)

    typedef struct tExt2Session
    {
        int sessionNbr;
        int (*readFunction)(int, char*, int);
        int (*writeFunction)(int, char*, int, int);
        ext2_super_block_t superBlock;
        int device;
        int blockSize, inodes_per_block, inode_table_blocks;
        ext2_inode_t rootDirInode;
    } sExt2Session;

    typedef struct tFile
    {
        int filePtr;
        int sess;
        int base,offset;
        ext2_inode_t inode;
        char symlinkPath[1024];
        int inodeNumber;
        char* name;
        char path[1024];
        int lastBlockFreeSize;
        unsigned int* blockList;
    } sFile;

	int ext2_get_superblock(block_device_t* block_device);

#endif
