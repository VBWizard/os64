#include "gpt.h"
#include "mbr.h"
#include "part_table.h"
#include "vfs.h"
#include "memops.h"
#include "ata.h"
#include "part_table.h"
#include "filesystem.h"
#include "kmalloc.h"

#define EXPECTED_GPT_SIGNATURE 0x5452415020494645ULL // "EFI PART" in little-endian

e_part_table_type detect_partition_table_type(block_device_info_t* device)
{
    mbr_t mbr;
    gpt_header_t gpt;
    bool lResult;

	device->block_device->partition_table = kmalloc(sizeof(vfs_partition_table_t));

    // Read MBR
    lResult = device->block_device->ops->read(device, 0, &mbr, 1);
    if (!lResult)
        return PART_TABLE_TYPE_ERROR;

    // Check if MBR is uninitialized
    if (mbr.mbr_signature == 0x0000)
        return PART_TABLE_TYPE_UNINITIALIZED;

    // Validate MBR signature
    if (mbr.mbr_signature != 0xAA55)
        return PART_TABLE_TYPE_UNKNOWN;

    // Check for protective MBR partition type 0xEE
    if (mbr.partition_entries[0].partition_type == 0xEE) {
        // Read GPT Header
	    lResult = device->block_device->ops->read(device, 1, &gpt, 1);
        if (!lResult)
            return PART_TABLE_TYPE_ERROR;
		device->block_device->partition_table->validBootSector = true;
        // Validate GPT Signature
        if (gpt.signature == EXPECTED_GPT_SIGNATURE)
            return PART_TABLE_TYPE_GPT;
        else
            return PART_TABLE_TYPE_UNKNOWN;
    } else {
        return PART_TABLE_TYPE_MBR;
    }
}

bool read_block_partitions(block_device_info_t* devices, int deviceCount)
{
	bool lResult;
	block_device_info_t* device;
	for (int idx=0;idx<deviceCount;idx++)
	{
		device = &devices[idx];
		if (device->bus != BUS_NONE && (device->ATADeviceType == ATA_DEVICE_TYPE_SATA_HD || device->ATADeviceType == ATA_DEVICE_TYPE_NVME_HD))
		{
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
				detect_partition_filesystem(device, partNo);
			}
		}
	}
	return lResult;
}