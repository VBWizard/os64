#include "ramdisk.h"
#include "ata.h"
#include "block_device.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "strings.h"
#include "serial_logging.h"
#include "CONFIG.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;

// Set by limine_boot_entry_point() when the boot entry passes an
// os64_disk.img module; left NULL/0 otherwise (see ramdisk.h).
void* kRamdiskModuleAddress = NULL;
uint64_t kRamdiskModuleSize = 0;

// The disk image is built with 512-byte sectors (sgdisk/mformat defaults in
// the root GNUmakefile), matching what the GPT and FAT code expect.
#define RAMDISK_SECTOR_SIZE 512

// Read/write share the storage-ops contract with nvme_vfs_read_disk et al:
// sector-granular, return 0 on success / nonzero on error (part_table.c
// treats any nonzero return from ops->read as a failure).
size_t ramdisk_vfs_read_disk(block_device_info_t* device, uint64_t sector, void* buffer, uint64_t sector_count)
{
	ramdisk_device_t* ramdisk = (ramdisk_device_t*)device->block_extra_info;

	if (sector + sector_count > ramdisk->totalSectors)
	{
		printd(DEBUG_BOOT, "RAMDISK: read past end of disk (sector %lu, count %lu, total %lu)\n",
			sector, sector_count, ramdisk->totalSectors);
		return 1;
	}

	memcpy(buffer, ramdisk->base + (sector * RAMDISK_SECTOR_SIZE), sector_count * RAMDISK_SECTOR_SIZE);
	return 0;
}

size_t ramdisk_vfs_write_disk(block_device_info_t* device, uint64_t sector, const void* buffer, uint64_t sector_count)
{
	// Stray-write tripwire (block_device.c): same guard as the NVMe path.
	// RAMDisk writes vanish at reboot, which makes THIS the boot mode where a
	// misrouted write would be invisible forever — all the more reason to trap.
	block_verify_write_allowed(device, sector, sector_count);

	ramdisk_device_t* ramdisk = (ramdisk_device_t*)device->block_extra_info;

	if (sector + sector_count > ramdisk->totalSectors)
	{
		printd(DEBUG_BOOT, "RAMDISK: write past end of disk (sector %lu, count %lu, total %lu)\n",
			sector, sector_count, ramdisk->totalSectors);
		return 1;
	}

	memcpy(ramdisk->base + (sector * RAMDISK_SECTOR_SIZE), (void*)buffer, sector_count * RAMDISK_SECTOR_SIZE);
	return 0;
}

// Mirrors init_vfs_block_device() in nvme.c: claim a kBlockDeviceInfo slot
// and attach a block_device whose ops are the memcpy wrappers above. Must be
// called after init_block() has allocated the table.
void init_ramdisk_block_device(void* base, uint64_t size)
{
	ramdisk_device_t* ramdisk = kmalloc(sizeof(ramdisk_device_t));
	ramdisk->base = (uint8_t*)base;
	ramdisk->totalSectors = size / RAMDISK_SECTOR_SIZE;

	// ATA_DEVICE_TYPE_HD is one of the three types vfs_mount_root_part()
	// scans when hunting for the ROOT= partition GUID.
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_extra_info = (void*)ramdisk;
	kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceType = ATA_DEVICE_TYPE_HD;
	kBlockDeviceInfo[kBlockDeviceInfoCount].bus = BUS_NONE;
	kBlockDeviceInfo[kBlockDeviceInfoCount].DeviceAvailable = true;
	kBlockDeviceInfo[kBlockDeviceInfoCount].dmaSupported = false;
	kBlockDeviceInfo[kBlockDeviceInfoCount].driveNo = kBlockDeviceInfoCount;
	kBlockDeviceInfo[kBlockDeviceInfoCount].major = 0x1;
	kBlockDeviceInfo[kBlockDeviceInfoCount].sectorSize = RAMDISK_SECTOR_SIZE;
	kBlockDeviceInfo[kBlockDeviceInfoCount].totalSectorCount = ramdisk->totalSectors;
	strncpy(kBlockDeviceInfo[kBlockDeviceInfoCount].ATADeviceModel, "os64 ramdisk", 40);

	block_device_t* blockDev = kmalloc(sizeof(block_device_t));
	blockDev->name = "ramdisk0";
	blockDev->device = &kBlockDeviceInfo[kBlockDeviceInfoCount];
	block_operations_t* bops = kmalloc(sizeof(block_operations_t));
	bops->read = (void*)ramdisk_vfs_read_disk;
	bops->write = (void*)ramdisk_vfs_write_disk;
	blockDev->ops = bops;
	kBlockDeviceInfo[kBlockDeviceInfoCount].block_device = blockDev;
	kBlockDeviceInfoCount++;

	printd(DEBUG_BOOT, "RAMDISK: registered %lu MB module at 0x%016lx as a block device\n",
		size / (1024 * 1024), (uintptr_t)base);
}
