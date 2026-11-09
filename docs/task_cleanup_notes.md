# Task Cleanup Implementation Notes

> **STATUS, 2026-08-13.** Everything below was the ORIGINAL PLAN, written before
> `task_destroy` existed. Most of it is now done; read this header first, because
> the plan below no longer describes the code.
>
> **Reclaimed at burial today:** per-thread stacks, thread_t, TID, syscall
> scratch; path, cwd; the static `elf_image_t` and its open backing file; the
> argv blob; the env blob; the ring-3 exit trampoline page; every `vma_t` and
> every mmaps/shared_objects dlist node; one `shared_object` reference; and —
> since the paging arena — the PML4 and every page table this address space ever
> drew, in one `arena_destroy`. Section 5 below ("Page Tables — NOT YET
> IMPLEMENTED") is obsolete: the pool's bump allocator has nothing to do with a
> task's tables anymore.
>
> **Still deferred — exactly one thing:** the VMA BACKING PAGES (section 3's
> physical frames). Not because they are hard to free, but because ownership is
> a ruling: a frame under a `MAP_SHARED_LIBRARY` VMA may belong to the
> shared_object page cache. That guard is already written
> (`task_frame_is_shared_object_cache`, task.c) and already exercised on every
> boot; what is missing is the general answer for the day `fork` introduces
> task-to-task sharing no registry records. Booked to the fork/CoW arc.
>
> **It is counted, not merely admitted.** Every burial walks its VMAs, books the
> genuinely-task-owned resident frames into `kTaskDeferredReclaimBytes`, and says
> so on DEBUG_TASK. The `task_teardown_leak` test (LATE phase) asserts that a
> spawn→exit→burial cycle's allocator delta equals exactly what was booked — so
> every byte is either given back or counted, and anything else is an unknown
> leak the test names and fails on.
>
> **Two corrections this doc got wrong, worth keeping visible:**
> 1. The env blob was long believed CoW-shared with children. It is not, and
>    never was — `env_inherit` does a plain memcpy. A wrong answer there would
>    have been a cross-task use-after-free, so it was verified in the code
>    before anything was freed.
> 2. The list of leaks was incomplete: the ring-3 exit trampoline page appears
>    nowhere below and was leaking 4KB per user command since it was written.
>    If you are auditing, do not trust a list — walk the page tables.

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

### 5. Page Tables (DONE 2026-08-13 — see the status header)
- PML4 page
- PDPT pages (lower-half only)
- PD pages (lower-half only)
- PT pages (lower-half only)
- ~~Note: Paging page pool uses bump allocator, doesn't support freeing yet~~
  Superseded: each task's tables come from its own arena (`task->tableArena`),
  and `arena_destroy` at burial returns all of them at once. The pool's bump
  allocator was never the obstacle it looked like — the answer was to stop
  drawing task tables from the pool at all. See PAGING_ARENA.md.

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
