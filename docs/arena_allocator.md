# Arena Allocator System

This document describes the arena (bump) allocator system in os64, covering both the general-purpose `arena_t` and the task-specific `task_arena_t`.

## Overview

Arena allocators provide fast, bulk-freeable memory allocation. Instead of tracking individual allocations for freeing, all memory allocated from an arena is freed together when the arena is destroyed or reset. This pattern is ideal for:

- Task initialization data (argv, envp, auxiliary vectors)
- Temporary allocations with well-defined lifetimes
- Reducing fragmentation in long-running systems
- Simplifying cleanup code (one free instead of many)

## Components

### 1. Basic Arena (`arena_t`)

**Files:**
- `kernel/include/memory/arena.h`
- `kernel/src/memory/arena.c`

**Purpose:** General-purpose arena for kernel-space allocations. Memory is allocated via `kmalloc()` and is only accessible from kernel context.

**Structure:**
```c
typedef struct arena {
    uint8_t *buffer;      // Base of allocated buffer
    size_t capacity;      // Total capacity in bytes
    size_t offset;        // Current allocation offset (next free byte)
} arena_t;
```

**API:**
| Function | Description |
|----------|-------------|
| `arena_create(capacity)` | Create arena with specified capacity |
| `arena_alloc(arena, size)` | Bump-allocate `size` bytes |
| `arena_alloc_aligned(arena, size, alignment)` | Allocate with alignment (power of 2) |
| `arena_reset(arena)` | Reset offset to 0, reuse memory |
| `arena_destroy(arena)` | Free all memory |
| `arena_remaining(arena)` | Query remaining capacity |

**Use Cases:**
- Kernel-only temporary allocations
- Building data structures that will be freed together
- Parsing operations with temporary buffers

### 2. Task Arena (`task_arena_t`)

**Files:**
- `kernel/include/memory/task_arena.h`
- `kernel/src/memory/task_arena.c`

**Purpose:** Arena for per-task memory that needs to be accessible from both kernel (during initialization) and task (during execution) contexts.

**Structure:**
```c
typedef struct task_arena {
    uint8_t *task_buffer;    // Virtual address in task's address space
    uint8_t *kernel_buffer;  // Kernel-accessible address (HHDM)
    uintptr_t phys_base;     // Physical base address
    size_t capacity;         // Total capacity in bytes
    size_t offset;           // Current allocation offset
    task_t *owner;           // Owning task
} task_arena_t;
```

**API:**
| Function | Description |
|----------|-------------|
| `task_arena_create(task, capacity)` | Create arena mapped into task's address space |
| `task_arena_alloc(arena, size)` | Allocate, returns task-space pointer |
| `task_arena_alloc_aligned(arena, size, alignment)` | Aligned allocation |
| `task_arena_to_kernel_ptr(arena, task_ptr)` | Convert task pointer to kernel pointer |
| `task_arena_to_task_ptr(arena, kernel_ptr)` | Convert kernel pointer to task pointer |
| `task_arena_reset(arena)` | Reset for reuse |
| `task_arena_destroy(arena)` | Unmap from task, free memory |
| `task_arena_remaining(arena)` | Query remaining capacity |

**Use Cases:**
- Task argv/argc arrays
- Task environment (envp)
- ELF auxiliary vectors
- Any data the kernel prepares that the task needs to read

## Design Decisions

### Why Two Arena Types?

1. **Basic arena** is simpler and more efficient for kernel-only data
2. **Task arena** handles the complexity of dual address spaces

Combining them would add unnecessary overhead to the common case (kernel-only allocations).

### Task Arena Memory Allocation Strategy

**Problem:** Memory allocated with `allocate_memory_aligned()` may return physical addresses not covered by Limine's HHDM (Higher-Half Direct Map), particularly in low memory regions (first 1MB).

**Solution:** Use `kmalloc_aligned()` which guarantees HHDM accessibility, then derive the physical address via page table walking:

```c
// Allocate via kmalloc (guaranteed HHDM access)
uint8_t *kernel_buffer = kmalloc_aligned(capacity);

// Get physical address by walking kernel page tables
uintptr_t phys = paging_walk_paging_table(kKernelPML4v, kernel_buffer);

// Map into task's PML4
paging_map_pages(task->pml4v, task_virt, phys, page_count, flags);
```

**Trade-off:** Memory remains mapped in kernel PML4 (via HHDM) even after task creation. This slightly reduces isolation but ensures reliable kernel access during initialization.

**Alternative considered:** Using `allocate_memory_aligned()` directly and temporary kernel mappings. Rejected because:
- More complex cleanup paths
- Risk of forgetting to unmap temporary mappings
- HHDM coverage issues with low memory

### Dual Pointer Design

The task arena maintains two pointers to the same physical memory:

1. `task_buffer` - Virtual address in task's lower-half address space
2. `kernel_buffer` - HHDM address for kernel access

This allows:
- Kernel to write initialization data via `kernel_buffer`
- Task to read that data via `task_buffer` after context switch
- Pointer conversion between the two spaces

### Capacity Rounding

Task arena capacities are always rounded up to page boundaries because:
1. Page mapping granularity requires full pages
2. Prevents partial page mapping issues
3. Simplifies cleanup (unmap whole pages)

## Memory Layout

```
Task Address Space (lower half):
+------------------+ task->taskMemoryNextVirt (before)
| Task Arena       |
| (PAGE_SIZE * N)  |  <- task_buffer points here
+------------------+ task->taskMemoryNextVirt (after)

Kernel Address Space (HHDM):
+------------------+ kernel_buffer (= phys + kHHDMOffset)
| Same Physical    |
| Memory           |
+------------------+
```

## Pointer Conversion: Why and When

The `task_arena_to_kernel_ptr()` and `task_arena_to_task_ptr()` functions solve a fundamental problem: **the kernel and task see the same physical memory at different virtual addresses**.

### The Problem

When the kernel creates a task, it needs to set up data structures (argv, envp, etc.) that the task will read after it starts running. However:

1. The kernel runs with `kKernelPML4` loaded in CR3
2. The task will run with `task->pml4v` loaded in CR3
3. The same physical memory is mapped at **different virtual addresses** in each

```
Physical Memory:        Kernel View:              Task View:
+-------------+        +------------------+      +------------------+
| 0x00123000  |  <--   | 0xffff800000123  |      | 0x10001000       |
| (the data)  |        | (HHDM address)   |      | (task address)   |
+-------------+        +------------------+      +------------------+
```

### The Solution

`task_arena_alloc()` returns **task-space pointers** because that's what the task will use. But the kernel needs to **write** to that memory before the task runs. So:

1. **Allocate** - get task-space pointer (what the task will use)
2. **Convert** - get kernel-space pointer (what the kernel can write to)
3. **Write** - kernel writes data via kernel pointer
4. **Store** - any pointers stored in the data must be task-space pointers

### Conversion Functions

**`task_arena_to_kernel_ptr(arena, task_ptr)`**
- Input: A pointer in task's address space (returned by `task_arena_alloc`)
- Output: The corresponding kernel-accessible pointer (HHDM address)
- Use: When the kernel needs to read/write memory that was allocated for the task

**`task_arena_to_task_ptr(arena, kernel_ptr)`**
- Input: A kernel pointer within the arena's kernel_buffer
- Output: The corresponding task-space pointer
- Use: Less common; useful if you're working with kernel pointers and need to store a reference the task can use

### Detailed Example: Setting Up argv

```c
// Kernel is setting up a task that will run: /bin/echo hello world
// argc = 3, argv should be: ["/bin/echo", "hello", "world", NULL]

task_arena_t *arena = task_arena_create(task, 4096);

// Step 1: Allocate the argv array (4 pointers: 3 args + NULL terminator)
// task_arena_alloc returns a TASK-SPACE pointer
char **argv = task_arena_alloc(arena, 4 * sizeof(char *));
// argv = 0x10001000 (task-space address)

// Step 2: Convert to kernel pointer so we can write to it
char **argv_k = task_arena_to_kernel_ptr(arena, argv);
// argv_k = 0xffff800000123000 (kernel HHDM address)

// Step 3: Allocate and fill each argument string
const char *args[] = {"/bin/echo", "hello", "world"};
for (int i = 0; i < 3; i++) {
    // Allocate space for this string (returns task-space pointer)
    char *str = task_arena_alloc(arena, strlen(args[i]) + 1);
    // str = 0x10001020 (task-space)

    // Convert to kernel pointer to write the string
    char *str_k = task_arena_to_kernel_ptr(arena, str);
    // str_k = 0xffff800000123020 (kernel HHDM)

    // Kernel writes the string data
    strcpy(str_k, args[i]);

    // Store the TASK-SPACE pointer in argv
    // (because the task will read argv, not argv_k)
    argv_k[i] = str;  // NOT str_k!
}
argv_k[3] = NULL;  // NULL terminator

// Step 4: Pass task-space argv pointer to the task
task->argv = argv;  // Task will use this address

// When task runs and reads argv[0], it will:
// 1. Read from address 0x10001000 (argv)
// 2. Get value 0x10001020 (the task-space string pointer)
// 3. Read string from 0x10001020 -> "/bin/echo"
```

### Common Mistakes

**Mistake 1: Storing kernel pointers in task data**
```c
// WRONG: Task can't read kernel addresses!
argv_k[i] = str_k;

// CORRECT: Store task-space pointers
argv_k[i] = str;
```

**Mistake 2: Writing via task pointer from kernel**
```c
// WRONG: Kernel can't write to task addresses (different CR3)!
strcpy(str, args[i]);

// CORRECT: Convert to kernel pointer first
strcpy(str_k, args[i]);
```

**Mistake 3: Forgetting which pointer is which**
```c
// Use clear naming conventions:
char *str = ...;    // task-space (what task uses)
char *str_k = ...;  // kernel-space (what kernel uses to write)
```

## Usage Example

```c
// Create arena during task initialization
task_arena_t *arena = task_arena_create(task, 8192);

// Allocate argv array (returns task-space pointer)
char **argv = task_arena_alloc(arena, argc * sizeof(char *));

// Get kernel pointer to write data
char **argv_k = task_arena_to_kernel_ptr(arena, argv);

// Kernel writes to argv_k, task later reads from argv
for (int i = 0; i < argc; i++) {
    char *arg = task_arena_alloc(arena, strlen(args[i]) + 1);
    char *arg_k = task_arena_to_kernel_ptr(arena, arg);
    strcpy(arg_k, args[i]);
    argv_k[i] = arg;  // Store task-space pointer
}

// Later, during task cleanup:
task_arena_destroy(arena);
```

## Testing

Tests are in `kernel/test/test_main.c`:

| Test | Description |
|------|-------------|
| `arena_create_and_destroy` | Basic arena lifecycle |
| `arena_basic_alloc` | Sequential allocations |
| `arena_aligned_alloc` | Alignment guarantees |
| `arena_reset` | Reset and reuse |
| `arena_exhaustion` | Capacity limits |
| `task_arena_create_and_destroy` | Task arena lifecycle |
| `task_arena_alloc_and_convert` | Pointer conversion |
| `task_arena_aligned_alloc` | Aligned task allocations |
| `task_arena_exhaustion` | Task arena limits |

All tests run in the pre-boot phase using the kernel task.

## Future Considerations

1. **Sub-arenas:** Allow creating child arenas that can be reset independently
2. **Statistics:** Track peak usage, allocation counts for debugging
3. **Guard pages:** Optional guard pages between arenas for overflow detection
4. **Memory isolation:** Investigate whether task arena memory can be fully isolated from kernel PML4 while maintaining reliable initialization access

## Related Documentation

- `docs/task_cleanup_notes.md` - Task cleanup implementation notes
- `CLAUDE.md` - Memory management overview and coding patterns
