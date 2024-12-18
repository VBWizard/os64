#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dlist.h"
#include "types.h"


#define DENTRY_ROOT 0xFFFFFFFF    
#define VFS_MAX_OPEN_FILES 512
#define VFS_MAX_OPEN_DIRS 64
#define VFS_FILE_ALLOC_SIZE 65535+FS_FILE_COPYBUFFER_SIZE
#define VFS_MAX_PARTITIONS 128
#define DEFAULT_SECTOR_SIZE 512

#define SEEK_SET	0	/* Seek from beginning of file.  */
#define SEEK_CUR	1	/* Seek from current position.  */
#define SEEK_END	2	/* Seek from end of file.  */

struct vfs_partition_table;

typedef struct directory vfs_directory_t;
typedef struct direntry vfs_dirent_t;
typedef struct dir_operations vfs_directory_operations_t;
typedef struct inode vfs_inode_t;
typedef struct dentry dentry_t;
typedef struct vfsmount vfs_mount_t;
typedef struct inode_operations vfs_inode_operations_t;
typedef struct vfs_filesystem vfs_filesystem_t;
typedef struct inode inode_t;
typedef struct file vfs_file_t;
typedef struct file_operations vfs_file_operations_t;
typedef struct block_device block_device_t;
typedef struct block_operations block_operations_t;
typedef struct vfs_partition_table vfs_partition_table_t;

typedef enum
{
	FILESYSTEM_TYPE_UNDEFINED,
	FILESYSTEM_TYPE_FAT,
	FILESYSTEM_TYPE_FAT32,
	FILESYSTEM_TYPE_EXT2,
	FILESYSTEM_TYPE_NTFS
} e_filesystem_type;

enum whichBus
{
	BUS_NONE,
    BUS_ATA_PRIMARY,
    BUS_ATA_SECONDARY,
    BUS_SATA,
	BUS_NVME
};

typedef enum
{
	FILETYPE_FILE = 1,
	FILETYPE_PIPE = 2,
	FILETYPE_PROCFILE = 3
} eFileType;

enum whichDrive
{
    master = 0,
    slave = 1
};

typedef struct 
{
    uint16_t ATAIdentifyData[256];
    char ATADeviceModel[80];
    bool queryATAData;
    uint8_t DeviceAvailable;
    int ATADeviceType;
    uint32_t totalSectorCount;
    uint32_t sectorSize;
    bool lbaSupported;
    bool lba48Supported;
    bool dmaSupported;
    enum whichBus bus; 
    enum whichDrive driveNo;
    uintptr_t ioPort;
    uint8_t irqNum;
    uint8_t driveHeadPortDesignation;
	int major;
	block_device_t* block_device;
	void* block_extra_info;
} __attribute__((packed)) block_device_info_t;

struct block_operations
{
	//int (*seek) (void *dev, long offset, int origin);
	size_t (*read) (void* device, uint64_t sector, void * buffer, uint64_t sectorCount);
	size_t (*write) (void* device, uint64_t sector, const void * buffer, uint64_t sectorCount);
};

typedef enum
{
	PART_TABLE_TYPE_UNINITIALIZED,
	PART_TABLE_TYPE_UNKNOWN,
	PART_TABLE_TYPE_MBR,
	PART_TABLE_TYPE_GPT,
	PART_TABLE_TYPE_ERROR
} e_part_table_type;

struct block_device
{
	char* name;
	block_device_info_t* device;
	block_operations_t* ops;
	int part_count;
	e_part_table_type partTableType;
	vfs_partition_table_t* partition_table;
};

struct inode
{
	unsigned int            i_dev;          //12 bits major, 20 bits minor
	unsigned short          i_mode;
	unsigned short          i_opflags;
	unsigned int            i_uid;
	unsigned int            i_gid;
	unsigned int            i_flags;
	const vfs_inode_operations_t   *i_op;
	struct vfsmount         *i_vfsmount;
};

struct vfs_inode_operations
{
	int (*create) (vfs_inode_t *,dentry_t *);
	int (*mkdir) (vfs_inode_t *,dentry_t *);
	int (*rmdir) (vfs_inode_t *,dentry_t *);
	int (*mknod) (vfs_inode_t *,dentry_t);
	int (*rename) (vfs_inode_t *, dentry_t *,inode_t *, dentry_t *, unsigned int);
};

struct vfsmount 
{
	dentry_t *mnt_root;        /* root of the mounted tree */
	struct super_block *mnt_sb;     /* pointer to superblock */
	int mnt_flags;
};

struct directory
{
	char* f_path;
	inode_t* f_inode;
	vfs_directory_operations_t* dops;
	void* handle;
	dlist_t listEntry;
	void *owner;
	//arena_t* arena;
};

struct dir_operations
{
	int (*open) (vfs_directory_t** vfs_dir, const char* path, vfs_filesystem_t* vfs_fs);
	int (*read) (vfs_directory_t* vfs_dir, void* fileInfo);
	int (*close) (vfs_directory_t* vfs_dir);
};
	
struct dentry
{
	char* d_name;
	struct inode* d_inode;
	dentry_t* d_parent;
};

typedef struct
{
    uint32_t partStartSector; //LBA address of partition
    uint32_t partEndSector;
    uint32_t partTotalSectors;
	uuid_t partTypeGUID;
	uuid_t uniquePartGUID;
    bool bootable;
    uint8_t systemID;
	char partName[36];
	e_filesystem_type filesystemType;
	block_device_info_t* block_device_info;
} partEntry_t;

struct vfs_partition_table
{
    partEntry_t* parts[VFS_MAX_PARTITIONS];
    int partCount;
    uint8_t diskID[10];
    bool validBootSector;
} __attribute__((packed));

struct vfs_filesystem
{
	vfs_mount_t *mount; 
	vfs_inode_operations_t* iops;
	//Block operations
	block_operations_t* bops;
	//File operations
	vfs_file_operations_t* fops;
	vfs_directory_operations_t* dops;
	dlist_t inode_list;
	vfs_file_t *files;
	vfs_directory_t *dirs;
	char* vfsReadBuffer, *vfsWriteBuffer;
	int partNumber;
	int blockSize;
	int inodes_per_block;
	int inode_table_blocks;
	void* super_block;
	void* block_group_descriptor;
	void* root_dir_inode;
	block_device_info_t* block_device_info;	
	uint8_t major;
	uint8_t minor;
	uint8_t fatDiskNumber;
	void* fs_specific;
};

struct file
{
	eFileType filetype;
	char* f_path;
	inode_t* f_inode;
	vfs_file_operations_t* fops;
	void* handle;
	void *pipe, *pipeContent, **pipeContentPtr;
	void *copyBuffer;
	uint32_t verification;
	void *owner;
	//arena_t* arena;
};

struct file_operations
{
    int (*open)(vfs_file_t** vfs_file, const char* path, const char* mode, vfs_filesystem_t* vfs_fs);
    int (*read)(vfs_file_t* vfs_file, void* buffer, size_t size);
	char* (*fgets)(vfs_file_t* vfs_file, char* buffer, int length);
	int (*fputs)(vfs_file_t* vfs_file, char* buffer);
	int (*tell)(vfs_file_t* vfs_file);
	int (*fprintf)(vfs_file_t* vfs_file, const char* fmt, ...);
    int (*write)(vfs_file_t* vfs_file, const void* buffer, size_t size);
    int (*seek)(vfs_file_t* vfs_file, long offset, int whence);
	int (*sync)(vfs_file_t* vfs_file);
    int (*close)(vfs_file_t* vfs_file);
	int (*flush) (void *f);
	int (*rm) (const char *filename);
	int (*initialize) (vfs_filesystem_t* device);
	int (*uninitialize) (vfs_filesystem_t* device);
};


extern dlist_t* kBlockDeviceDList;

void init_block();
vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps);
int ext2_initialize_filesystem(vfs_filesystem_t* device);

#endif
