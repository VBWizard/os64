#ifndef MEMORY_TASK_ARENA_H
#define MEMORY_TASK_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "task.h"

/// Task arena allocator for per-task memory allocations.
///
/// This arena allocates physical memory and maps it into a task's PML4,
/// making it accessible to the task when running. The kernel can also
/// access this memory during task initialization via the HHDM.
///
/// Key features:
/// - Memory is mapped into task's address space (lower half)
/// - Kernel can write to it via HHDM during initialization
/// - All allocations freed together when arena is destroyed
/// - Physical memory is properly tracked for cleanup

typedef struct task_arena {
    uint8_t *task_buffer;    // Virtual address in task's address space
    uint8_t *kernel_buffer;  // HHDM address for kernel access during init
    uintptr_t phys_base;     // Physical base address (for cleanup)
    size_t capacity;         // Total capacity in bytes
    size_t offset;           // Current allocation offset
    task_t *owner;           // Owning task (for mapping)
} task_arena_t;

/// @brief Create a new task arena mapped into the task's address space
/// @param task The task that will own this arena
/// @param capacity Size in bytes for the arena
/// @return Pointer to new task_arena, or NULL on failure
task_arena_t *task_arena_create(task_t *task, size_t capacity);

/// @brief Allocate memory from the task arena
/// @param arena The arena to allocate from
/// @param size Number of bytes to allocate
/// @return Task-space virtual address, or NULL if arena is exhausted
void *task_arena_alloc(task_arena_t *arena, size_t size);

/// @brief Allocate aligned memory from the task arena
/// @param arena The arena to allocate from
/// @param size Number of bytes to allocate
/// @param alignment Required alignment (must be power of 2)
/// @return Task-space virtual address (aligned), or NULL if arena is exhausted
void *task_arena_alloc_aligned(task_arena_t *arena, size_t size, size_t alignment);

/// @brief Convert a task-space pointer to a kernel-accessible pointer
/// @param arena The arena the pointer belongs to
/// @param task_ptr A pointer previously returned by task_arena_alloc
/// @return Kernel-accessible (HHDM) pointer to the same memory
void *task_arena_to_kernel_ptr(task_arena_t *arena, void *task_ptr);

/// @brief Convert a kernel pointer back to task-space pointer
/// @param arena The arena the pointer belongs to
/// @param kernel_ptr A kernel-accessible pointer
/// @return Task-space pointer to the same memory
void *task_arena_to_task_ptr(task_arena_t *arena, void *kernel_ptr);

/// @brief Reset the arena to reuse its memory
/// @param arena The arena to reset
void task_arena_reset(task_arena_t *arena);

/// @brief Destroy the arena and free all its memory
/// @param arena The arena to destroy
/// @note This unmaps from task's PML4 and frees physical memory
void task_arena_destroy(task_arena_t *arena);

/// @brief Get remaining capacity in the arena
/// @param arena The arena to query
/// @return Number of bytes still available
size_t task_arena_remaining(task_arena_t *arena);

#endif // MEMORY_TASK_ARENA_H
