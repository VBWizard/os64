#include "block_device.h"
#include "kmalloc.h"
#include "panic.h"

dlist_t* kBlockDeviceDList;
int SATAMinor=0, NVMEMinor=0;

// The shared device table every storage driver (AHCI, NVMe, RAMDisk)
// registers into. These historically lived in ahci.c and were allocated by
// init_AHCI(), which meant a noahci boot never got a table at all — any
// other driver registering into it dereferenced NULL. Owned here now.
block_device_info_t* kBlockDeviceInfo = NULL;
int kBlockDeviceInfoCount = 0;

#define MAX_BLOCK_DEVICES 20

void init_block()
{
	// Idempotent: kernel_init() calls this before any storage driver runs,
	// but init_AHCI() also still calls it (its original call site) — the
	// second invocation is a no-op.
	if (kBlockDeviceDList != NULL)
		return;

	kBlockDeviceDList = kmalloc(sizeof(dlist_t));
	if (kBlockDeviceDList == NULL)
	{
		panic("init_block: failed to allocate block device dlist\n");
	}
	dlist_init(kBlockDeviceDList);

	kBlockDeviceInfo = kmalloc(MAX_BLOCK_DEVICES * sizeof(block_device_info_t));
	if (kBlockDeviceInfo == NULL)
	{
		panic("init_block: failed to allocate block device info table\n");
	}
	kBlockDeviceInfoCount = 0;
}

dlist_node_t* add_block_device(vfs_filesystem_t* vfs_block_device)
{
	int major = 0;
	int minor = 0;

	switch (vfs_block_device->block_device_info->bus)
	{
		case BUS_SATA:
			major = 8;
			minor = SATAMinor++;
			break;
		case BUS_NVME:
			major = 259;
			minor = NVMEMinor++;
		break;
		default:
			major = 0;
			minor = 0;
	}
	vfs_block_device->major = major;
	vfs_block_device->minor = minor;

	dlist_node_t* retVal = dlist_add(kBlockDeviceDList, vfs_block_device);
	retVal->major = major;
	retVal->minor = minor;
	return retVal;
}
