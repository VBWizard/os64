#include "filesystem.h"
#include "kmalloc.h"
#include "serial_logging.h"
#include "CONFIG.h"

void detect_partition_filesystem(block_device_info_t* device, int partNumber)
{
	unsigned char* fsbuffer = kmalloc(512*3);

	device->block_device->ops->read(device, device->block_device->partition_table->parts[partNumber]->partStartSector, fsbuffer, 3);
	
	if (fsbuffer[0x438]==0x53 && fsbuffer[0x439]==0xef)
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_EXT2;
	else if (fsbuffer[0x52]=='F' && fsbuffer[0x53]=='A' && fsbuffer[0x54]=='T' && fsbuffer[0x55] == '3' && fsbuffer[0x56] == '2')
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_FAT32;
	else 
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_UNDEFINED;

	kfree(fsbuffer);
	if (device->block_device->partition_table->parts[partNumber]->filesystemType != FILESYSTEM_TYPE_UNDEFINED)
		printd(DEBUG_HARDDRIVE, "Detected file system for device %s, partition %u is %s\n",
				&device->ATADeviceModel, partNumber, 
				device->block_device->partition_table->parts[partNumber]->filesystemType==FILESYSTEM_TYPE_EXT2?"ext2"
				:device->block_device->partition_table->parts[partNumber]->filesystemType==FILESYSTEM_TYPE_FAT32?"fat32":"unknown");
}