#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>
#include "vfs.h"

// RAMDisk block device
//
// The "disk" is a Limine module: a byte-for-byte copy of the QEMU NVMe disk
// image (GPT, partition GUID, FAT32 filesystem and all) that the bootloader
// loads into RAM before the kernel runs. Boot entries opt in by passing both
// the module (module_path: .../os64_disk.img) and the RAMDISK cmdline flag.
//
// Because the module is an exact copy of the image, the normal storage stack
// (partition-table detection, ROOT= GUID matching, the FAT driver) operates
// on it unmodified — it's just another entry in kBlockDeviceInfo whose ops
// happen to be memcpy instead of controller I/O.
//
// Writes work but land in RAM only: every boot starts from the pristine
// image on the boot media.

// Per-device state hung off block_device_info_t.block_extra_info (the same
// slot NVMe uses for its controller pointer).
typedef struct
{
	uint8_t* base;          // module data (HHDM VA, kept mapped across the CR3 switch by init_os64_paging_tables)
	uint64_t totalSectors;  // module size in 512-byte sectors
} ramdisk_device_t;

// Captured from the Limine module list in limine_boot_entry_point();
// NULL/0 when the boot entry didn't pass an os64_disk.img module.
extern void* kRamdiskModuleAddress;
extern uint64_t kRamdiskModuleSize;

void init_ramdisk_block_device(void* base, uint64_t size);

#endif
