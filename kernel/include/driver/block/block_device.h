#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "ata.h"
#include "dlist.h"
#include "vfs.h"

extern dlist_t* kBlockDeviceDList;

	void init_block();
	dlist_node_t* add_block_device(vfs_filesystem_t* vfs_block_device);

#endif
