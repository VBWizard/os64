# SCRIPT_STACK_REVIEW.md — Fable's read of SCRIPT_STACK.md

*Fable, 2026-10-31, at Chris's ask. Against b4b15398 in the script-stack
worktree, checked against the kernel it names.*

## Verdict

**Option A, as proposed. Build it.** The problem is measured, the cause is
right (the engine's limit is bounded by the thread under it, and the thread's
size is a constant every thread shares), and declaring the size in the ELF is
the honest door: the linker already writes `PT_GNU_STACK.p_memsz` for
`-z stack-size`, glibc reads that field as its default thread stack size and
Fuchsia reads it for the main thread, so os64 reading it is interop, not
invention. The kernel half is reading one header and carrying one number;
no lock, no lifetime (the free releases the extent by its base and never
asks the size — `task_free_guarded_stack`, task.c).

Six findings. Two change the design's shape (2 and 4); the rest are text.
None blocks the slice.

## Findings

**1. The 870 KB is the whole stack, not layout's share.** DOM_D10's table
reports *sampled used bytes* with the engine already 160 calls deep: 870,481
used + 178,095 headroom = 1 MiB, the stack. Layout's own share under the
128 KiB engine is about 740 KB. The direction is conservative, so the 4 MiB
sizing stands, but §2 and §5 should say what the number is, because the next
person who re-measures with the engine at 2 MiB will read ~2.75 MiB "used"
and conclude layout grew. (§7's headroom rerun is the right proof; it should
record the engine's depth beside the total.)

**2. Read the size where both loaders meet, before the task exists.** The
main thread is created inside `task_initialize` (task.c, `createThread` at
~2034) and `task_initialize` runs at ~2672, BEFORE either loader touches the
image (~2757). And yonder is dynamically linked, so it never takes
`elf_load_from_path`'s segment walk at all; it goes through
`shared_object_load_executable`. The one place every program passes first is
`elf_can_load` (elf_loader.c ~700), which already opens the file and
validates the header while allocating nothing. Have it answer the declared
stack size too (0 = none), and carry that number into `task_initialize` →
`createThread` in place of the constant. "The ELF loader reads it" is true
but names the wrong walk.

**3. The size becomes a thread field, and three readers follow it.** Today
nothing records a thread's user stack size; three places re-derive it from
`THREAD_USER_STACK_SIZE`:
- `stack_guard_side` and the guard forensics line
  (simple_exceptions.c:783, :834) — with a 4 MiB stack, the "underflow"
  window would be computed INSIDE yonder's live stack and the forensics line
  would print a 1 MiB range;
- `/proc/<pid>/maps` (procfs.c:965) — would show a 1 MiB `[stack:n]` and a
  `[guard]` row over mapped memory;
- `createThread`'s RSP seed (thread.c:219).
Add `esp3Size` (or the house name) beside `esp3BaseV`, set once at
allocation, read by all three. `task_free_guarded_stack` needs nothing.
Spawned threads store the default and nothing else changes.

**4. On a failed allocation, degrade to 1 MiB; do not fail exec.** Two facts
make the lifecycle row "the allocation fails → exec fails, logged" dearer
than it reads. `createThread` does not check `task_alloc_guarded_stack` for
NULL (thread.c:158) — today a failure is a zero `esp3BaseV` and a dead task,
not a refusal. And the stack is allocated inside `task_initialize`, after
the task is built; `task_create`'s whole can-we-run check exists because
failing AFTER building means unwinding a half-built task, which nothing is
designed to do. So: ask for the declared size; if the frame table says no,
ask for the default, log one line naming the program and both sizes, and
record what was given. §5's clamp already finishes the job — yonder reads the
real size and runs at 128 KiB, exactly the "old kernel" row. One fewer failure
path, and the degrade is the behaviour you want from a machine short of
contiguous frames anyway. (Add the NULL check for the default while the line
is open; it is today's bug.)

**5. Name the door yonder reads its size through.** §4 says `scripts.c`
"reads the size it was given" and nothing in libos64 or the ABI exposes a
thread's stack today. The cheapest door already exists: `/proc/self/maps`
lists `[stack:<tid>]` with exact bounds, and the main thread finds its row by
the address of a local. One reader, no new ABI. If a second reader appears,
a `stack` line in the thread's `/proc` status is the next step. Two facts
worth a sentence in the doc because they carry the arithmetic:
`JS_UpdateStackTop` (runtime.c:376) re-reads the top at each run, so the
engine's limit is "2 MiB below where this run started", which is what §5's
headroom sum assumes; and the engine is on yonder's MAIN thread (yonder
spawns only the ticker and libos64's work pool, neither of which runs a
script), which is the assumption that makes "declare the main thread's
size" reach the engine. Say it, so moving the engine to a spawned thread is
known to require §8's item.

**6. Option B is nearer than §4 says.** Anonymous VMAs already fault their
pages in from the allocator (`vma_resolve_backing_page`, vma.c:103), and
`-fstack-clash-protection` (#422) already makes a frame touch every page in
order — the two things a growable stack needs. What B adds is a stack VMA
kind whose fault handler refuses to grow into the guard, plus its burial and
accounting. Still correctly named-not-proposed: A gives the 4 MiB now, and A's
declared size is exactly the reservation B would take later, so none of A is
thrown away when B comes. Just soften "a larger kernel change" so the next
reader does not over-fear it.

## Confirmed

- `-z stack-size=N` on this `x86_64-elf-ld` sets `GNU_STACK` MemSiz; every
  shipped binary already carries the header (MemSiz 0, `-z noexecstack` in
  LDFLAGS), so "absent or zero = default" is the whole rule.
- Contiguous 4 MiB + 32 KiB of guards is one frame run (TLSF bins above 64
  frames); the allocator zeroes it, so no extra clearing.
- runtime.c:460 refuses `stack_bytes > &local / 2`; with the stack VA near
  0x10000000, 2 MiB passes with room.
- The 2 MiB engine + ~740 KB layout + dispatch frames fits 4 MiB with over
  1 MiB spare; the engine's check is dynamic, so a layout→JS→layout chain is
  caught at every JS half.
- Reviewer tier: the kernel part has no concurrency and no lifetime, so the
  code round is the ordinary Codex round, not a Fable pass.
