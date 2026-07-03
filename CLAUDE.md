# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

os64 is a 64-bit x86 operating system kernel built for educational purposes. It uses the Limine bootloader protocol and boots on QEMU (or real hardware) in both BIOS and UEFI modes.

**Key Technologies:**
- Bootloader: Limine (v8.x)
- Toolchain: x86_64-elf-gcc cross-compiler
- Architecture: x86_64 (long mode)
- Memory model: Higher-half kernel (0xffffffff80000000)
- Build system: GNU Make

## Building and Running

### Prerequisites
- `x86_64-elf-gcc` cross-compiler
- `x86_64-elf-ld` linker
- `make`
- `nasm` (for assembly files)
- `qemu-system-x86_64` (for testing)
- `xorriso` (for ISO creation)

### Common Commands

**Build the kernel and create bootable ISO:**
```bash
make
```
This builds `kernel/bin/os64_kernel` and creates `os64_kernel.iso`.

**Run in QEMU (BIOS mode):**
```bash
make run
```

**Run in QEMU (UEFI mode):**
```bash
make run-uefi
```

**Debug with GDB:**
```bash
make debug
```
This starts QEMU with GDB server on port 1234 (use `-S -s` flags). Connect with:
```bash
gdb kernel/bin/os64_kernel
(gdb) target remote :1234
```

**Clean build artifacts:**
```bash
make clean
```

**Clean everything including dependencies:**
```bash
make distclean
```

### Kernel-only Build

To rebuild just the kernel without creating an ISO:
```bash
make -C kernel
```

To build the test ELF binary:
```bash
make -C kernel test-elf
```

### Build System Architecture

- **Root Makefile** (`GNUmakefile`): Orchestrates kernel build, ISO creation, disk image setup
- **Kernel Makefile** (`kernel/GNUmakefile`): Builds kernel binary and test programs
- **Parallel builds**: Uses `-j$(nproc)` automatically in kernel makefile
- **Dependencies**: Run `kernel/get-deps` to fetch Limine headers and C runtime

## Kernel Architecture

### Boot Flow

1. **Limine bootloader** loads kernel at higher-half address (0xffffffff80000000)
2. **Entry point**: `limine_boot_entry_point()` in `kernel/src/main.c`
   - Sets up initial stack
   - Verifies Limine protocol responses
   - Extracts bootloader-provided info (memory map, framebuffer, ACPI tables, etc.)
   - Calls `kernel_main()`
3. **kernel_main()** in `kernel/src/kernel.c`:
   - Initializes serial logging
   - Sets up paging and memory management
   - Switches to kernel stack
   - Calls `kernel_init()`
4. **kernel_init()**:
   - Initializes ACPI, PCI, storage drivers (AHCI/NVMe)
   - Initializes SMP (multi-core support)
   - Creates kernel task and idle tasks
   - Initializes scheduler
   - Runs pre-boot and post-boot tests
   - Loads and runs ELF test program from filesystem

### Memory Management

**Paging (`kernel/src/memory/paging.c`):**
- 4-level paging (PML4 → PDPT → PD → PT)
- Higher-half direct mapping (HHDM) provided by Limine
- Kernel mapped at 0xffffffff80000000
- Page size: 4KB (0x1000)
- Functions:
  - `paging_map_page()` / `paging_map_pages()`: Map virtual to physical
  - `paging_unmap_page()` / `paging_unmap_pages()`: Remove mappings
  - `paging_walk_paging_table()`: Walk page tables to resolve virtual address

**Physical Memory Allocator (`kernel/src/memory/allocator.c`):**
- Bitmap-based allocator
- Tracks free/used physical pages
- Uses Limine memory map to determine usable memory

**Heap Allocator (`kernel/src/memory/kmalloc.c`):**
- Kernel-space malloc/free
- `kmalloc()`, `kmalloc_aligned()`, `kfree()`

**Virtual Memory Areas (`kernel/src/memory/vma.c`):**
- Manages process memory regions (code, data, heap, etc.)
- Supports demand paging and file-backed mappings

### Process/Task Management

**Tasks (`kernel/include/task.h`, `kernel/src/task.c`):**
- A task represents a process
- Each task has one or more threads
- Contains: page tables (PML4), heap bounds, environment variables, file descriptors
- Task creation: `task_create()` - can load ELF binaries from filesystem
- Task exit: `task_exit()` - marks task as exited, moves to zombie state

**Threads (`kernel/include/thread.h`, `kernel/src/thread.c`):**
- Lightweight execution units within a task
- Each thread has saved register state and stack

**Scheduler (`kernel/include/scheduler.h`, `kernel/src/scheduler.c`):**
- Multi-core cooperative scheduler with preemption via timer interrupts
- Thread states: RUNNING, RUNNABLE, USLEEP, ISLEEP, STOPPED, ZOMBIE
- Queues: `qRunning`, `qRunnable`, `qUSleep`, `qISleep`, `qStopped`, `qZombie`
- Each CPU core has its own idle task
- Functions:
  - `scheduler_init()`: Initialize scheduler
  - `scheduler_enable()`: Enable scheduling
  - `scheduler_submit_new_task()`: Add task to runnable queue
  - `scheduler_change_thread_queue()`: Move thread between queues
  - `scheduler_yield()`: Voluntarily yield CPU

### Driver Subsystems

**Block Devices (`kernel/src/driver/block/`):**
- GPT partition table support (`gpt.c`)
- Block device abstraction layer (`block_device.c`)
- Registered devices tracked in `kBlockDeviceDList`

**Storage Drivers (`kernel/src/driver/system/`):**
- **AHCI** (`ahci.c`): SATA hard drive/SSD support
- **NVMe** (`nvme.c`): NVMe SSD support
- Initialization can be disabled via kernel cmdline (`noahci`, `nonvme`)

**Filesystem Support (`kernel/src/driver/filesystem/`):**
- **VFS layer** (`vfs/`): Virtual filesystem abstraction
  - File operations: open, read, write, seek, close
  - Directory operations: open, read, close, mkdir
- **FAT** (`fat/`): FAT12/16/32 support
- **ext2** (`ext2/`): ext2 filesystem support
- Root filesystem mounted via `ROOTPARTUUID` kernel cmdline parameter

**System Drivers:**
- **PCI** (`pci.c`): PCI device enumeration and configuration
- **ACPI** (`acpi.c`): ACPI table parsing (RSDP, MADT, MCFG, etc.)
- **APIC** (`apic.c`): Local APIC and I/O APIC management
- **RTC** (`rtc.c`): Real-time clock
- **PIT** (`pit.c`): Programmable Interval Timer
- **Keyboard** (`keyboard.c`): PS/2 keyboard driver

### SMP (Symmetric Multiprocessing)

**SMP Initialization (`kernel/src/smp.c`, `kernel/src/smp_core.c`):**
- Bootstrap processor (BSP) brings up application processors (APs)
- Each core gets its own:
  - GDT, IDT, TSS
  - Core-local storage (CLS) structure
  - Idle task
- Inter-processor interrupts (IPIs) for scheduler coordination
- Can be disabled via `nosmp` kernel cmdline

**Core-Local Storage:**
- Accessed via `get_core_local_storage()`
- Contains: APIC ID, current task/thread, CPU info, TSS, etc.

### ELF Loading

**ELF Loader (`kernel/src/elf_loader.c`):**
- Loads ELF64 binaries from VFS
- Supports program headers (PT_LOAD segments)
- Maps code/data sections into task's address space
- Demand paging: pages faulted in on access
- Entry point: `kernel/test/elf/serial_ping.S` (test ELF that writes to serial port)

### Interrupt Handling

**IDT (`kernel/src/driver/system/idt.c`):**
- Interrupt Descriptor Table setup
- Exception handlers in `kernel/src/driver/system/exceptions/`
- IRQ handlers:
  - IRQ0 (timer): `handler_irq0_timer.S` - scheduler tick
  - IRQ1 (keyboard): `handler_irq1_keyboard.S`

**Signals (`kernel/src/signals.c`):**
- Signal infrastructure (not fully POSIX-compliant yet)
- `init_signals()`, `signal_handler()`

### Configuration

**Kernel Configuration (`kernel/include/CONFIG.h`):**
- **Timing**: `TICKS_PER_SECOND` = 100 (10ms per tick)
- **Memory**: `PAGE_SIZE` = 0x1000, `KERNEL_STACK_SIZE` = 20 pages
- **Scheduler**: `MP_SCHEDULER_RUNS_PER_SECOND` = 10
- **Debug levels**: Bit flags for different subsystems (BOOT, SMP, PCI, SCHEDULER, etc.)
  - Default: `DEBUG_OPTIONS` in CONFIG.h
  - Override via kernel cmdline

**Kernel Command Line:**
- Parsed in `kernel/src/kernel_commandline.c`
- Common options:
  - `ROOTPARTUUID=<uuid>`: Mount partition as root
  - `nosmp`: Disable multicore support
  - `noahci`: Disable AHCI driver
  - `nonvme`: Disable NVMe driver
  - `nolog` / `noseriallog` / `alllog`: Control logging

### Logging and Debugging

**Serial Logging (`kernel/src/serial_logging.c`):**
- Outputs to COM1 (0x3F8)
- QEMU redirects to `qemu_com1.log`
- `printd(DEBUG_LEVEL, fmt, ...)`: Conditional debug logging
- `printf(fmt, ...)`: Always prints to both serial and framebuffer

**Debug Macros:**
- Use `printd(DEBUG_SUBSYSTEM, ...)` for conditional logging
- Example: `printd(DEBUG_SCHEDULER, "Switching to task %lu\n", task->taskID);`

### Testing

**Test Framework (`kernel/include/test_framework.h`):**
- `test_framework_init()`: Initialize test infrastructure
- `test_run_preboot()`: Tests run before scheduler starts
- `test_run_postboot()`: Tests run after scheduler enabled
- Test files in `kernel/test/`

## Important Implementation Details

### Address Space Layout

- **Kernel code/data**: 0xffffffff80000000 - 0xffffffffffffffff
- **HHDM (physical memory)**: Offset provided by Limine (stored in `kHHDMOffset`)
- **Task heap**: 0x70000000 - 0x00007fffffffffff
- **Task environment**: Virtual mappings at `TASK_ENVP_VIRT`, `TASK_ENV_VIRT`
- **Paging structures**: Pool of identity-mapped pages for page tables

### Key Global Variables

- `kKernelPML4` / `kKernelPML4v`: Kernel page table (physical and virtual)
- `kHHDMOffset`: Higher-half direct map offset
- `kKernelTask`: Kernel task structure
- `kIdleTasks[]`: Idle tasks for each CPU core
- `kRootFilesystem`: Mounted root filesystem
- `kBlockDeviceDList`: Linked list of block devices

### Calling Conventions

- x86_64 System V ABI (Linux/Unix calling convention)
- Arguments: RDI, RSI, RDX, RCX, R8, R9, then stack
- Return value: RAX
- Assembly uses Intel syntax (`-masm=intel`)

### Common Pitfalls

1. **Virtual vs Physical Addresses**: Always convert with `PHYS_TO_VIRT()` / `VIRT_TO_PHYS()` macros
2. **Page Alignment**: Addresses for paging functions must be page-aligned (4KB)
3. **Interrupts**: Disable interrupts (`cli`) when modifying shared scheduler state
4. **CR3 Reload**: After modifying page tables, reload CR3 with `RELOAD_CR3` macro
5. **Task Creation**: `task_create()` needs valid parent task for environment inheritance

## File Organization

```
kernel/
├── src/
│   ├── main.c              # Boot entry point
│   ├── kernel.c            # Kernel initialization
│   ├── scheduler.c         # Task scheduler
│   ├── task.c              # Task management
│   ├── thread.c            # Thread management
│   ├── elf_loader.c        # ELF binary loader
│   ├── memory/
│   │   ├── paging.c        # Virtual memory
│   │   ├── allocator.c     # Physical page allocator
│   │   ├── kmalloc.c       # Kernel heap
│   │   └── vma.c           # Virtual memory areas
│   └── driver/
│       ├── block/          # Block devices & partitions
│       ├── filesystem/     # VFS, FAT, ext2
│       │   └── vfs/
│       └── system/         # Hardware drivers
│           ├── ahci.c      # SATA
│           ├── nvme.c      # NVMe
│           ├── pci.c       # PCI bus
│           └── acpi.c      # ACPI
├── include/                # All header files
├── test/                   # Kernel tests
│   └── elf/                # Test ELF programs
└── GNUmakefile
```

## QEMU Configuration

The default QEMU configuration (in root `GNUmakefile`):
- 8GB RAM (`-m 8g`)
- 2 CPU cores (`-smp 2`)
- Serial output to `qemu_com1.log`
- Telnet monitor on port 55555
- NVMe disk image at `disk/os64.img` (64MB FAT partition)

To change config, edit `QEMUFLAGS` in root `GNUmakefile`.

## Working with Code

### Adding a New System Call

1. Define syscall number in `kernel/include/syscall.h`
2. Add handler in `kernel/src/syscall.c`
3. Update syscall dispatch table
4. Implement in assembly wrapper if needed

### Adding a New Driver

1. Create source file in appropriate `kernel/src/driver/` subdirectory
2. Add header to `kernel/include/driver/`
3. Initialize in `kernel_init()` (kernel/src/kernel.c)
4. Hook into VFS or block device layer as appropriate

### Modifying the Scheduler

- Scheduler code is in `kernel/src/scheduler.c` and `kernel/src/scheduler.S`
- Core-local storage accessed via `get_core_local_storage()`
- Always disable interrupts when modifying thread queues
- Test thoroughly with SMP enabled (multiple cores)

## Coding Style and Best Practices

### Extern Declarations

**Always place `extern` declarations at the top of the file, never inside functions.**

```c
// GOOD - at top of file
extern uintptr_t kKernelPML4;
extern uintptr_t kHHDMOffset;

void some_function(void) {
    // use kKernelPML4
}

// BAD - inside function
void some_function(void) {
    extern uintptr_t kKernelPML4;  // Don't do this!
}
```

This makes external dependencies clear and easy to find at a glance.

### Context Switching Between Task and Kernel Space

**Problem**: When running in task context (task's CR3 loaded, task's stack), you cannot safely access kernel structures because they may not be mapped in the task's address space.

**Solution**: Switch both CR3 and RSP before accessing kernel data structures:

```c
void task_exit(void) {
    core_local_storage_t *cls = get_core_local_storage();

    // Save kernel stack pointer (from CLS while it's still valid)
    uintptr_t kernel_rsp = cls->kernel_interrupt_stack_top - 16;

    // Switch to kernel stack
    __asm__ volatile("mov rsp, %0" : : "r"(kernel_rsp));

    // Switch to kKernelPML4
    __asm__ volatile("mov cr3, %0" : : "r"((uint64_t)kKernelPML4) : "memory");

    // CRITICAL: Reload CLS pointer after stack/CR3 switch!
    cls = get_core_local_storage();

    // Now safe to access kernel structures
    thread_t *thread = cls->currentThread;
    // ...
}
```

**Key points**:
- Core-local storage (CLS) is accessible from any CR3 (it's in the shared upper-half)
- Save values from CLS (like kernel_rsp) BEFORE switching stack
- Switch stack BEFORE switching CR3 (order matters for some edge cases)
- **ALWAYS reload CLS pointer after switching RSP/CR3** - the old pointer is invalid
- The `"memory"` clobber on CR3 load prevents compiler reordering

**CRITICAL: Reload CLS Pointer After Stack Switch**

After switching RSP (and optionally CR3), the original `cls` pointer variable becomes invalid because it was likely stored on the old stack. You MUST reload it via `get_core_local_storage()`:

```c
void call_in_kernel_context(void (*func)(void*), void *arg)
{
    core_local_storage_t *cls = get_core_local_storage();

    // Save values to CLS before stack switch
    cls->saved_func = func;
    cls->saved_arg = arg;

    // Switch to kernel stack
    __asm__ volatile("mov rsp, %0" : : "r"(kernel_rsp));

    // Switch to kKernelPML4
    __asm__ volatile("mov cr3, %0" : : "r"(kKernelPML4) : "memory");

    // CRITICAL: Reload CLS after context switch!
    // The old 'cls' variable is on the old stack and now invalid.
    cls = get_core_local_storage();  // Reads from GS:0, always valid

    // Now safe to access cls
    cls->saved_func(cls->saved_arg);
}
```

**Why this is necessary**:
- Local variables (including `cls`) may be stored on the stack by the compiler
- After `mov rsp`, that stack is no longer accessible
- Accessing the old `cls` pointer reads garbage → crash (often GPF with corrupted RIP)
- `get_core_local_storage()` reads from `GS:0`, which is always valid regardless of RSP or CR3

**Symptoms of this bug**:
- General Protection Fault (#GP) with corrupted RIP like `0xf000ff53f000ff53`
- Happens after stack/CR3 switch when trying to dereference the stale `cls` pointer
- Only occurs in SMP systems under specific timing/load conditions

### Writing to Task Memory from Kernel via the HHDM (preferred)

**Problem**: When the kernel needs to write to task memory (e.g., writing a
return address to a task's stack), the task's pages are mapped at a task-local,
lower-half VA that is NOT present in kKernelPML4. You cannot write through the
task VA directly.

**Bad Solution**: Use `kmalloc()` for task stacks — this makes them permanently
accessible from kernel space, defeating memory isolation.

**Solution (since the lazy-HHDM change)**: Translate the task VA to its physical
page through the task's own page tables, then write through the HHDM alias. Any
allocator-owned page (which task stacks/heap are — they come from
`allocate_memory_aligned()`) is reachable at `phys | kHHDMOffset` in kKernelPML4
*while allocated*, so no temporary mapping is needed:

```c
// Allocate with allocate_memory_aligned() - keeps it isolated,
// but it IS HHDM-reachable from the kernel while allocated.
uintptr_t phys = allocate_memory_aligned(size);

// Map only into task's PML4 (task sees it at its lower-half VA)
paging_map_pages(task->pml4v, virt, phys, pages, flags);

// Later, when the kernel needs to write to it:
// 1. Resolve the task VA to physical via the task's OWN page tables
uintptr_t phys_addr = paging_walk_paging_table((pt_entry_t*)task->pml4v, task_virt_addr);

// 2. Write through the HHDM alias — no temp mapping, no unmap, no TLB dance
if (phys_addr && phys_addr != 0xbadbadba)
    *(uint64_t *)(phys_addr | kHHDMOffset) = value;
```

**Why not a scratch temporary mapping**: The old technique mapped the page at a
fixed scratch VA in kKernelPML4, wrote, then unmapped. Do NOT resurrect it, and
in particular never reuse `0xffffffff80000000` as the scratch VA — that is the
kernel link base, so mapping/unmapping there clobbers the kernel's own first
text page. The HHDM write above supersedes it entirely.

**Validity note**: `phys | kHHDMOffset` is valid only while the page is
allocated. That is always true right after you allocate/map it (e.g. at
thread-creation time). Touching it after free is the intentional use-after-free
tripwire — see the HHDM section below.

### HHDM (Higher-Half Direct Mapping) — Lazy Maintenance

**The rule (since July 2026): physical memory is HHDM-mapped in the kernel
page tables exactly while the allocator considers it allocated.**

- ANY allocator-owned memory — `kmalloc()`, `kmalloc_aligned()`, AND
  `allocate_memory_aligned()` — **IS** accessible via `phys | kHHDMOffset`
  while allocated, on every memory map (no more layout luck).
- Freed or never-allocated RAM is **deliberately unmapped**: touching it via
  the HHDM page-faults with a "use-after-free or wild pointer?" panic. This
  is a designed tripwire (DEBUG_PAGEALLOC-style), chosen over an eager
  Linux-style full direct map.
- Mechanics: the allocator's single alloc/free choke points call
  `paging_hhdm_map_range()` / `paging_hhdm_unmap_range()` (see paging.h for
  the boundary-page rules); frees broadcast a TLB-shootdown IPI. Early-boot
  allocations are retro-mapped when `init_os64_paging_tables()` builds the
  real tables. MMIO/reserved regions still require explicit mappings.

**When to use each**:
- `kmalloc()`: For kernel data structures that need to be permanently accessible
- `allocate_memory_aligned()`: For task-specific memory (stacks, heap pages) —
  note it IS kernel-visible via HHDM while allocated, so the temporary-mapping
  technique above is no longer strictly required for reading/writing task
  pages from the kernel (it remains valid, just redundant)

**Converting addresses**:
- Physical to HHDM: `phys | kHHDMOffset` (valid iff the memory is currently allocated)
- HHDM to Physical: `hhdm - kHHDMOffset`

**Locking**: all allocator entry points take an interrupts-disabled spinlock
(`kMemoryStatusLock`) — allocations happen concurrently from page-fault
handlers on multiple cores. Never call allocate/free while holding another
spinlock that a fault path might also take.

### Static Variables and SMP Safety

**Warning**: Using `static` variables in functions can create race conditions in SMP (multi-core) systems.

```c
// NOT SMP-safe - race condition if multiple cores call this simultaneously
void some_function(void) {
    static kernel_read_params_t params;  // Shared across all cores!
    params.file = file;
    // If another core calls this function, 'params' gets clobbered
}
```

**Solution**: Use per-CPU storage via core-local storage:

```c
// Add field to core_local_storage_t with meaningful prefix
// Example: cikc_ = call_in_kernel_context (the function that owns these variables)
typedef struct {
    // ... existing fields ...

    // cikc = call_in_kernel_context (vma.c context switching scratch space)
    void (*cikc_saved_func)(void*);
    void *cikc_saved_arg;
    uint64_t cikc_saved_cr3;
    uint64_t cikc_saved_rsp;
} core_local_storage_t;

// Access via CLS
void call_in_kernel_context(void (*func)(void*), void *arg) {
    core_local_storage_t *cls = get_core_local_storage();
    cls->cikc_saved_func = func;
    cls->cikc_saved_arg = arg;
    // Each core has its own isolated copy
}
```

**Naming Convention for CLS Scratch Variables**:
- Use a meaningful abbreviation as prefix (e.g., `cikc_` for `call_in_kernel_context`)
- Add a comment above the variables explaining what the abbreviation stands for
- This prevents accidental reuse in unrelated functions
- Makes it clear which function owns these scratch variables

### Writing to Task Memory from Kernel

When creating threads, the return address needs to be written to the task's stack. The stack may only be mapped in the task's PML4, not kKernelPML4.

**Special case**: ktask/idle tasks use kKernelPML4 directly, so their stacks are accessible:

```c
task_t *task = (task_t*)ownerTask;

if (task->pml4v == (uint64_t*)kKernelPML4v) {
    // ktask/idle - stack is directly accessible
    *(uintptr_t *)newThread->regs.RSP = (uintptr_t)&task_exit;
} else {
    // Other tasks - use temporary mapping technique (see above)
}
```
