# ELF Loader — Test Integration and Return Value Capture

## Overview

This document covers the work done on the `elf_loader` branch to:

1. Move the ELF loader smoke test into the proper test framework with
   programmatic pass/fail verification (no manual log reading required).
2. Design and implement a reliable mechanism for capturing a task's return
   value (`RAX` at the moment the thread function returns).
3. Fix a critical `#GP` fault in `/logd` that prevented long SMP runtimes.

After these changes, the kernel ran for **72 minutes** with 2 SMP cores and
logd enabled — previously the system would `#GP` within 5–15 minutes.

---

## 1. ELF Loader Test (`test_elf_loader`)

### What it tests

- A small ELF binary (`kernel/test/elf/serial_ping.S`) is loaded from the
  root filesystem via the VFS layer, mapped into its own address space, and
  executed as a kernel task.
- The test verifies three things:
  1. The task exits within a reasonable time (1 second / 100 × 10 ms ticks).
  2. At least one page fault occurred during execution, confirming that demand
     paging fired (the ELF pages were not pre-faulted).
  3. The task's return value equals the magic constant `0xE1F0CA11`, proving
     the return value was captured correctly end-to-end.

### Test binary

`kernel/test/elf/serial_ping.S` writes the string `"ELF OK\n"` to COM1 via
`out` instructions and then does:

```asm
mov rax, 0xE1F0CA11   // magic return value
ret
```

The `ret` instruction lands in `task_exit_with_retval`, which captures `RAX`
before any C code can clobber it (see §2).

### Test registration

```c
// kernel/test/test_main.c
test_register("elf_loader", test_elf_loader, TEST_PHASE_POSTBOOT);
```

`TEST_PHASE_POSTBOOT` is required because the test needs the VFS and scheduler
both running.  If `kRootFilesystem == NULL` (no disk image mounted) the test
skips gracefully rather than failing.

### Demand-paging verification

```c
uint64_t faults_before = kPageFaultCount;
// ... create and run task ...
if (kPageFaultCount == faults_before)
    FAIL("no page faults — demand paging did not fire");
```

`kPageFaultCount` is incremented by the `#PF` exception handler each time a
demand page is faulted in.

---

## 2. Return Value Capture (`task_exit_with_retval`)

### The problem

The x86-64 System V ABI leaves a function's integer return value in `RAX` when
the function executes `ret`.  For ELF tasks, the thread "function" is the ELF
entry point; when it `ret`s, execution falls into the return address that was
pushed onto the stack during thread creation — `task_exit_with_retval`.

`task_exit` is a C function.  The very first thing any C function does is call
`get_core_local_storage()`, whose return value lands in `RAX`.  By the time
`task_exit` could write `RAX` into `task->retVal`, the register has already
been overwritten multiple times.

### The solution — asm trampoline with CLS chain walk

`task_exit_with_retval` (in `kernel/src/task_exit_asm.S`) runs before any C
code and stores `RAX` directly into `task->retVal` by walking a chain of
pointers that are always accessible regardless of the current stack or CR3:

```
GS base  →  CLS.self  →  CLS.currentThread  →  thread_t.ownerTask  →  task_t.retVal
```

In assembly:

```asm
task_exit_with_retval:
    mov  rcx, rax                               // rcx = ELF's return value
    mov  rax, gs:[0]                            // rax = CLS self-pointer
    mov  rax, [rax + CLS_CURRENTTHREAD_OFFSET]  // rax = currentThread
    mov  rax, [rax + THREAD_OWNERTASK_OFFSET]   // rax = ownerTask (task_t*)
    mov  [rax + TASK_RETVAL_OFFSET], rcx        // task->retVal = return value
    jmp  task_exit
```

`RCX` is used as scratch (caller-saved, but `task_exit` never returns so
clobbering it is harmless).  No stack use, no function calls, no intermediate
storage — `RAX` goes directly into the right place.

### Why not a global variable?

An earlier iteration used `volatile uint64_t g_task_exit_retval` as a
temporary holding register.  This has two problems:

- **SMP race**: on a 2-core system, two tasks could exit simultaneously and
  one would overwrite the other's retval before `task_exit` reads it.
- **Ownership**: the return value belongs to the task, not to a global.  The
  CLS-chain approach stores it exactly where it will be read from, with no
  intermediate copies.

### Struct offset validation

The hardcoded offsets in the `.S` file are validated at compile time by
`_Static_assert` checks in `kernel/src/task.c`:

```c
_Static_assert(offsetof(core_local_storage_t, currentThread) == 56, ...);
_Static_assert(offsetof(thread_t, ownerTask)                 == 384, ...);
_Static_assert(offsetof(task_t, retVal)                      == 168, ...);
```

If any struct layout changes, the build fails immediately with a clear message
rather than silently producing wrong runtime behavior.

### `task_exit` after the trampoline

Once `task_exit_with_retval` has written `task->retVal`, `task_exit` (C) does
not touch it.  It only copies `task->retVal` into `thread->retVal` for
convenience, marks both as exited, and calls `task_enqueue_dead_child`:

```c
if (thread) {
    thread->exited = true;
    thread->retVal = task ? task->retVal : 0;
}
if (task) {
    task->exited = true;
    task_enqueue_dead_child(task);
}
```

Tasks that call `task_exit` directly (without going through the trampoline)
get `retVal == 0`, which is the zero-initialized default.

---

## 3. `volatile` on `exited` and `retVal`

Both `task_t` and `thread_t` declare these fields as `volatile`:

```c
volatile bool     exited;
volatile uint64_t retVal;
```

Without `volatile`, an optimizing compiler is permitted to read `exited` once
into a register and never re-read it — turning a polling loop into an infinite
loop.  On x86 the hardware's TSO memory model makes the ordering safe in
practice, but `volatile` prevents the compiler-side caching that would break
even correct hardware ordering.

The correct long-term solution is C11 `_Atomic` with
`memory_order_acquire`/`memory_order_release`, but `volatile` is sufficient for
an x86-only kernel and carries no runtime cost.

---

## 4. logd `#GP` fix

### Root cause

A `General Protection Fault` in `/logd` after 5–15 minutes of SMP uptime was
traced to a buffer overflow that overwrote the return address on the stack.

**Chain of events:**

1. `vsnprintf` called the unbounded `vsprintf` on the caller's buffer first,
   then truncated — the overflow had already happened before the length check.
2. A prior overflow corrupted a `log_entry_t` in the ring buffer, inflating its
   `ticks` and `threadID` fields to large values.
3. `logd_drain_one` formatted the corrupted metadata into a 288-byte stack
   buffer (`print_buf2`).  The inflated fields pushed the formatted string past
   288 bytes, overwriting the return address.
4. The overwritten return address contained ASCII bytes from the format string —
   `0x3038666666667830` = `"0xffff80"` — confirming a string overflow into the
   return-address slot.

**Fixes:**

- `vsnprintf` now formats into a 2048-byte temporary buffer first, then copies
  at most `size-1` bytes into the caller's buffer.
- `print_buf2` in `logd_drain_one` widened from `MAX_LOG_MESSAGE_SIZE + 32`
  (288 bytes) to `MAX_LOG_MESSAGE_SIZE + 256` (512 bytes), with a comment
  documenting the worst-case breakdown.

After both fixes, the system ran for **72 minutes** without fault.
