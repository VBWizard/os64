#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "ata.h"
#include "dlist.h"
#include "vfs.h"

extern dlist_t* kBlockDeviceDList;

	void init_block();
	dlist_node_t* add_block_device(vfs_filesystem_t* vfs_block_device);
	// Stray-write tripwire: panics unless [sector, sector+count) lies fully
	// inside a partition whose MOUNTED filesystem installed a write path
	// (per-mount fops — a read-only mount refuses even when the driver could
	// write). Every bops->write implementation calls this before moving
	// bytes — see block_device.c for the corruption that earned it.
	void block_verify_write_allowed(block_device_info_t* device, uint64_t sector, uint64_t count);

	// Assign the system's bus+ordinal device names ("nvme0") — once, after
	// enumeration; /sys/block and the future /dev nodes speak these.
	void block_assign_dev_names(void);

#endif
