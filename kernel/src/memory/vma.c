#include "memory/vma.h"

#include "memory/kmalloc.h"

static inline bool vma_contains(const vma_t* vma, uintptr_t addr)
{
    return vma != NULL && addr >= vma->start && addr < vma->end;
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
