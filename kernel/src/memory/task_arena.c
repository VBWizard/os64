#include "memory/task_arena.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "allocator.h"
#include "memory/paging.h"
#include "task.h"
#include "serial_logging.h"
#include "CONFIG.h"

extern uint64_t kHHDMOffset;
extern uintptr_t kKernelPML4v;

task_arena_t *task_arena_create(task_t *task, size_t capacity)
{
    if (task == NULL || capacity == 0) {
        printd(DEBUG_TASK, "task_arena_create: Invalid parameters (task=%p, capacity=%lu)\n",
               task, capacity);
        return NULL;
    }

    // Round capacity up to page boundary
    size_t page_count = (capacity + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t aligned_capacity = page_count * PAGE_SIZE;

    // Allocate buffer via kmalloc_aligned - this guarantees HHDM accessibility
    // Note: kmalloc memory is always mapped in kKernelPML4 via HHDM
    uint8_t *kernel_buffer = kmalloc_aligned(aligned_capacity);
    if (kernel_buffer == NULL) {
        printd(DEBUG_TASK, "task_arena_create: Failed to allocate %lu bytes via kmalloc\n",
               aligned_capacity);
        return NULL;
    }

    // Get physical address by walking kernel page tables
    uintptr_t phys = paging_walk_paging_table((pt_entry_t *)kKernelPML4v, (uintptr_t)kernel_buffer);
    if (phys == 0) {
        printd(DEBUG_TASK, "task_arena_create: Failed to get physical address for buffer\n");
        kfree(kernel_buffer);
        return NULL;
    }

    // Get virtual address in task's address space
    uintptr_t task_virt = task->taskMemoryNextVirt;

    // Map into task's PML4 (in addition to kernel PML4 where it's already mapped)
    paging_map_pages(task->pml4v, task_virt, phys, page_count, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    // Update task's next virtual address
    task->taskMemoryNextVirt += aligned_capacity;

    // Allocate the arena structure itself (kernel-only, use kmalloc)
    task_arena_t *arena = kmalloc(sizeof(task_arena_t));
    if (arena == NULL) {
        printd(DEBUG_TASK, "task_arena_create: Failed to allocate arena structure\n");
        // Unmap from task's PML4 and free buffer
        for (size_t i = 0; i < page_count; i++) {
            paging_unmap_page((pt_entry_t *)task->pml4v, task_virt + (i * PAGE_SIZE));
        }
        kfree(kernel_buffer);
        return NULL;
    }

    arena->task_buffer = (uint8_t *)task_virt;
    arena->kernel_buffer = kernel_buffer;
    arena->phys_base = phys;
    arena->capacity = aligned_capacity;
    arena->offset = 0;
    arena->owner = task;

    // Zero the buffer via kernel pointer (safe since kmalloc guarantees HHDM access)
    memset(arena->kernel_buffer, 0, aligned_capacity);

    printd(DEBUG_TASK, "task_arena_create: Created arena for task %s, capacity=%lu, task_virt=0x%lx, kernel_virt=0x%lx, phys=0x%lx\n",
           task->exename, aligned_capacity, task_virt, (uintptr_t)kernel_buffer, phys);

    return arena;
}

void *task_arena_alloc(task_arena_t *arena, size_t size)
{
    if (arena == NULL || size == 0) {
        return NULL;
    }

    // Check if we have enough space
    if (arena->offset + size > arena->capacity) {
        printd(DEBUG_TASK, "task_arena_alloc: Arena exhausted, requested %lu, available %lu\n",
               size, arena->capacity - arena->offset);
        return NULL;
    }

    // Bump allocate - return task-space pointer
    void *ptr = arena->task_buffer + arena->offset;
    arena->offset += size;

    printd(DEBUG_TASK | DEBUG_DETAILED, "task_arena_alloc: Allocated %lu bytes at task_ptr=0x%lx, offset now %lu/%lu\n",
           size, (uintptr_t)ptr, arena->offset, arena->capacity);

    return ptr;
}

void *task_arena_alloc_aligned(task_arena_t *arena, size_t size, size_t alignment)
{
    if (arena == NULL || size == 0 || alignment == 0) {
        return NULL;
    }

    // Alignment must be power of 2
    if ((alignment & (alignment - 1)) != 0) {
        printd(DEBUG_TASK, "task_arena_alloc_aligned: Alignment %lu is not a power of 2\n", alignment);
        return NULL;
    }

    // Calculate aligned offset using task buffer address
    uintptr_t current = (uintptr_t)(arena->task_buffer + arena->offset);
    uintptr_t aligned = (current + alignment - 1) & ~(alignment - 1);
    size_t padding = aligned - current;

    // Check if we have enough space (including padding)
    if (arena->offset + padding + size > arena->capacity) {
        printd(DEBUG_TASK, "task_arena_alloc_aligned: Arena exhausted, requested %lu (with %lu padding), available %lu\n",
               size, padding, arena->capacity - arena->offset);
        return NULL;
    }

    // Bump allocate with alignment
    arena->offset += padding + size;
    void *ptr = (void *)aligned;

    printd(DEBUG_TASK | DEBUG_DETAILED, "task_arena_alloc_aligned: Allocated %lu bytes (alignment=%lu, padding=%lu) at task_ptr=0x%lx, offset now %lu/%lu\n",
           size, alignment, padding, (uintptr_t)ptr, arena->offset, arena->capacity);

    return ptr;
}

void *task_arena_to_kernel_ptr(task_arena_t *arena, void *task_ptr)
{
    if (arena == NULL || task_ptr == NULL) {
        return NULL;
    }

    // Calculate offset from task buffer base
    uintptr_t offset = (uintptr_t)task_ptr - (uintptr_t)arena->task_buffer;

    // Bounds check
    if (offset >= arena->capacity) {
        printd(DEBUG_TASK, "task_arena_to_kernel_ptr: Pointer 0x%lx is outside arena bounds\n",
               (uintptr_t)task_ptr);
        return NULL;
    }

    return arena->kernel_buffer + offset;
}

void *task_arena_to_task_ptr(task_arena_t *arena, void *kernel_ptr)
{
    if (arena == NULL || kernel_ptr == NULL) {
        return NULL;
    }

    // Calculate offset from kernel buffer base
    uintptr_t offset = (uintptr_t)kernel_ptr - (uintptr_t)arena->kernel_buffer;

    // Bounds check
    if (offset >= arena->capacity) {
        printd(DEBUG_TASK, "task_arena_to_task_ptr: Pointer 0x%lx is outside arena bounds\n",
               (uintptr_t)kernel_ptr);
        return NULL;
    }

    return arena->task_buffer + offset;
}

void task_arena_reset(task_arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    printd(DEBUG_TASK, "task_arena_reset: Resetting arena, was at offset %lu\n", arena->offset);

    // Zero the used portion via kernel pointer
    memset(arena->kernel_buffer, 0, arena->offset);

    arena->offset = 0;
}

void task_arena_destroy(task_arena_t *arena)
{
    if (arena == NULL) {
        return;
    }

    printd(DEBUG_TASK, "task_arena_destroy: Destroying arena, capacity=%lu, used=%lu, phys=0x%lx\n",
           arena->capacity, arena->offset, arena->phys_base);

    // Unmap from task's PML4
    if (arena->owner != NULL && arena->owner->pml4v != NULL) {
        size_t page_count = arena->capacity / PAGE_SIZE;
        for (size_t i = 0; i < page_count; i++) {
            paging_unmap_page((pt_entry_t *)arena->owner->pml4v,
                              (uintptr_t)arena->task_buffer + (i * PAGE_SIZE));
        }
    }

    // Free buffer memory (allocated via kmalloc, so use kfree)
    if (arena->kernel_buffer != NULL) {
        kfree(arena->kernel_buffer);
    }

    // Free the arena structure itself
    kfree(arena);
}

size_t task_arena_remaining(task_arena_t *arena)
{
    if (arena == NULL) {
        return 0;
    }

    return arena->capacity - arena->offset;
}
