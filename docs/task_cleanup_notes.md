# Task Cleanup Implementation Notes

## Resources That Need Cleanup When a Task Exits

### 1. Thread Resources
- `thread_t` structure (kmalloc)
- Kernel stack - physical pages via `allocate_memory_aligned()`
- User stack (if ring3) - physical pages via `allocate_memory_aligned()`
- Thread ID - mark as unused via `mark_TID_unused()`

### 2. Task Strings/Structures (kmalloc)
- `task->path` - path string
- `task->cwd` - current working directory string
- `task->argv` - argument array
- `task->mmaps` - dlist_t structure

### 3. VMA Resources
- Each `vma_t` structure (kmalloc)
- VMA backing pages (physical) - allocated on page fault via `vma_resolve_backing_page()`
  - Anonymous: `allocate_memory_aligned(PAGE_SIZE)`
  - File-backed: `kmalloc_aligned(PAGE_SIZE)`
- File handles for file-backed VMAs - need to close

### 4. ELF Resources
- `elf_image_t` structure (kmalloc)
- `elf->shdrs` - section headers (kmalloc)
- `elf->shstrtab` - section header string table (kmalloc)
- `elf->file` - file handle - need to close

### 5. Page Tables (NOT YET IMPLEMENTED)
- PML4 page
- PDPT pages (lower-half only)
- PD pages (lower-half only)
- PT pages (lower-half only)
- Note: Paging page pool uses bump allocator, doesn't support freeing yet

### 6. Scheduler/Task List
- Remove from `kTaskList` via `scheduler_remove_task()`
- Thread already removed from queues via `scheduler_reap_zombie_thread()`

## Cleanup Order (Important!)

1. Remove task from kTaskList (so scheduler doesn't find it)
2. Destroy thread (mark TID unused, free thread_t)
3. Free physical allocations (stacks, VMA backing pages)
4. Walk VMAs: unmap pages, free physical frames, close file handles, destroy VMA structs
5. Free ELF resources (close file, free shdrs, shstrtab, elf struct)
6. Free strings (path, cwd, argv)
7. Free task_t structure

## Arena Allocator Strategy

**Use arena for kmalloc-style allocations:**
- path, cwd, argv strings
- VMA structures
- ELF structures (elf_image_t, shdrs, shstrtab)
- mmaps dlist
- Thread structure

**Keep separate tracking for physical memory:**
- Thread stacks (physical pages)
- VMA backing pages (physical pages)

This hybrid approach:
- Arena handles "easy to forget" small allocations
- Physical allocations are explicit and fewer in number
- Simpler than a physical memory arena

## Key Functions

- `task_wait()` - should call cleanup after reaping zombie, return taskID not task_t*
- `task_cleanup()` - orchestrates all cleanup
- `thread_destroy()` - frees thread resources
- `scheduler_remove_task()` - removes from kTaskList
- `scheduler_reap_zombie_thread()` - removes thread from zombie queue

## VMA Page Walking

To free VMA backing pages:
```c
for (uintptr_t addr = vma->start; addr < vma->end; addr += PAGE_SIZE) {
    uintptr_t phys = paging_walk_paging_table((pt_entry_t*)task->pml4v, addr);
    if (phys != 0 && phys != 0xbadbadba) {
        paging_unmap_page((pt_entry_t*)task->pml4v, addr);
        free_memory(phys & ~0xFFF);  // Mask off flags
    }
}
```

## File Handle Closing Pattern

```c
vfs_file_t *file = ...;
vfs_file_operations_t *fops = file->fops;
if (fops == NULL && file->owner != NULL) {
    fops = ((vfs_filesystem_t*)file->owner)->fops;
}
if (fops != NULL && fops->close != NULL) {
    fops->close(file);
}
```
