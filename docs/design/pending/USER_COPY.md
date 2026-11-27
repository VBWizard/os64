# User copies: a copy that outlives its check costs the program, not the machine

The design behind `kernel/src/user_copy.S`, `copy_fault.c`, `user_copy.c` and the page-fault handler's two recovery points (2026-11-27; Fable's brief `BRIEF_USER_COPY_FIXUP.md` and the design state approved the same day). It retires the DEBTS row "A copy can outlive the check that allowed it".

**The check stays the gate; the recovery is the net under it.** `user_range_accessible` (syscall.c) refuses any pointer aimed at the kernel's memory, at a supervisor page or at page zero, and it is why a copy is only ever given a user address. What this adds catches only the race the check cannot see: a page that was there at the check and is gone at the copy. The copy then answers "not copied" instead of panicking. A copy that faults on an address the check would have refused stays a panic, because that is a kernel bug, the check bypassed.

## The race

A syscall checks a user range, then copies through it. Between the two, a sibling thread of the same program can take the memory away. There are two doors:

1. A sibling's `unmap` of a region just handed to a syscall.
2. A thread ending while a sibling's syscall copies into a buffer on its stack, so the reap (THREAD_STACKS.md) unmaps the stack under the copy.

Either is the program's own bug. Before this, the copy faulted in ring 0 on an address nothing mapped, and the machine panicked. A program could take the machine down by racing its own address space.

## Where user memory is touched

Ring 0 touches user memory by its user address only in the copy routines, `user_copy_in` and `user_copy_out` (user_copy.S).
- Everything else goes through syscall.c's helpers, which call them: `validate_and_copy_user_data` (behind `copy_user_buffer` and `copy_user_string`), `copy_to_user_buffer`, and `user_string_length`'s scan.
- The subsystems below a syscall (VFS, pipes, ttys, pty, the network, klog, GUI, audio, media keys) see only kernel buffers.
- A GUI canvas or plane is touched through the HHDM.

A new way into user memory goes through these routines, or it is a site to convert, never a second table entry.

The inventory behind this (the 2026-11-27 design state): three instructions, all inside the helpers, reached from 76 call sites. No spinlock was held at any copy, interrupts were off at every one (a syscall runs with IF clear), and every copy checked immediately before copying, after any park.

## The copy routines

Each routine moves its bytes the way the kernel's own `memcpy` does above its short sizes (shared/copy_fill.c, `copy_rep`): `rep movsb` up to the destination's first 8-byte boundary, `rep movsq`, then `rep movsb` for the tail. QEMU without KVM emulates a string instruction one step at a time, so eight bytes a step keeps a large read as fast there as it was through `memcpy`. Those three moves are the only instructions in each routine that touch user memory. A string move that faults leaves RCX holding the steps it had not done, and its failure exit adds the moves still to come and returns the bytes not copied, so 0 means copied. A fault inside an 8-byte step counts that whole step as not copied, which only matters to a caller that wants the count; every caller today asks only whether it is 0. The direction must be upward. A syscall arrives with DF clear (SFMASK clears bit 10, smp_core.c), the kernel's calling convention keeps it clear across calls and parks, and each routine starts with `cld` anyway.

**The resume table** (user_copy.S) lists each of those moves beside its failure exit. `memcpy` is never in it: a kernel bug inside `memcpy` is never mistaken for a user copy.

**The helpers' existing `false` is the failure answer**, the same one a refused check gives. Every caller therefore already had the error path. The 76 call sites were re-read against "acts on nothing after false"; the PR lists the two that needed looking at: sigreturn's two reads of its frame, and spawn's marshal and its frees.

## Which faults recover

A page fault resumes at a copy's failure exit when ALL of these hold (`copy_fault_resume`, pure, `tools/test_copy_fault_host.sh` beside a model, `tools/test_copy_fault_mutants.py`):

1. **Ring 0:** the error code's U/S bit is clear.
2. **No reserved-bit fault.**
3. **The faulting address is one the check could have passed:** `PAGE_SIZE <= cr2 < USER_CANONICAL_MAX`.
4. **The faulting RIP is a resume-table instruction.**
5. **The handler would otherwise panic** (asked only on those exits).
6. **The handler was given the fault's own frame**, which the unified exception path always does. The old path (`EXCOLD`) gives none, and every fatal fault there panics as before.

**Condition 5's exits:**
- the no-mapping exit, which is door 1's unmapped region and door 2's stack (no mapping covers a stack);
- inside a mapping: a protection violation on a present page, a shared-object page or backing page that cannot be resolved (no memory, a failed read), and the two CoW failures. `page_fault_in_mapping` returns those as reasons for a ring-0 fault, as it always did for ring 3, and the caller releases the mapping's hold before it recovers or panics.

**A shared-object mapping covering an address outside its image stays a panic** where it is found: that is the loader's invariant broken, not a fault.

**A fault the handler resolves** (a page the mapping covers but has not populated, which a copy may meet) resolves exactly as before, and the copy carries on.

**The fault's own frame.** The handler edits the frame the fault pushed, passed down from `exception_dispatch`, never the per-CPU slot `exception_current_context()`. A resolve that reads a file page can park and resume on another CPU, and that CPU's slot names whatever it was last doing. The frame lives on the faulting thread's kernel stack and moves with it. A caught SIGSEGV's delivery takes the same frame for the same reason.

No ring-3 call can make a file-backed mapping (an ELF segment or a shared object) go away while a fault parks on it, because `unmap` takes only `map`'s anonymous regions. So no fixture can make a parked resolve fail under a copy, and passing the frame makes it right by construction.

**Nothing new is locked.** The decision is made after the mapping's hold is released. It edits one field of the frame and adds two relaxed atomic counts.

## What the program sees

The call answers `BAD_USER_DATA` (-2), as it would for a buffer the check refused:
- A chunked write answers the bytes it wrote before the failing chunk, if any.
- A copy out stops where the page went: what had been copied stays where it landed.
- A copy in leaves a kernel buffer partly written, which the call does not use.

**A program that takes away its own buffer during a call loses what the call was handing it:**
- the bytes a read took from a pipe, a socket or the keyboard;
- the connection an accept took;
- the status a wait reaped;
- the event a GUI wait took.

## Measured strings

`user_string_length` (spawn's argv, a thread's name) measures a string without copying it. It reads it in pieces of at most 256 bytes through `user_copy_in`, so the copy routines stay the only instructions that touch user memory. A piece never crosses the end of the page the check has just passed, so a string ending near the end of the last mapped page still measures.

The measurement is a first look and the copy a second. The string can change between them, so no caller trusts the second look to agree with the first:
- spawn's marshal copies each argument's measured length plus one, into room the measure paid for, and refuses the argument if the byte at the measured end is not a terminator;
- the thread name is copied byte by byte up to its bound, refused if no terminator arrives, and checked as it arrived.

## Counts

- **Per task:** `copy_recoveries` in `/proc/<id>/status`.
- **Machine-wide:** `/sys/memory/usercopy` gives the recoveries since boot and since the last read, and the last one's task and faulting address.

Outside a program racing its own memory, as `/tests/copyrace` does on purpose, none is the expected number.

## The proof

- **`/tests/copyrace`** races both doors for thousands of rounds and passes when every call answered a count or -2 and the machine lived. It prints the kernel's recovery count; above zero is what shows it reached the net.
- **A kernel with the recovery off panics on it in QEMU.**
- **The pure rule** has its host test and mutants.
- **The whole ring-3 suite (`testrun`)** passes, since every syscall's copy changed.

## The other class: task memory through the HHDM

Writes and reads through the HHDM after a walk of a task's tables do not fault on a user address. Their race is a frame freed between the walk and the access, which trips the HHDM's use-after-free tripwire.

| Site | What keeps the frame |
|---|---|
| a new thread's or task's stack (createThread, `task_setup_ring3_exit_path`, thread_join_create) | not published yet |
| a signal frame (signals.c, the three delivery paths) | `syscall_unmap` clears each page under `signalLock`, which delivery holds across its resolve and its write. The thread-stack reap frees only after a waited invalidation, which no CPU can answer inside a delivery's interrupts-off section (thread_reap.c) |
| `/proc/<id>/mem`, the heap report, and the death reports' stack and code reads | `allocator_copy_from_task_va`: the walk and the read under the frame lock, each frame checked live |
| the CoW copy in the fault handler | the old frame is a shared object's cached page (CoW mappings are a library's writable segments, task.c), kept by the task's reference on the object; ring 3 cannot unmap a library's mapping |

The death reports (stack_trace.c `read_task_u64`, `user_fault_kill`'s bytes at RIP) used a plain walk and read. A dying task's siblings still run and can unmap between the two, so they moved onto `allocator_copy_from_task_va` with this design.

## What it does not cover

- **SMAP** is not enabled. Its DEBTS row is gated on the copy routines being the only user-access path, which they now are; it is ready to take.
- **A user pointer kept across a park and used from another context:** none exists. Every copy runs on the calling thread and checks after its park. The rule is that a user address is checked at its copy.
- **The old exception path (`EXCOLD`)** has no frame to resume, so it panics as before. It is a diagnostic path that retires with the unified one.
- **The other cure in the old DEBTS row**, a copy holding the mapping it checked, is not taken. No mapping covers a thread's stack, so it cannot close door 2.
