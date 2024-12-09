#include "block_device.h"
#include "kmalloc.h"

dlist_t* kBlockDeviceDList;
int SATAMinor=0, NVMEMinor=0;

void init_block()
{
	kBlockDeviceDList = kmalloc(sizeof(dlist_t));
}

dlist_node_t* add_block_device(volatile void* device, block_device_info_t* block_device)
{
	int major = 0;
	int minor = 0;

	switch (block_device->bus)
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
	dlist_node_t* retVal = dlist_add(kBlockDeviceDList, device);
	retVal->major = major;
	retVal->minor = minor;
	return retVal;
}

