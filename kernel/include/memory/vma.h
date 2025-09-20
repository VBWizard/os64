#ifndef MEMORY_VMA_H
#define MEMORY_VMA_H

#include <stdbool.h>
#include <stdint.h>

#include "dlist.h"
#include "task.h"

#ifndef PROT_READ
#define PROT_READ  0x1
#endif

#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif

#ifndef PROT_EXEC
#define PROT_EXEC  0x4
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x01
#endif

#ifndef MAP_SHARED
#define MAP_SHARED  0x02
#endif

//VMA tracking structure
typedef struct vma {
    uintptr_t start;              // Inclusive start address
    uintptr_t end;                // Exclusive end address
    int prot;                     // PROT_READ, PROT_WRITE, etc.
    int flags;                    // MAP_PRIVATE, MAP_SHARED, etc.
    void* file;                   // Optional backing file
    uint64_t file_offset;         // File offset for mmap
    bool cow;                     // Is this region CoW-enabled?
    bool loaded;                  // Has it been faulted in yet?
    dlist_node_t* listItem;       // Back-pointer to the owning task's list node
} vma_t;

vma_t* vma_create(uintptr_t start,
                  uintptr_t end,
                  int prot,
                  int flags,
                  void* file,
                  uint64_t file_offset);

void vma_add(task_t* task, vma_t* vma);

vma_t* vma_lookup(task_t* task, uintptr_t addr);

void vma_destroy(vma_t* vma);

#endif // MEMORY_VMA_H
