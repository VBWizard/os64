#include "vfs.h"
#include "part_table.h"
#include "driver/filesystem/vfs/vfs.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "strings/strings.h"
#include "panic.h"
#include "ext2_fs.h"
#include "ext2_vfs.h"
#include "block_device.h"
#include "sprintf.h"
#include "fat_glue.h"
#include "serial_logging.h"

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

char* compare_part_uuids(const char* rootPartUUID, const char* currPartUUID)
{
	char* result = 0;

	//Make currPartUUID look like rootPartUUID
	//The format is U32-U16-U16-BYBY-BYBYBYBYBYBY
	uint32_t currPart1=(uint32_t)*(uint32_t*)currPartUUID;
	currPartUUID+=4;
	uint16_t currPart2=(uint16_t)*(uint16_t*)currPartUUID;
	currPartUUID+=2;
	uint16_t currPart3=(uint16_t)*(uint16_t*)currPartUUID;
	currPartUUID+=2;
	uint8_t currPart4[2];
	currPart4[0]=*(uint8_t*)currPartUUID;
	currPartUUID+=1;
	currPart4[1]=*(uint8_t*)currPartUUID;
	currPartUUID+=1;

	uint8_t currPart5[6];
	for (int cnt=0;cnt<6;cnt++)
	{
		currPart5[cnt]=*(uint8_t*)currPartUUID;
		currPartUUID+=1;
	}

	char* currPartStr=kmalloc(20);
	sprintf(currPartStr, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			currPart1, currPart2, currPart3, 
			currPart4[0], currPart4[1],
			currPart5[0], currPart5[1], currPart5[2], currPart5[3], currPart5[4], currPart5[5]);

	//Compare the formatted currPartUUID to the part UUID passed to be the root
	result = strnstr(rootPartUUID, currPartStr,36);

	printd(DEBUG_BOOT | DEBUG_DETAILED, "\tBOOT: Compared rootPartUUID=%s with partition %s, result=%p\n", rootPartUUID, currPartStr, result);

    kfree(currPartStr);

    if (result == rootPartUUID)
    {
        return result;
    }
	return NULL;
}

int vfs_mount_root_part(char* rootPartUUID)
{
	vfs_file_operations_t fileOps;
	vfs_directory_operations_t dirOps;
	bool mounted=false;

	//First we need to get the partition tables of all of the detected drives, and identify the partition types of each of the partitions
	for (int idx=0;idx<kBlockDeviceInfoCount;idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_SATA_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_NVME_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_HD)
		{
			kBlockDeviceInfo[idx].block_device->partTableType = detect_partition_table_type(&kBlockDeviceInfo[idx]);
			detect_partition_filesystem_types(&kBlockDeviceInfo[idx]);
		}
	}

	//Then we need to look for the partition UUID mentioned in the boot commandline parameter ROOTPARTUUID.  If it is found we'll mount it as the root of the filesystem
	for (int idx=0;idx<kBlockDeviceInfoCount;idx++)
	{
		for (int partno=0;partno<kBlockDeviceInfo[idx].block_device->part_count;partno++)
		{
			if (compare_part_uuids(rootPartUUID, (char*)kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->uniquePartGUID))
			{
				switch (kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->filesystemType)
				{
					// case FILESYSTEM_TYPE_EXT2:
					// 	fileOps.initialize = &ext2_initialize_filesystem;
					// 	vfs_filesystem_t* t = kRegisterFilesystem("/", &kBlockDeviceInfo[cnt], part, &fileOps);
					// 	mounted = true;
					// 	break;
					case FILESYSTEM_TYPE_FAT32:
						fileOps = fat_fops;
						dirOps = fat_dops;
						printd(DEBUG_BOOT, "BOOT: Root filesystem found, mounting\n");
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps);
						mounted=true;
						break;
					default: 
						panic("Could not mount root filesystem, type=%u", kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->filesystemType);
					break;
				}
			}
			if (mounted)
				break;
		}
		if (mounted)
			break;
	}
	if (kRootFilesystem==NULL)
		panic("BOOT: Could not find/mount root filesystem\n");
	return 0;
}