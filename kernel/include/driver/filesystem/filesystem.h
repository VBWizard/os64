#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#include "vfs.h"

void detect_partition_filesystem(block_device_info_t* device, int partNumber);

#endif