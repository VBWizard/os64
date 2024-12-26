#include "gpt.h"
#include "mbr.h"
#include "part_table.h"
#include "vfs.h"
#include "memops.h"
#include "ata.h"
#include "part_table.h"
#include "filesystem.h"
#include "kmalloc.h"
#include "serial_logging.h"

#define EXPECTED_GPT_SIGNATURE 0x5452415020494645ULL // "EFI PART" in little-endian

e_part_table_type detect_partition_table_type(block_device_info_t* device)
{
    mbr_t mbr;
    gpt_header_t gpt;
    bool lResult;

	printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Detecting partition table type for disk %s\n", device->block_device->name);

	device->block_device->partition_table = kmalloc(sizeof(vfs_partition_table_t));

    // Read MBR
    lResult = device->block_device->ops->read(device, 0, &mbr, 1);
    if (lResult)
	{
		printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Error detecting partition type\n");
        return PART_TABLE_TYPE_ERROR;
	}

    // Check if MBR is uninitialized
    if (mbr.mbr_signature == 0x0000)
	{
		printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: MBR is uninitialized\n");
        return PART_TABLE_TYPE_UNINITIALIZED;
	}

    // Validate MBR signature
    if (mbr.mbr_signature != 0xAA55)
	{
		printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: MBR is initialized but signature is not 0xAA55\n");
        return PART_TABLE_TYPE_UNKNOWN;
	}

    // Check for protective MBR partition type 0xEE
    if (mbr.partition_entries[0].partition_type == 0xEE) {
        // Read GPT Header
		printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Found protective MBR\n");
	    lResult = device->block_device->ops->read(device, 1, &gpt, 1);
        if (lResult)
		{
			printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Error reading GPT header\n");
            return PART_TABLE_TYPE_ERROR;
		}
		device->block_device->partition_table->validBootSector = true;
        // Validate GPT Signature
        if (gpt.signature == EXPECTED_GPT_SIGNATURE)
		{
			printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Partition table type is GPT\n");
			return PART_TABLE_TYPE_GPT;
		}
        else
		{
			printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: GPT signature not found ... table type unknown\n");
            return PART_TABLE_TYPE_UNKNOWN;
		}
    } else {
		printd(DEBUG_BOOT | DEBUG_DETAILED, "BOOT: Partition table type is MBR\n");
        return PART_TABLE_TYPE_MBR;
    }
}

bool read_block_partitions(block_device_info_t* device)
{
	bool lResult;
	if (device->bus != BUS_NONE && (device->ATADeviceType == ATA_DEVICE_TYPE_SATA_HD || device->ATADeviceType == ATA_DEVICE_TYPE_NVME_HD))
	{
		printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT: Reading partitions for disk %s", device->block_device->name);
		switch (device->block_device->partTableType)
		{
			case PART_TABLE_TYPE_GPT:
				lResult = parseGPT(device);
				break;
			default:
				break;
		}
		for (int partNo = 0; partNo < device->block_device->part_count;partNo++)
		{
			printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT: Detecting partition filesystem type for partition 0x%02x\n", partNo);
			detect_partition_filesystem_type(device, partNo);
		}
	}

	printd(DEBUG_BOOT | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"BOOT: Done reading partitions for disk.");
	return lResult;
}