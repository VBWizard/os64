#ifndef BLOCK_DEVICE_H
#define BLOCK_DEVICE_H

#include "ata.h"
#include "dlist.h"
#include "vfs.h"

	void init_block();
	dlist_node_t* add_block_device(volatile void* device, block_device_info_t* block_device);

#endif
