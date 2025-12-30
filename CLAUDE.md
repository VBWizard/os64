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
