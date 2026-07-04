#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#include "vfs.h"

#define SEEK_SET 0 /* Seek from beginning of file.  */
#define SEEK_CUR 1 /* Seek from current position.  */
#define SEEK_END 2 /* Seek from end of file.  */


void detect_partition_filesystem_type(block_device_info_t* device, int partNumber);

#endif