#ifndef MBR_H
#define MBR_H

#include <stdint.h>

typedef struct {
    uint8_t boot_indicator;
    uint8_t starting_chs[3];
    uint8_t partition_type;
    uint8_t ending_chs[3];
    uint32_t starting_lba;
    uint32_t size_in_lba;
} __attribute__((packed)) partition_entry_t;


typedef struct {
    uint8_t boot_code[440];
    uint32_t disk_signature;
    uint16_t reserved;
    partition_entry_t partition_entries[4];
    uint16_t mbr_signature;
} mbr_t;

#endif
