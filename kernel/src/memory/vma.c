#include "memory/vma.h"
#include "memory/mmap.h"
#include "memory/kmalloc.h"
#include "paging.h"
#include "panic.h"
#include "filesystem/filesystem.h"

static inline bool vma_contains(const vma_t* vma, uintptr_t addr)
{
    return vma != NULL && addr >= vma->start && addr < vma->end;
}

// Resolves and returns the physical address of a page for the given faulting address.
// This function handles anonymous and file-backed memory only (CoW later).
uintptr_t vma_resolve_backing_page(vma_t *vma, uintptr_t fault_addr)
{
    uintptr_t page_offset = (fault_addr & ~(PAGE_SIZE - 1)) - vma->start;
    uintptr_t phys = 0;

    if ((vma->flags & MAP_ANONYMOUS) || vma->file == NULL)
    {
        void *virt = kmalloc_aligned(PAGE_SIZE); // Page-aligned for direct mapping
        phys = (uintptr_t)virt - kHHDMOffset;
    }
    else
    {
        vfs_file_t *file = (vfs_file_t *)vma->file;
        vfs_file_operations_t *fops = NULL;
        uint64_t file_offset = vma->file_offset + page_offset;

        if (file == NULL)
            panic("vma_resolve_backing_page: Null file backing");

        if (file->fops != NULL)
        {
            fops = file->fops;
        }
        else if (file->owner != NULL)
        {
            fops = ((vfs_filesystem_t *)file->owner)->fops;
        }

        if (fops == NULL || fops->read == NULL || fops->seek == NULL)
            panic("vma_resolve_backing_page: File ops not available for backing");

        // Allocate a physical page
        void *virt = kmalloc_aligned(PAGE_SIZE);
        if (!virt)
            panic("Failed to allocate page for file-backed VMA");

        phys = (uintptr_t)virt - kHHDMOffset;

        if (fops->seek(file, (long)file_offset, SEEK_SET) < 0)
            panic("vma_resolve_backing_page: Failed to seek file backing");

        int bytes_read = fops->read(file, virt, PAGE_SIZE);
        if (bytes_read < 0)
            panic("vma_resolve_backing_page: Failed to read file backing");
    }
    return phys;
}

vma_t* vma_create(uintptr_t start,
                  uintptr_t end,
                  int prot,
                  int flags,
                  void* file,
                  uint64_t file_offset)
{
    vma_t* vma = kmalloc(sizeof(vma_t));
    if (!vma) {
        return NULL;
    }

    vma->start = start;
    vma->end = end;
    vma->prot = prot;
    vma->flags = flags;
    vma->file = file;
    vma->file_offset = file_offset;
    vma->cow = false;
    vma->loaded = false;
    vma->listItem = NULL;

    return vma;
}

/// @brief Add a VMA to a task's memory map list
/// @param task A task_t struct pointer
/// @param vma A vma_t struct pointer
void vma_add(task_t* task, vma_t* vma)
{
    if (!task || !vma) {
        return;
    }

    if (task->mmaps == NULL) {
        task->mmaps = kmalloc(sizeof(dlist_t));
        if (!task->mmaps) {
            return;
        }
        dlist_init(task->mmaps);
    }

    vma->listItem = dlist_add(task->mmaps, vma);
}

/// @brief Find a specific memory map for a given task based on address
/// @param task A task_t struct pointer
/// @param addr An address within the memory map to find
/// @return 
vma_t* vma_lookup(task_t* task, uintptr_t addr)
{
    if (!task || !task->mmaps) {
        return NULL;
    }

    dlist_node_t* node = task->mmaps->head;
    while (node) {
        vma_t* current = (vma_t*)node->data;
        if (vma_contains(current, addr)) {
            return current;
        }
        node = node->next;
    }

    return NULL;
}

/// @brief Free a vma_t
/// @param vma A vma_t struct pointer
void vma_destroy(vma_t* vma)
{
    if (!vma) {
        return;
    }

    kfree(vma);
}
