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

// Marks a VMA as backed by a shared_object_t (a dynamically-linked library
// or executable's demand-paged segment) rather than a plain vfs_file_t.
// When set, `vma->file` is actually a `shared_object_t*` — see
// shared_object.h and the page-fault handler in simple_exceptions.c, which
// checks this flag to route resolution through the per-library page cache
// (shared_object_resolve_page) instead of the ordinary per-VMA file read.
#ifndef MAP_SHARED_LIBRARY
#define MAP_SHARED_LIBRARY 0x04
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

uintptr_t vma_resolve_backing_page(vma_t *vma, uintptr_t fault_addr);

void vma_destroy(vma_t* vma);

// Runs `func(arg)` with kKernelPML4 and the core's kernel interrupt stack
// loaded instead of whatever task context called this — needed whenever
// code invoked from inside a page-fault handler must touch kernel-only
// resources (e.g. a VFS/disk driver's own buffers or MMIO, which aren't
// necessarily mapped into an arbitrary task's own page tables). `arg` must
// point to kmalloc'd (HHDM-accessible) memory, never a task-stack address —
// see vma.c's kernel_read_file for the reference use of this pattern.
void call_in_kernel_context(void (*func)(void*), void *arg);

#endif // MEMORY_VMA_H
