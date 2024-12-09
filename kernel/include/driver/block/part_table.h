#ifndef PART_TABLE_H
#define PART_TABLE_H

#include "vfs.h"
#include "block_device.h"

	e_part_table_type detect_partition_table_type(block_device_info_t* device);
	bool read_block_partitions(block_device_info_t* device);
	
#endif
