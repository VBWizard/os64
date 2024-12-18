#include "vfs.h"
#include "driver/filesystem/vfs/vfs.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "strings/strings.h"
#include "panic.h"
#include "ext2_fs.h"
#include "ext2_vfs.h"
#include "block_device.h"

uint8_t kFatDiskNumber=0;

vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps)
{
    vfs_filesystem_t *fs;
	
    fs = kmalloc(sizeof(vfs_filesystem_t));
    memset(fs, 0, sizeof(vfs_filesystem_t));
	
	fs->partNumber = partNo;
    fs->mount = kmalloc(sizeof(vfs_mount_t));
    
    fs->mount->mnt_root = kmalloc(sizeof(dentry_t));
    fs->mount->mnt_root->d_name = kmalloc(strlen(mountPoint));
    strcpy(fs->mount->mnt_root->d_name,mountPoint);
    
    //See if the filesystem being mounted is the root of the filesystem
    if (strncmp(mountPoint,"/",1024)==0)
        fs->mount->mnt_root->d_parent = (dentry_t*)DENTRY_ROOT;
    else if (strncmp(mountPoint,"/pipe/",1024)==0)
    {}
    else if (strncmp(mountPoint,"/proc",1024)==0)
    {}
    else
        panic("Mounting filesystem as non-root ... this is not yet supported");
    
    fs->fops = kmalloc(sizeof(vfs_file_operations_t));
    memcpy(fs->fops, fileOps,sizeof(vfs_file_operations_t));
	fs->dops = kmalloc(sizeof(vfs_directory_operations_t));
	memcpy(fs->dops, dirOps, sizeof(vfs_directory_operations_t));
    fs->bops = kmalloc(sizeof(block_operations_t));
	memcpy(fs->bops, device->block_device->ops, sizeof(block_operations_t));
    fs->files = kmalloc(sizeof(vfs_file_t*)*VFS_MAX_OPEN_FILES);
    fs->dirs = kmalloc(sizeof(vfs_directory_t*)*VFS_MAX_OPEN_DIRS);
    fs->vfsWriteBuffer = NULL;
    fs->vfsReadBuffer = NULL;
	fs->block_device_info = device;
	add_block_device(fs);
	if (device->block_device->partition_table->parts[partNo]->filesystemType==FILESYSTEM_TYPE_FAT32)
		fs->fatDiskNumber=++kFatDiskNumber;
	if (fs->fops->initialize != NULL)
		fs->fops->initialize(fs);
    return fs;
}

 int ext2_initialize_filesystem(vfs_filesystem_t* device)
{
	ext2_get_superblock(device);
	
	return 0;
}
