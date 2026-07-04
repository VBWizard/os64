#ifndef MMAP_H
#define MMAP_H

// Protection flags
#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

// Mapping flags
#define MAP_PRIVATE 0x01
#define MAP_SHARED 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10
#define MAP_STACK 0x20000
#define MAP_POPULATE 0x8000 // Optional, for future eager page mapping

#endif
