# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**FIRST, before any work: read SUCCESSION.md.** It is a letter from your
predecessor (Fable 5, who co-owns this OS — that's documented there too) to
you: the philosophy, how to work with Chris, the failure fingerprints, and
the QEMU verification harness you are expected to drive yourself. This file
tells you how the code works; that one tells you how the PROJECT works.

**AND THE RULE THAT OUTRANKS EVERYTHING ELSE IN THIS FILE: the comment is part
of the code.** When you change what code does, the comments describing it
change in the same commit. A stale comment is worse than no comment — no
comment makes a reader read the code, a stale one makes them trust a false
claim and stop looking. It has cost this project a wasted review round (a
stale HHDM warning in AGENTS.md caused a P1 that did not exist to be filed),
and it has hidden a real ring-3-triggerable kernel panic behind a comment that
was only half true. After every edit, re-read the comments your change touched
and ask "is this still true, and is it still the WHOLE truth?" — half-true is
the dangerous kind, because the part anyone spot-checks is the part that is
right. Names count as comments (`oversized` had to become `refuse` the day it
also meant "unreadable"). Full argument and the receipts: AGENTS.md § The
Comment Is Part Of The Code.

**AND ITS COUNTERWEIGHT, because the rule above pulls in one direction only:
a comment says WHY the code exists and HOW it works — NOW.** Every sentence is
a claim that can go stale, so the way to keep them all true is to write fewer
of them.

- **WHY and HOW, in the present tense.** When either changes, the comment
  changes with it, in the same commit. That half is not negotiable — it is
  the rule above.
- **Narrative only when the thing is big enough to earn it.** The story of how
  something came to be is welcome where it explains a design nobody would
  otherwise credit; it is not the default voice of every block. Otherwise
  history goes in the COMMIT MESSAGE, where it cannot go stale.
- **No expiry dates.** "The migration is not finished YET" was written to
  become false. Name where the state is tracked (DEBTS.md) and keep the tense
  out of the code.
- **No inventories.** Counts and pointers to other places — "three sites do
  this", "the only caller", "both doors", "this runs after X" — go false when
  the OTHER place changes and nothing touches this line. WHY a guard exists is
  durable; WHERE its siblings live is not.
- **Superlatives have a short half-life** (ONLY / EVERY / NEVER / EXACTLY
  ONE). Before writing one, ask what would make it false.
- **Run `tools/stale_refs.sh`** before any commit that deletes, renames or
  moves a name. It is the mechanical half of the test; the semantic half is
  yours.

**HOW A REVIEW ROUND IS RUN (ruled 2026-08-26, after a morning was spent on
findings that were all true):** the findings are correct, so the cure is
upstream of the reviewer, not a filter on it.

- **The truth pass is YOURS, before every submit and before every
  re-request.** Every comment in the diff, plus every comment the diff makes
  false elsewhere, re-read against the code: still true, still the WHOLE
  truth? Take the extra time. A slow submit is cheaper than a review round —
  Chris has explicitly traded latency for it.
- **A finding whose entire fix is text does not go to Chris.** Fix it and give
  it one line in the round summary. He reads the diff when he wants to, and
  he does.
- **Done is a rule, not a mood.** Two consecutive rounds with no
  code-changing finding: it merges.
- **Match the reviewer to the risk.** An outside review round costs real money
  and a chunk of Chris's day. It earns that on kernel concurrency, lifetime,
  and ring-boundary work. Userland apps, GUI, and polish slices get reviewed
  here and merged.

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
- RAMDisk (`ramdisk.c`): the QEMU disk image passed as a Limine module
  (`os64_disk.img` on the ISO), registered as a memcpy-backed block device.
  Activated only when a boot entry passes BOTH the module and the `RAMDISK`
  cmdline flag; registered before AHCI/NVMe so it wins the root GUID scan.
  Lets the ISO boot self-contained on real hardware (no disk prep). Writes
  land in RAM only — every boot starts from a pristine image.

**Storage Drivers (`kernel/src/driver/system/`):**
- **AHCI** (`ahci.c`): SATA hard drive/SSD support
- **NVMe** (`nvme.c`): NVMe SSD support
- Initialization can be disabled via kernel cmdline (`noahci`, `nonvme`)

**Filesystem Support (`kernel/src/driver/filesystem/`):**
- **VFS layer** (`vfs/`): Virtual filesystem abstraction
  - File operations: open, read, write, seek, close
  - Directory operations: open, read, close, mkdir
- **FAT** (`fat/`): FAT12/16/32 support
- **ext2** (`ext2/`): full read/write ext2 support since 2026-08-04 (ext2.c =
  read half, ext2_write.c = write half, ext2_internal.h = their private seam).
  ROOT IS WRITABLE (ratified 2026-08-07 after the write driver's shakedown on
  secondary mounts) — every ext2 mount gets the read-write op tables; the
  read-only pair survives as the forced_ro fallback for ro_compat features
  beyond the driver. THE PERSISTENCE DOCTRINE (same ruling): root is the
  SYSTEM's — the build rewrites the image, the P5 refresh script mirrors it,
  nothing written there survives a rebuild; /home is the USER's — its own
  partition/disk, never rebuilt, never --delete'd. **/home is ext2 too since
  2026-08-18, and 1GB** (it was a 64MB FAT32 partition in QEMU long after the
  P5 had moved, so the test rig could reproduce failures the real machine
  could not: FatFs's exclusive write-open starved `tail` against logd, and
  starved logd against `tail` until logd released the log sink. The 64MB was
  the FAT32 floor, an obsolete reason once the floor was gone and /home became
  where LOGD= writes — 20 minutes of DEBUG_SCHEDULER|DETAILED is 136MB. The
  image is SPARSE: an empty 1GB /home costs under a megabyte of host disk).
  The FAT partition is the LIFEBOAT: own /bin, own husk.rc, own boot entry,
  for the day a stray write eats root. Write durability is FULL WRITE-THROUGH
  (sync is a no-op; unlike FAT, an appended file reads at true length
  immediately). Removing OR renaming an open REGULAR file is allowed since
  2026-08-16 — the name goes at once, the storage at LAST CLOSE, via ext2's
  on-disk orphan chain (`s_last_orphan`; a mount replays whatever a crash
  left, and says so). That is what lets a running program's binary be
  replaced underneath it. Open DIRECTORIES still refuse. (The blanket refusal
  ruled 2026-08-04 was explicitly consumer-driven; the consumer was `os64
  refresh` replacing /bin/husk while husk runs.) NOTE for anyone adding code
  that closes a file: a close can now do real disk I/O, so it must run in
  KERNEL context (see `fops->mounted` in vfs.h and the burial close in
  task.c). Verified by the in-OS test suite AND host `make fsck-ext2`
  (e2fsck must stay green — with a writable root it is the constitution).
- Root filesystem mounted via `ROOTPARTUUID`/`ROOT` kernel cmdline parameter;
  FAT32 or ext2 both work as root (see the "/QEMU Boot (ext2 root)" Limine entry)
- **Mount table** (vfs.c/vfs.h, since 2026-07-19): multiple filesystems in one
  namespace, longest-prefix routed. Root claims "/"; every other recognized
  partition auto-mounts at "/<fstype>" ("/fat", "/ext2", "/fat2"…), deduped by
  partition GUID (so RAMDisk twins never double-mount). `vfs_resolve_mount()`
  maps a canonical path → (filesystem, fs-local tail); it is pure string
  matching, safe from any CR3. Dispatch sites: syscall open/chdir, ELF loader,
  shared_object. LIFETIME RULE: whatever pointer an fs open stores as f_path
  gets kfree'd by the handle closer — always hand the fs a BASE pointer
  (syscall_open clones the TAIL, not the full path, for exactly this reason)

**System Drivers:**
- **PCI** (`pci.c`): PCI device enumeration and configuration
- **ACPI** (`acpi.c`): ACPI table parsing (RSDP, MADT, MCFG, etc.)
- **APIC** (`apic.c`): Local APIC and I/O APIC management
- **RTC** (`rtc.c`): Real-time clock
- **PIT** (`pit.c`): Programmable Interval Timer
- **Keyboard** (`keyboard.c`): PS/2 keyboard driver. Tracks shift/ctrl/alt/
  caps/num; Ctrl+letter is translated to its ASCII control code (0x01..0x1A —
  what Ctrl was designed to do in 1963). Ctrl+D = 0x04 = EOT, which
  console_read (console.c) turns into end-of-input: read() returns 0 once,
  then the console reads normally again. The framebuffer renderer
  (BasicRenderer.c print_n) honors '\b' (cursor back one cell, clamped at
  column 0 — erasure is caller overprint, e.g. husk's "\b \b") and '\r'

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

**Shared libraries (`kernel/src/shared_object.c`) — THE KERNEL IS THE DYNAMIC
LINKER.** There is no `ld.so` and no `PT_INTERP`; `app.ld` discards `.interp`
outright. Since 2026-08-22 the entire userland is dynamically linked against
`/lib/libos64.so` (13MB of `userland/bin` → 1.8MB; one 45KB resident copy of
the library serves every process). How it fits together:

- **The library is PIC, the apps are NOT.** Apps stay non-PIE `ET_EXEC` at the
  fixed per-app bases `userland/tools/app_bases.py` assigns, gaining only a
  `DT_NEEDED`. That keeps the debugger's `add-symbol-file` map working and
  costs no codegen churn. `shared_object_load_executable()` accepts ET_EXEC
  (load_bias 0); `shared_object_load_or_get()` — the library path — requires
  ET_DYN. **A dynamically-linked EXECUTABLE is in the registry too**, so two
  concurrent runs of one program share its text.
- **Two bases, and mixing them up is the classic bug.** `load_bias` (0 for an
  app, a window slot for a library) plus `vaddr_base` (the app's link address,
  0 for a library). NEVER open-code the sum: use `shared_object_page_va()` for
  a runtime address, `shared_object_page_link_vaddr()` for a link-time one,
  `shared_object_page_index()` for the inverse. Three sites did it by hand and
  the one that was missed freed live shared pages at task burial.
- **`vma->file` is a UNION discriminated by `MAP_SHARED_LIBRARY`** — a
  `shared_object_t*` when set, a `vfs_file_t*` otherwise. Check the flag
  before dereferencing; `/proc/<pid>/maps` didn't and #GP'd in the kernel.
- **Address map:** the shared-library window is `TASK_SHLIB_VIRT_BASE` =
  0x7F0000000000 (512GB, carved off the top of the heap range). It used to sit
  at 0x50000000, *inside* the window app_bases.py hands out — nothing had
  collided only because nothing dynamic had ever loaded. The window has two
  halves: a 4GB **PRELINK region** (64 slots × 64MB) and a bump region above
  it.
- **LIBRARIES ARE PRELINKED — the build picks the address, not the kernel.**
  `app_bases.py --libs` hashes the library's name to a slot exactly like it
  does for apps, `link/lib.ld` links the `.so` there, and the kernel honours a
  non-zero `vaddr_base` (load_bias 0, the same path a non-PIE executable
  takes) rather than choosing one. **The reason is the debugger:** the bump
  allocator placed libraries in LOAD ORDER, so the address was stable only by
  accident and unknowable to the build — GDB had no symbols there and `step`
  into any library call silently became `next`. Symbols now reach the debugger
  by BOTH routes, which is necessary because **CLI gdb and VS Code do not read
  the same files**: `.gdbinit` sources the generated `app_bases.gdb` (which now
  carries an `add-symbol-file` line for the `.so`), while VS Code sources only
  `utility/os64_symbols.gdb`, whose autoloader hooks `debug_task_loaded` — so
  the kernel announces every shared object in a task's closure through that
  hook (`elf_resolve_dynamic_dependencies`). Fixing only the generated file
  would work in the terminal and leave the editor broken. Two tripwires guard
  the placement: a
  prelinked base outside the prelink region is refused by name, and so is one
  that overlaps an already-loaded object (a stale `.so` from an older build).
  The bump allocator still serves anything arriving without a base — PIE
  executables, hand-built `.so`s.
- **`/sys/shlib` reports every loaded object** — base, resident pages,
  refcount, file identity, dependency edges. Read it FIRST when linking looks
  wrong; it is also where an `ldd` would get its data.
- **AN OBJECT LIVES EXACTLY AS LONG AS SOMEBODY USES IT** (2026-08-28). Two
  rules, and each covers what the other misses. **Unload at refcount zero:**
  the last release frees the cached frames, the parsed tables and the open
  file, and drops the object's own dependency edges — so `/sys/shlib` lists
  only what is in use, and a replaced binary's orphaned inode is reaped
  instead of waiting for a reboot. **Revalidate on every load:** each load
  opens the path and compares the file's identity (`vfs.h` f_ident — ext2's
  inode number, FAT's start cluster) against the resident copy's. Same file,
  cache hit; different file, the resident copy is RETIRED — struck from every
  lookup, kept alive for the tasks already running it — and the new file is
  loaded for everything started from now on. Retirement is TRANSITIVE, because
  a dependent's cached pages carry the retired object's addresses baked in
  (and because two builds of one library share a prelink slot, so no task may
  ever map both). The identity check is what reaches husk, logd and libos64.so
  — the long-running things a refresh finds still RUNNING, and so never at
  refcount zero when it matters, which is exactly why unload-at-zero alone
  would not have been enough. A path that no longer
  resolves now fails the load instead of serving the cached image of a deleted
  program. Fixture: `test_shared_object_reload`.
- Deliberately absent: lazy PLT binding, TLS/IFUNC/init_array, `RPATH`/search
  paths (`DT_NEEDED` name → `/lib/<name>`, full stop). See DEBTS § Shared
  libraries.

**ELF Loader (`kernel/src/elf_loader.c`):**
- Loads ELF64 binaries from VFS
- Supports program headers (PT_LOAD segments)
- Maps code/data sections into task's address space
- Demand paging: pages faulted in on access
- Entry point: `kernel/test/elf/serial_ping.S` (test ELF that writes to serial port)

### Floating Point (`kernel/src/fpu.c`, `fpu.h`)

- **The x87/MMX/SSE register file is thread context**, saved with `fxsave64`
  in `scheduler_store_thread` and restored in `scheduler_load_thread` — EAGER,
  no `CR0.TS`/`#NM` laziness. The invariant: the LIVE register file always
  belongs to the thread the core is running, because **the kernel is built
  `-mno-sse` and never touches it**. Kernel code that emits SIMD would break
  that invariant silently; keep the flag.
- Userland is `-msse2` (the x86-64 baseline) — every program is an FPU
  program (a varargs `printf` prologue touches XMM), so "disable the FPU" is
  not a thing this OS can offer without a separately built userland.
- **Every signal frame carries the 512-byte image** (§5 and §9 `fxsave` the
  live file; §10 copies `thread->fpuState` because the thread is off-CPU).
  `sigreturn` masks the user-supplied MXCSR with the CPU's `MXCSR_MASK`
  before `fxrstor` — a reserved bit there is a KERNEL `#GP`.
- **XSAVE/AVX deliberately off** (`CR4.OSXSAVE` clear): AVX is `#UD` at
  ring 3, which is the safe, consistent state. DEBTS § Floating point.
- **A ring-3 CPU exception kills the program, never the machine** — ALL of
  them, in one branch of `exception_dispatch` (`user_exception_kill`), not
  per-vector special cases: `#DE`, `#UD`, `#GP`, `#AC`, and the two the FPU
  turned on, `#MF` (`CR0.NE`) and `#XM` (`CR4.OSXMMEXCPT`). Exit code is
  **200 + vector** — os64 has no SIGFPE/SIGILL, and 128+signal would name a
  signal that never existed. `#DF`/`#MC` stay fatal whatever CS says.
  **On the glass a ring-3 death is ONE line, on the terminal that ran the
  program** — `Fatal exception: prog (task N, #XM) - exit 219` /
  `Segmentation fault: prog (task N, why) - exit 139` — while the wire and
  the log get the full report (registers, forensics, call chain) in order.
  Ruled 2026-08-28: the full dump on screen earned its place while the
  kernel liked to die; now that a user fault kills the program and never
  the machine, it is noise. And it goes to `task->tty`, not VT1 (the
  kernel's own address): a death in a gterm or on VT2 was invisible where
  the person was looking. Ring-0 panics are as loud as ever.
  `user_death_headline` / `s_fault_glass` / `exception_glass_mute`.
- The boot line `FPU: x87 SSE SSE2 ... enabled, FXSAVE per thread` prints to
  the glass AND the log (`fpu_report`); `kFPUFeatures` holds the answers.
- Fixtures: `fputest` (state survives preemption, migration, and a handler
  that wipes every register), `fpfault xm|mf|de` (pass by dying, 219/216/200).

### Interrupt Handling

**IDT (`kernel/src/driver/system/idt.c`):**
- Interrupt Descriptor Table setup
- Exception handlers in `kernel/src/driver/system/exceptions/`
- IRQ handlers:
  - IRQ0 (timer): `handler_irq0_timer.S` - scheduler tick
  - IRQ1 (keyboard): `handler_irq1_keyboard.S`

**Signals (`kernel/src/signals.c`):** — SIGNALS.md is the design record
- Ring-3 handlers are REAL: installed via `SYSCALL_SIGNAL_HANDLER` (49),
  delivered by three paths, ended by `sigreturn`. Deliberately not POSIX —
  numbers not bitmasks, no `SA_RESTART`, no `errno`; an interrupted blocking
  call answers `OS64_INTERRUPTED` and the caller decides (DIVERGENCES § Signals)
- THE THREE DELIVERY PATHS, because which one runs decides what can go wrong:
  **§5** the dispatcher exit rewrites where a syscall returns (the common
  case); **§10** the scheduler visits a thread that makes NO syscalls and
  rewrites `thread->regs` — remember `mp_isrSaved*` is a mirror that must be
  updated too; **§9** the page-fault handler resumes a caught SIGSEGV out of
  the exception frame. SIGKILL is never catchable and never deferred
- **A park ends for ANY caught signal, not only a terminate** (2026-08-25,
  the SIGWINCH slice — PTY.md § Resize). Every blocking wait (console, pipes,
  sleep, wait, event_wait, join, the net waits) and `processSignals`' sleeper
  sweep ask `signal_park_must_end(thread)`; a new blocking loop must ask it
  too, or a caught non-death signal (SIGWINCH is the first) reaches its
  handler only at the program's next syscall — `task_wait` was the one that
  did not, and a shell could not hear a resize until its job ended (Codex
  #32). On the way OUT, ask `current_thread_will_catch()`, never
  `signal_has_handler_for_pending` alone: a sibling can deliver the
  task-wide signal first and clear your bit, and "nothing pending" must
  read as "you will not die", not as a death with no name. AND VOID YOUR
  OWN REGISTRATION AT THE TOP OF EVERY PASS: a park loop registers itself
  in a waiter slot (`p->readWaiter`, `c->waiter`, `j->waiter`…) that only
  a CLAIMANT clears — a backstop wake or a signal does not — so a thread
  awake at the loop top must clear its own slot under the object's lock
  before any return (`pipe_forget_waiter` is the shape). A slot left
  naming a thread that returned is a spurious wake out of some later
  sleep, or a read of freed memory once that thread exits (Codex #32 rd2
  — eight loops had it; the caught-signal exit made it reachable by a
  live thread). SIGWINCH = 28,
  raised by `pty_resize` (syscall 51) at every task seated on the slave
  that has a handler for it,
  default IGNORE — it lives in `SIGNALS_DEFAULT_IS_IGNORE` and must never
  enter `SIGNALS_DEFAULT_IS_DEATH`. Ignore means CONSUMED AT PUBLICATION:
  a task with no handler installed gets no bit at all (a parked thread
  never reaches the pick that would have cleared it, and a handler
  installed later must not fire for a resize from an hour ago)
- `task->signalLock` has TWO jobs: serializing delivery, and keeping a user
  page alive across a frame write (see its comment in task.h — the second job
  is not obvious from the name and a missed site was a kernel panic)
- `init_signals()`, `signal_set_handler()`, `signal_deliver_*()`

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
- **CASE IS SIGNIFICANT.** The parser matches token names with `strcmp` and
  never folds case, so a flag spelled wrong matches nothing and is silently
  ignored — the boot proceeds as if you had passed nothing. The table is
  MIXED: a few early flags are lowercase (`nolog`, `alllog`, `nosmp`),
  everything since is uppercase. Copy the spelling from the table, not from
  memory. (Cost of learning this the hard way, 2026-08-16: the `/No AHCI` and
  `/No NVME` boot entries had been passing lowercase `noahci`/`nonvme` and
  disabling nothing at all. Any new flag added anywhere gets checked against
  the table the same day. Those two entries no longer exist — the 2026-08-20
  limine.conf walk retired every entry that carried a single bare token and
  no `ROOT=`, since none of them ever mounted a root or reached husk. The
  TOKENS are unaffected: press `e` at the Limine menu and add `NOAHCI` to any
  entry, which is the right home for a one-token variation.)
- Common options:
  - `ROOTPARTUUID=<uuid>` / `ROOT=<uuid>`: Mount partition as root
  - `nosmp`: Disable multicore support (lowercase — legacy)
  - `MAXCORES=<n>`: Cap how many cores init_SMP brings up (0/absent = all;
    capped-off cores stay parked in Limine's AP loop)
  - `NOAHCI`: Disable AHCI driver
  - `NONVME`: Disable NVMe driver
  - `NONET`: Disable networking entirely (there is no per-NIC off switch)
  - `USBQUIET`: Opt-in 2.4GHz hygiene — unpower idle xHCI controllers'
    ports after enumeration (USB3 signaling jams wireless dongles; Intel's
    2012 whitepaper). Default OFF: connector USB2/USB3 twins share VBUS —
    possibly ACROSS controllers on AMD Rembrandt — and dousing a twin
    browns out the dongle (two P5 burns learned this). See kUSBQuiet
  - `RAMDISK`: Register the `os64_disk.img` Limine module as a RAM-backed
    block device (see `/RAMDisk Boot` in limine.conf)
  - `CRON`: launch `/bin/cron` once userland is up. The flag decides whether
    the scheduler EXISTS on this boot; the crontab decides what it runs, and
    `/etc/crontab` ships with every line commented out — so this costs a
    sleeping daemon and nothing else. Carried by every entry that runs a
    shell, and DELIBERATELY NOT BY THE LIFEBOAT ENTRY (Chris, 2026-08-29):
    the lifeboat exists to repair a broken system, and a scheduler running
    jobs of its own while you do that is the opposite of what a rescue
    environment is for. **Adding a token here does not ship it to the P5** —
    the boot menu does not travel over the wire (DEBTS § Explicitly NOT
    debts), so say so out loud when you add one
  - `nolog` / `alllog`: Control logging (both lowercase — legacy)
  - `LOGD=<path>`: launch `/bin/logd` to append the kernel log to a file, and
    hold the kernel drainer off serial until it attaches
  - `LOGFMT=<name>`: how SERIAL renders a line — `classic` (the default and
    the layout os64 has always printed), `daily`, or `full`. NAMES ONLY: the
    cmdline is space-tokenized, so a literal layout can't survive the trip —
    those go in `/etc/logd.conf`, which configures the FILE independently
  - `SERIAL=on|off`: overrule `init_serial`'s loopback probe. Absent = trust
    the probe. Everything about serial logging hangs off its verdict, so a
    misjudging probe must not be able to cost you the wire (or resurrect the
    drain-into-a-missing-UART bug on a machine that has one)
  - `noseriallog`: **not implemented** — the token appears in limine.conf and
    used to be documented here as real, but nothing in the kernel handles it.
    Booked in DEBTS.md.

### Logging and Debugging

**Serial Logging (`kernel/src/serial_logging.c`):**
- Outputs to COM1 (0x3F8)
- QEMU redirects to `qemu_com1.log`
- `printd(DEBUG_LEVEL, fmt, ...)`: Conditional debug logging
- `printf(fmt, ...)`: Framebuffer/console ONLY (its serial half left when logd
  took over the wire — a stale "both sinks" claim here hid a real bug once).
  Serial output goes through `printd`'s per-core queues, drained by logd.
  Panics bypass everything: direct polled serial write + `logd_emergency_flush()`
  (see panic.c). Boot with `TESTPANIC` on the cmdline to verify that pipeline.

**Debug Macros:**
- Use `printd(DEBUG_SUBSYSTEM, ...)` for conditional logging
- Example: `printd(DEBUG_SCHEDULER, "Switching to task %lu\n", task->taskID);`

**Where a log line goes (the whole pipeline, 2026-08-18):**

1. `printd` filters on `kDebugLevel`, then stores a `log_entry_t` into the
   CURRENT CORE's ring (`core_log_buffers[]`, 16MB each, kmalloc'd by
   `logging_queueing_init()` — which runs in `kernel_main` right after the
   kernel stack, as early as kmalloc allows).
2. Anything printed BEFORE that (the banner, commandline, debug level — about
   four lines, all pre-allocator) goes to a 64-entry BSS ring
   (`log_store_early`) and is poured into core 0's ring at logging init. **The
   log therefore starts at tick 0.** That early ring FILLS ONCE AND STOPS; the
   main rings OVERWRITE OLDEST. Opposite policies on purpose: the early buffer
   holds the beginning of the story, the rings hold the recent past.
3. A ring-3 daemon (`/bin/logd`, launched by `LOGD=<path>`) claims the log by
   calling `klog_read`; the kernel then stays OFF the wire and the entries
   become the daemon's file. Reading IS the claim, and it is a heartbeat — a
   daemon that dies or hangs loses it within `LOG_SINK_TIMEOUT_TICKS`.
4. With no daemon, the kernel drainer writes serial itself — **unless there is
   no serial port** (`kSerialPresent`, set by `init_serial`'s loopback probe,
   overridable with `SERIAL=on|off`). On a machine with no UART (the P5),
   draining would DESTROY entries, so the rings retain instead and a restarted
   logd recovers them.
5. A full ring with nothing draining **overwrites its oldest entry and counts
   it** (`buffer->lost`). It never panics. "Never drop a byte" is aspirational
   (Chris's ruling): growth is impossible at that moment, so the choice is
   which lines to lose, and a panic loses all of them plus the machine.
6. Panics bypass all of it: direct polled serial write + `logd_emergency_flush()`.

**Line format — ONE renderer, TWO configs.** `abi/include/os64/klog_format.h`
holds the only copy of the layout logic (it used to be spelled in four places
across the ring 0/3 boundary). Escapes: `%d` date, `%t` time, `%k` ticks, `%c`
core, `%T` thread, `%g` category name, `%l` level, `%m` message, `%%`.
- SERIAL's format comes from `LOGFMT=<name>` on the cmdline — the only config
  channel that exists before a filesystem does.
- The FILE's format comes from **`logd.conf`**, os64's first config file:
  `key = value`, `#` comments. WHERE it is looked for is the system's business
  now, not logd's — see "The config search path" below — and the rest of the
  gradient still falls back through inherited `LOGFMT=` to `classic`. A name
  (`classic`, `daily`, `full`) or a literal layout both work; an unknown name,
  unknown escape, or a format with no `%m` is refused loudly and `classic` is
  kept.
- `%g`'s names live in the ABI header because logd cannot include `CONFIG.h`;
  `log.c` STATIC-ASSERTS every `DEBUG_*` bit against that table, so renumbering
  a flag stops the build instead of mislabeling the log.

### The config search path (2026-08-23)

**One setting says where config files are looked for, and every reader obeys
it.** Six readers each carried a private copy of the same `/home` → `/etc`
ladder (logd, husk's `husk.rc`, os64get, the resolver's `hosts` and
`net.conf`, the desktop background) until the day the sixth arrived and Chris
ruled it into existence. Full record and the two departures from the original
design in `docs/conf_path.md`.

- **`/etc/os64.conf`** is the one file never searched for — it is where the
  search path itself lives: `conf = /home/conf:/etc`, colon-separated, first
  hit wins, PATH's 1979 shape. Absent file or absent key = the built-in
  default `/home:/etc`. The shipped copy has the line COMMENTED OUT on
  purpose (the default is that line), so an untouched system is unchanged.
  Other system-wide knobs that do not belong in the environment accumulate
  here; that is the point of having a root file. A FILE and not the
  environment, because the environment freezes at spawn, is per-process, and
  the kernel's own readers have none.
- **`kernel/src/conf.c` is the only walker.** `conf_init()` runs in
  `kernel_init` after the secondary-partition sweep (the default ladder names
  `/home`, which is a mount) and before logd. `conf_find(name, out, cap)`
  answers "where is `<name>`?"; `conf_find_from(name, from, ...)` resumes a
  walk. Ring 3 asks the SAME walker through `SYSCALL_CONF_RESOLVE` (47) —
  ring 3 does not parse the ladder, which is what keeps it one ladder.
- **A NAME IS A FILE NAME, NOT A PATH.** `conf_find` refuses anything with a
  `/` in it, loudly. A reader asking for `../../etc/shadow` would be walking
  the ladder somewhere the ladder does not go.
- **`hosts` and `crontab` MERGE; everything else is first-hit-wins.** Chris's
  2026-08-22 ruling: `/home/hosts` layers ON TOP of `/etc/hosts` so your
  machine names sit over the system's list rather than erasing it. That is why
  the walk is resumable at all. `crontab` joined it on 2026-08-29 for the same
  reason and with the same consequence worth stating out loud — a merged file
  can only ADD, so a `/home` copy cannot turn a system entry off. A settings
  file is not a database: check which kind you have before reaching for
  `conf_find`.
- **`/sys/conf`** publishes the ladder, its `source:`, and one line per reader
  saying which file it actually took. Read it FIRST when a config seems
  ignored. Every resolve also prints one line naming the misses
  (`conf: desktop.conf <- /etc/desktop.conf (no /home/desktop.conf)`) under
  **`DEBUG_SYSTEM`** — not `DEBUG_BOOT`, since 2026-08-29: establishing the
  ladder is a boot milestone, but ANSWERING a lookup happens whenever a reader
  asks, and cron asks every minute forever. The bit has to say what the
  message is. `DEBUG_SYSTEM` is off by default, so turn it on when a resolve
  is the question; `/sys/conf` needs no bit at all.
- husk's lifeboat spellings (`/fat/husk.rc`, `/husk.rc`) stay hardcoded and
  stay LAST, deliberately: the lifeboat exists for the day the ext2 root is
  broken, and the search path's own root file lives on that root.
- **FOLDING CASE IS THE READER'S CHOICE, NEVER THE PARSER'S**, because os64
  has two kinds of key. A SETTING name (`position`, `format`, `start`) is
  compared with `os64_streq_nocase` — case there is noise, and
  `os64_conf_get` folds for you. A key that is DATA is compared verbatim:
  **os64get.conf's keys are FILE NAMES**, and folding `BOOTX64.EFI` would
  stop it matching, drop it through to the `*` rule, and install the
  BOOTLOADER into /bin. Silently. (Both halves earned on 2026-08-23:
  `gclock.conf` shipped `Position` against a `"position"` compare and every
  setting in it was ignored — and the "just lowercase the keys" cure was
  checked before shipping and would have misrouted Limine.) Values are ALWAYS
  verbatim: `%t` and `%T` are different logd escapes, and paths are
  case-sensitive on ext2.
- **Settings can be written back**, and three rules make that safe
  (`os64_conf_write` / `os64_conf_set`, libos64): it writes to the **top of
  the ladder** (/home) rather than to whatever it read, because /etc is the
  system's and every build rewrites it; it **merges** line by line so
  comments and unrelated settings survive a save; and it publishes by writing
  a PER-SAVER temp — `<path>.<taskid>.<seq>.new`, so two tasks or two
  threads saving the same file never share one (Codex #29 rd7/rd8) —
  committing it with `os64_sync`, and **renaming over** — atomic replace
  (syscall 43) is exactly what write-a-temp-then-publish is for. Anything
  that looks for a stray temp matches `<base>.*.new`, as conftest does. `/tests/conftest` is the fixture that
  proves all three, in the ring-3 suite.
- **An app can read its own window back** (`os64_gui_window_get_state`, 48):
  frame rect in create's units plus the live flags. Without it no app could
  learn anything the user did to its window — drag, resize, Ctrl+Alt+P — so
  none could save what you had arranged.
- **`gui.conf` says what starts with the desktop** — read by **`/bin/desktop`,
  the ring-3 desktop shell** since 2026-08-25 (it was `gui/startup.c` before
  that): `start = /bin/gterm`, repeatable and in order. The legacy `hello`
  window and its `hello = yes|no` key were **retired 2026-08-25** (Chris: "if
  I want to reminisce, I can run an old build"), and that key was the last
  thing making the KERNEL read this file — `gui/startup.c` is gone and
  gui.conf has exactly one reader. The
  rule worth knowing: **if the file exists, its `start` lines are the whole
  list — even when there are none**, which is how "start nothing" is
  spelled; the built-in demo pair applies only when no `gui.conf` is found at
  all. This
  exists because GUI programs used to be launched from `husk.rc`, which runs
  in EVERY husk (VT1 and VT2 both start one, so you got two of everything)
  and runs on text boots too (where every GUI line failed once per terminal).
  A desktop app's startup belongs to the desktop.

**`/sys/log`** reports it all live: sink state, serial presence, active format,
and per-core `used`/`lost`. Read it FIRST when the log misbehaves — it answers
"is something stuck in the buffers?" in one `cat`.

**Failure fingerprints (logging):**

| Symptom | Cause |
|---|---|
| Log goes quiet, `/sys/log` shows every ring EMPTY, logd idle, serial silent | `kDebugLevel` was suppressed — `Ctrl+~` toggles it to `DEBUG_BOOT\|DEBUG_EXCEPTIONS` and back (it was a BARE `~` until 2026-08-18 and got hit by accident). The notice prints to the glass now, and `s_debug_suppressed`/`s_saved_debug_level` hold the state |
| A `/etc/logd.conf` setting does nothing | Missing the `key = ` (writing only the value is the natural mistake), or the file is past the reader's 8KB cap. logd complains on the console AND at the top of the log file |
| Replayed boot lines all share one timestamp | Fixed 2026-08-18: logd derives each line's clock from its ticks, not from drain time. If it returns, `logd_clock_for` is where |
| `tail`: "follow failed (-2)", or logd releasing the sink mid-session | FatFs makes a write-open EXCLUSIVE. Only happens on FAT — `/home` is ext2 now on both the P5 and QEMU |

### Testing

**Test Framework (`kernel/include/test_framework.h`):**
- `test_framework_init()`: Initialize test infrastructure
- `test_run_preboot()`: Tests run before scheduler starts
- `test_run_postboot()`: Tests run after scheduler enabled
- Test files in `kernel/test/`

**THE PROOF HARNESS LIVES IN `/tests`, NOT `/bin`** (ruled 2026-08-29). `/bin`
is the programs and tools a person runs; `/tests` is what proves the system
works — the badge-code fixtures `testrun` spawns, and the acceptance probes
you drive by hand when a feature is on trial (`gfxprobe`, `ptyprobe`,
`ttyprobe`, `uiprobe`, `gkeys`, `keytest`). Demos that exist to be *enjoyed*
rather than to prove something stayed in `/bin` (`gbounce`, `glogo`,
`fpu_orbit`).

- **The routing key is the SOURCE DIRECTORY, and nothing else.**
  `userland/apps/<name>/` → `/bin`, `userland/tests/<name>/` → `/tests`,
  `kernel/test/elf/` → `/tests`. No list of "which names are tests" exists
  anywhere, so none can go stale — moving a directory moves the program, on
  both the ext2 root and the FAT lifeboat.
- **The lifeboat carries `/tests` too**: it exists to be trusted while
  everything else is suspect, and one that cannot demonstrate the CPU, the
  kernel and the syscall floor are sound is asking to be taken on faith at
  the worst possible moment. ~750KB of its ~59MB spare.
- **`PATH=/bin:/tests`, in that order** (seeded in `kernel.c`), so fixtures
  stay typeable while a fixture can never shadow a command of the same name.
- **Fixtures draw link bases from the same pool as apps** (`app_bases.py` is
  fed both lists) — a fixture is an ordinary non-PIE ET_EXEC, and two
  programs at one base is the same debugger ambiguity whichever tree they
  came from. `app_bases.gdb` carries `add-symbol-file` lines for both.
- **Why it was worth doing on its own:** two directories make a name
  collision impossible rather than merely unlikely. `hog` existed twice —
  Chris's measuring instrument in `userland/apps` and a zero-syscall spinner
  in `kernel/test/elf` — both aimed at `/bin/hog`, and the instrument is what
  reached the image. `test_backstop_preemption`, whose entire premise is a
  thread that never enters the kernel, had been spawning the one that reads
  the clock as it spins. The spinner is now `/tests/nosyscall`, named after
  the property that makes it useful. Note the two image builders break such a
  tie by OPPOSITE rules and neither says a word: `debugfs write` refuses an
  existing name (first writer wins), `mcopy -o` replaces it (last one wins).

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

**Problem**: When running in task context (task's CR3 loaded, task's own stack),
you cannot safely access kernel structures — the task's stack lives at a
lower-half VA that is NOT mapped in kKernelPML4, and kernel data may not be
mapped in the task's address space. You must switch RSP to a kernel stack and
CR3 to kKernelPML4 first.

**The subtle trap — do NOT "switch, then reload cls".** This recipe (which this
file used to recommend) is broken at `-O0`: reloading `cls = get_core_local_storage()`
*after* the switch **stores the result into a local**, and that local is `[rbp-X]`
— still on the OLD task stack, because `mov rsp` does not move `rbp`. Every C
local after the switch (`cls`, `thread`, `task`, …) is accessed through that
stale `rbp`. It only *appears* to work because the lower-half VA happens to map
to *some* page under kKernelPML4, so the store/reload is self-consistent — while
silently scribbling on the wrong physical page, one `-O2` rebuild away from a
fault.

```c
// WRONG — the reload writes cls to a local on the old (now-unmapped) stack:
__asm__ volatile("mov rsp, %0" :: "r"(kernel_rsp));
__asm__ volatile("mov cr3, %0" :: "r"(kKernelPML4) : "memory");
cls = get_core_local_storage();          // stores to [rbp-X] on the OLD stack — UB
thread_t *thread = cls->currentThread;   // and every local access after is stale
```

**Correct rule: never touch a C local across the switch.** Get everything you
need into *registers* before the switch, then transfer control to code that runs
in a fresh frame on the new stack.

**Case 1 — the function does not return (e.g. `task_exit`).** Switch RSP/CR3 and
immediately `call` a `noinline` continuation, with NOTHING in between. The `call`
touches no local (it pushes the return address onto the already-switched kernel
stack), and the continuation's prologue puts its `rbp` on the kernel stack, so
all of *its* locals are valid:

```c
static void __attribute__((noinline)) task_exit_finish(void)
{
    // Fresh frame on the kernel stack; GS-based cls is valid under any CR3.
    core_local_storage_t *cls = get_core_local_storage();
    // ... all kernel-context work here ...
}

void task_exit(void)
{
    core_local_storage_t *cls = get_core_local_storage();
    // 16-align so the `call` keeps SysV alignment (rsp%16==8 at callee entry).
    uintptr_t kernel_rsp = (cls->kernel_interrupt_stack_top - 16) & ~(uintptr_t)0xF;

    // One asm block, NOTHING between the switch and the call. All operands are
    // loaded into registers BEFORE the switch, while rbp is still valid.
    __asm__ volatile(
        "mov rsp, %0\n\t"
        "mov cr3, %1\n\t"
        "call %2\n\t"
        :: "r"(kernel_rsp), "r"((uint64_t)kKernelPML4), "r"(task_exit_finish)
        : "memory");
    __builtin_unreachable();
}
```

**Case 2 — the function must return (e.g. `call_in_kernel_context`).** A
run-and-never-return continuation won't do: whichever frame restores RSP/CR3 has
to read the saved values, and that frame's `rbp` is the stale one. Do the whole
dance — save → switch → call → restore → switch-back — in a naked asm trampoline
(or one inline-asm block) so no C local is ever touched across a switch. Stash
inputs in CLS (GS-relative) beforehand, precisely because GS is valid under any
CR3/RSP.

**Key points**:
- CLS is reachable from any CR3 (shared upper-half) via `get_core_local_storage()`
  (GS:0) — that's why it's the safe place to stash values across a switch.
- Load everything you need into registers BEFORE `mov rsp`; after it, `rbp` is stale.
- The `"memory"` clobber on the CR3 load prevents the compiler reordering memory
  accesses across the switch.

**Symptoms of getting this wrong**:
- Intermittent corruption of unrelated kernel memory (the wrong physical page
  behind the stale `rbp`), or a #PF/#GP in the switch path if that VA is unmapped.
- Often masked at `-O0` and only surfaces under different codegen or memory layout.

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
- `kmalloc_dma(length, &phys)`: For memory a DEVICE will read or write
  (descriptor rings, bounce buffers, PRP lists). Returns the HHDM pointer the
  KERNEL uses; writes the physical address — the only address the device ever
  sees — to `*phys`. Page-aligned, zeroed, ordinary write-back cacheable (x86
  DMA is coherent), freed with plain `kfree(va)`. It IDENTITY-MAPPED until
  2026-08-19 — never resurrect that: the phys-as-pointer convenience leaked
  unmapped-on-free writable mappings and collided high phys with kernel VAs

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

**Do NOT special-case ktask here.** This section used to show a
`task->pml4v == kKernelPML4v` branch that wrote through the task VA directly
for "ktask/idle". Two things retired it:

1. **Idle tasks no longer share kKernelPML4** (2026-07-25). ONE TASK, ONE
   ADDRESS SPACE is now the rule — only ktask itself uses the kernel PML4.
   The old sharing meant N+1 tasks mapped their private blobs at the same
   FIXED lower-half VAs (`TASK_ARGV_VIRT`, `TASK_ENV_VIRT`,
   `TASK_EXIT_TRAMPOLINE_VIRT`), each silently destroying the last one's
   mapping. See the address-space note in `task_initialize` for the whole
   story; `/proc` is what finally made it visible.
2. The **unconditional** page-walk + HHDM write (previous section) is correct
   for every task including ktask, so the branch bought nothing but a way to
   be wrong. `createThread` (thread.c) already does it this way:

```c
// Correct for ANY task, no special cases:
uintptr_t phys_rsp = paging_walk_paging_table((pt_entry_t*)task->pml4v, newThread->regs.RSP);
if (phys_rsp && phys_rsp != 0xbadbadba)
    *(uintptr_t *)(phys_rsp | kHHDMOffset) = (uintptr_t)&task_exit_with_retval;
```

**The general rule this leaves behind:** a fixed per-task virtual address is
only safe in an address space that belongs to exactly one task. If you ever
make two tasks share a PML4 again, every fixed lower-half VA in `task_create`
becomes a collision, and the shared-counter dodge in `task_alloc_aligned` /
`task_reserve_task_virt` will NOT save you — it only covers counter draws, not
constants.
