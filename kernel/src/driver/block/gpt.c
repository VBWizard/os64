#include "driver/block/gpt.h"
#include "stdbool.h"
#include "vfs.h"
#include "CONFIG.h"
#include "panic.h"
#include "serial_logging.h"
#include "kmalloc.h"
#include "memops.h"
#include "strcpy.h"

const uint32_t EXPECTED_SIGL = 0x20544945; // "EFI "
const uint32_t EXPECTED_SIGH = 0x54524150; // "PART"

uint8_t mbrBuffer[512];
gpt_header_t* gptHdr;
gpt_part_entry_t* gptPart;

/// @brief Convert a little endian utf16 array to a character string by ignoring every 2nd byte (high byte of the uint16_t)
/// @param dest The destination char*
/// @param src The source uint16_t*
/// @param dest_length The char length of the destination
/// @param src_length The number of 16 bit entries in the source
/// @return The count of characters copied
int utf16_to_char_string(char* dest, uint16_t* src, int dest_length, int src_length)
{
	int length = 0;
	for (int cnt=0;cnt<src_length;cnt++)
	{
		dest[cnt] = src[cnt];
		if (length++==dest_length || src[cnt]==0x0)
			break;
	}
	return length;
}


bool parseGPT(block_device_info_t* device)
{
	int readLen=0;
	char *partBuffer = kmalloc(40*512);

	printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT: parseGPT -  Retrieving MBR %s", device->block_device->name);
	bool lResult=device->block_device->ops->read(device, 1, mbrBuffer, 1);
    if (!lResult)
        panic("parseGPT: Read error\n");
    gptHdr=(gpt_header_t*)mbrBuffer;

    printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT:  parseGPT - GPT PT LBA=%u, PT entries=%04x, PT entry len=%04x, last usable LBA=%08x\n",
            gptHdr->partitionEntryLBA,
            gptHdr->numPartitionEntries,
            gptHdr->sizeofPartitionEntry,
            gptHdr->lastUsableLBA);
    readLen=((gptHdr->numPartitionEntries*gptHdr->sizeofPartitionEntry)/512)+1;
    
    printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT:  parseGPT - Reading GPT partition table @ lba %u for %u sectors\n",gptHdr->partitionEntryLBA,readLen);
	lResult=device->block_device->ops->read(device, gptHdr->partitionEntryLBA, partBuffer, readLen);
    if (!lResult)
        panic("parseGPT: Read error\n");
    
    gptPart=(gpt_part_entry_t*)partBuffer;
	device->block_device->part_count = 0;

    for (int cnt=0;cnt<20;cnt++)
    {
		if (gptPart[cnt].partTypeGUID[0] != 0)
		{
        	printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT:  parseGPT - Part %u, first=%u\n",cnt,gptPart[cnt].partFirstLBA);
			if (gptPart[cnt].partFirstLBA>0)
			{
				printd(DEBUG_BOOT | DEBUG_DETAILED,"BOOT: parseGPT -  Adding partition to device partiton list\n");
				device->block_device->partition_table->parts[cnt] = kmalloc(sizeof(partEntry_t));
				device->block_device->partition_table->parts[cnt]->partStartSector = gptPart[cnt].partFirstLBA;
				device->block_device->partition_table->parts[cnt]->partEndSector=gptPart[cnt].partLastLBA;
				device->block_device->partition_table->parts[cnt]->partTotalSectors=device->block_device->partition_table->parts[cnt]->partEndSector-device->block_device->partition_table->parts[cnt]->partStartSector;
				memcpy(&device->block_device->partition_table->parts[cnt]->partTypeGUID, &gptPart[cnt].partTypeGUID, UUID_LENGTH);
				memcpy(&device->block_device->partition_table->parts[cnt]->uniquePartGUID, &gptPart[cnt].uniquePartGUID, UUID_LENGTH);
				utf16_to_char_string((char*)&device->block_device->partition_table->parts[cnt]->partName, (uint16_t*)&gptPart[cnt].partName, 36, 72);
				device->block_device->partition_table->parts[cnt]->block_device_info = device;
				device->block_device->part_count++;
				device->block_device->partition_table->partCount++;
			}
		}
    }
	printd(DEBUG_BOOT | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"BOOT:  parseGPT - Freeing partBuffer\n");
	kfree(partBuffer);

	printd(DEBUG_BOOT | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"BOOT: parseGPT - returning\n");
    return true;
        
}
