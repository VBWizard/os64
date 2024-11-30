/* 
 * File:   gpt.h
 * Author: yogi
 *
 * Created on June 7, 2016, 9:11 AM
 */

#ifndef GPT_H
#define	GPT_H

#include "types.h"
#include "stdint.h"
#include "stdbool.h"
#include "vfs.h"

	typedef struct 
	{
		uint64_t signature;
		uint32_t    revision;           //8
		uint32_t    hdrSize;            //12
		uint32_t    crc32;              //16
		uint32_t    zeroes;             //20
		uint64_t current_lba;
		uint64_t backup_lba;
		uint64_t firstUsableLBA;
		uint64_t lastUsableLBA;
		uuid_t      diskGUID;           //56
		uint64_t partitionEntryLBA;
		uint32_t numPartitionEntries;
		uint32_t sizeofPartitionEntry;
		uint32_t partitionEntryArrayCRC32;
	} __attribute__((packed)) gpt_header_t;

	typedef struct 
	{
		uint8_t     partTypeGUID[16];
		uint8_t     uniquePartGUID[16];
		uint64_t    partFirstLBA;
		uint64_t    partLastLBA;
		uint64_t    partFlags;
		uint16_t     partName[36];
	} gpt_part_entry_t;

	bool parseGPT(block_device_info_t* device);

#endif	/* GPT_H */

