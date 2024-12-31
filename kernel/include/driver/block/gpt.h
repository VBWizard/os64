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

typedef struct {
    uint8_t     partTypeGUID[16];      // Partition Type GUID

    // Union to access Unique Partition GUID as raw bytes or structured fields
    union {
        uint8_t uniquePartGUID[16];    // Unique Partition GUID (PARTUUID) as raw bytes

        // Structured representation of the GUID components
        struct {
            uint32_t Data1;             // 4 bytes, Little-Endian
            uint16_t Data2;             // 2 bytes, Little-Endian
            uint16_t Data3;             // 2 bytes, Little-Endian
            uint8_t  Data4[8];          // 8 bytes, Big-Endian
        } guid;
    };

    uint64_t    partFirstLBA;          // Starting LBA
    uint64_t    partLastLBA;           // Ending LBA
    uint64_t    partFlags;             // Partition Attributes
    uint16_t    partName[36];          // Partition Name (UTF-16LE)
} gpt_part_entry_t;

	bool parseGPT(block_device_info_t* device);

#endif	/* GPT_H */

