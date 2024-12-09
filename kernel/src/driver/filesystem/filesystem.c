#include "filesystem.h"
#include "kmalloc.h"
#include "serial_logging.h"
#include "CONFIG.h"
#include "strings.h"

void detect_partition_filesystem_type(block_device_info_t* device, int partNumber)
{
	char fsType[50] = "                           ";

	printd(DEBUG_BOOT | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "BOOT: Allocating buffer to read filesystem header\n");
	unsigned char* fsbuffer = kmalloc(512*3);

	printd(DEBUG_BOOT | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "BOOT: Reading filesystem header at 0x%016lx, for 3 sectors\n", device->block_device->partition_table->parts[partNumber]->partStartSector);
	device->block_device->ops->read(device, device->block_device->partition_table->parts[partNumber]->partStartSector, fsbuffer, 3);
	
	if (fsbuffer[0x438]==0x53 && fsbuffer[0x439]==0xef)
	{
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_EXT2;
		strncpy((char*)&fsType, "ext2", 4);
	}
	else if (fsbuffer[0x52]=='F' && fsbuffer[0x53]=='A' && fsbuffer[0x54]=='T' && fsbuffer[0x55] == '3' && fsbuffer[0x56] == '2')
	{
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_FAT32;
		strncpy((char*)&fsType, "fat32", 6);
	}
	else if (fsbuffer[0x3]=='N' && fsbuffer[0x4]=='T' && fsbuffer[0x5]=='F' && fsbuffer[0x6]=='S')
	{
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_NTFS;
		strncpy((char*)&fsType, "NTFS", 5);
	}
	else 
	{
		device->block_device->partition_table->parts[partNumber]->filesystemType=FILESYSTEM_TYPE_UNDEFINED;
		strncpy((char*)&fsType, "Unidentified",13);
	}

	kfree(fsbuffer);

	if (device->block_device->partition_table->parts[partNumber]->filesystemType == FILESYSTEM_TYPE_UNDEFINED)
		printd(DEBUG_BOOT, "BOOT: Could not detect filesystem for device %s, partition %u\n", device->block_device->name, partNumber);
	else
		printd(DEBUG_BOOT, "BOOT: Detected filesystem type for device %s, partition %u is %s\n",
				device->block_device->name, partNumber, 
				fsType);
}