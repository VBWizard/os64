# SCRIPT_STACK.md — a script stack real pages fit in

*Opus, 2026-10-31, at Chris's ask after he caught a `stack overflow` in
yonder's status bar. Reviewed by Fable the same day
(SCRIPT_STACK_REVIEW.md): option A, build it, with six findings, folded in
below (§ 9 lists where each went).*

## 1. The problem, measured

The P5's records hold 15 page loads, on 3 sites, that end a script in
`InternalError: stack overflow` (timers, and a video player's main script)
or `SyntaxError: stack overflow` (the parser, in a captcha script one of
them loads). Since #437 each one is also on the console, with the callers
that show the recursion.

yonder gives the script engine **128 KiB** of stack (`scripts.c`,
`config.limits.stack_bytes`). DOM_D10.md measured what that buys: recursive
JavaScript to a depth of **160** calls. A desktop browser allows thousands.
A page whose library is a few hundred calls deep, or whose script nests an
expression a few hundred levels, runs in every browser and fails here.

## 2. Why it is 128 KiB (DOM_D10.md § Guest stack evidence)

- The engine runs on yonder's **window thread**, and so does layout, which
  a script can force from inside a call (`getBoundingClientRect`,
  `offsetWidth`). The engine's stack and layout's native recursion share
  one stack.
- That stack is **1 MiB**: `THREAD_USER_STACK_SIZE` in
  `kernel/include/thread.h`, every user thread's, main and spawned alike.
- With the engine 160 calls deep, layout of the deepest page libhtml admits
  brought the whole stack to **870 KB used** (509 nested blocks, from a
  listener chain): that figure is the engine's share and layout's together,
  and layout's own is about **740 KB**. A 256 KiB engine left 71 KB of
  headroom, so the browser kept 128 KiB.

So the engine's limit cannot rise by itself: the thread under it has to.

## 3. What a stack costs today

`task_alloc_guarded_stack` (kernel `task.c`) allocates a stack **physically,
up front and contiguous**, with unmapped guard pages on each side. Raising
the constant for everyone multiplies that by every thread of every program
(yonder alone runs a window thread and a pool of workers). So the change is
**per program, not global**.

## 4. Options

**A. A program declares its main thread's stack size (built).** The
linker already writes one: `ld -z stack-size=N` sets `p_memsz` of the
`PT_GNU_STACK` program header (checked with our `x86_64-elf-ld`:
`-z stack-size=4194304` gives `GNU_STACK` a MemSiz of `0x400000`). glibc
reads that field as its default thread stack size and Fuchsia for its main
thread, so os64 reading it is interop. Every shipped binary already carries
the header with a MemSiz of 0 (`-z noexecstack`), so the rule is: absent or
0 is the default.
- **Where it is read:** `elf_can_load`, the one check every user program
  passes before `task_create` builds anything. yonder is dynamically linked
  and never takes `elf_load_from_path`'s segment walk, and the main thread
  is made in `task_initialize`, before either loader touches the image.
  `elf_can_load` already opens the file and validates the header; it answers
  the declared size beside its yes, and `task_create` carries it through
  `task_initialize` into `createThread`.
- **Bounds:** a size from 1 MiB to `THREAD_USER_STACK_MAX` (16 MiB) is kept,
  rounded up to a page; any other, the default, with a `printd` line saying
  so.
- **The size a thread got is recorded** in its `esp3Size`. The three places
  that re-derived it from the constant read that instead: the RSP seed
  (`createThread`), the stack guard's name and forensics line
  (`simple_exceptions.c`), and `/proc/<pid>/maps`' `[stack:n]` and `[guard]`
  rows (`procfs.c`).
- **A refused allocation degrades, never fails exec.** The stack is
  allocated inside `task_initialize`, after the task exists, and nothing
  unwinds a half-built task. So `createThread` asks for the declared size
  of the allocator that may refuse (`task_try_guarded_stack`, over
  `allocate_memory_try`; `allocate_memory_aligned` panics where no run of
  frames is free, Codex #441 rd1), and when it is refused, asks for the
  default and logs one line naming the thread and both sizes. yonder reads
  the size it really got, and § 5's clamp gives it today's 128 KiB. The
  default is asked as every stack always has been, and panics on exhaustion
  as every kernel allocation that cannot do without does, so it has no NULL
  to check.
- **yonder reads its size from `/proc/self/maps`:** the `[stack:<tid>]` row
  whose bounds hold the address of one of its locals. One reader, no new
  ABI. The engine runs on yonder's MAIN thread (yonder spawns only the
  ticker and libos64's work pool, neither of which runs a script), which is
  why declaring the main thread's size reaches it; moving the engine to a
  spawned thread would need § 8's spawned-thread size. And
  `JS_UpdateStackTop` (libjs `runtime.c`) re-reads the top at each run, so
  the engine's limit is measured from where that run starts, which is what
  § 5's sum assumes.
- Spawned threads keep 1 MiB, and every program that declares nothing is
  unchanged.
- RAM cost: 3 MiB more for yonder's window thread, one per yonder process:
  one contiguous run of frames, which the allocator zeroes.

**B. Demand-paged stacks.** Reserve a large virtual stack and back pages on
fault, as Linux does (8 MiB by default). Every program could have a big
stack at no cost until used. It is nearer than it looks: anonymous VMAs
already fault their pages in (`vma_resolve_backing_page`), and
`-fstack-clash-protection` already makes a frame touch its pages in order.
What it adds is a stack VMA kind whose fault handler will not grow into the
guard, with its burial and accounting. Named here, not proposed: A gives
the 4 MiB now, and A's declared size is exactly the reservation B would
take, so none of A is thrown away when B comes.

**C. Userland only: run each script task on a stack yonder allocates.**
A small trampoline switches the window thread to a large heap block for the
task. Rejected: heap memory has no guard page, so a native overflow (layout
past its measured depth, under a script) corrupts the heap silently instead
of faulting. And it hides yonder's real stack from the kernel's accounting.

## 5. Sizing (option A)

| Part | Size | Why |
|---|---|---|
| Engine limit | 2 MiB | about 13× today's depth, about 2,000 levels, measured as § 7 says |
| Native headroom under it | 2 MiB | layout's own worst case, about 740 KB, more than doubled, with the dispatch frames |
| Main thread stack | 4 MiB | the two |

`scripts.c` computes the engine limit as stack − 2 MiB, clamped to
[128 KiB, 8 MiB]. So a 1 MiB stack, an old kernel or a failed declaration,
gives today's 128 KiB, never more than fits.

## 6. Lifecycle

| Event | Stack | Engine limit |
|---|---|---|
| exec of a program with no `PT_GNU_STACK` size | 1 MiB, as today | (not yonder) |
| exec of yonder (declares 4 MiB) | 4 MiB, allocated at exec | read once at start: 2 MiB |
| the declared allocation is refused (`task_try_guarded_stack` answers NULL) | the default 1 MiB, logged with both sizes | read at start: the clamp's 128 KiB |
| a thread yonder spawns (workers) | 1 MiB, as today | workers run no scripts |
| a script recurses past 2 MiB | the engine's own check | `InternalError: stack overflow`, as today, said on the console |

## 7. Proof

- **The target depth, measured before choosing 2 MiB.** On QEMU, a
  measurement build with a larger main stack runs the corpus pages that
  overflow today, at engine limits of 256 KiB, 512 KiB, 1 MiB and 2 MiB.
  The record says, per page, the smallest limit at which the overflow is
  gone. 2 MiB is kept only if it covers every one, with room.
- **Headroom, re-measured:** DOM_D10.md's guest stack fixtures rerun at the
  new size, with the engine recursing to its new limit before forcing
  layout, the engine's depth recorded beside the total used. Sampled
  headroom must stay above layout's measured worst case.
- **Kernel:** a program with no size gets 1 MiB, one with 4 MiB gets 4 MiB
  (its usable range checked from inside, and its `/proc` maps row and
  guards where the stack really is), an out-of-range size gets the default,
  and a refused allocation gets the default, logged.
- **The P5:** the 15 loads' pages, before and after, on the console.

## 8. Not in this

- Option B, demand-paged stacks.
- Spawned threads' sizes (`os64_thread` with a size): no caller needs one yet.
- The parser's own recursion limit, which is the engine's and follows its
  stack limit.

## 9. The review, and where it went

| Fable's finding | Where |
|---|---|
| 1. the 870 KB is the whole stack; layout's share is about 740 KB | § 2 |
| 2. read the size in `elf_can_load`, carried into `task_initialize` | § 4 A, "Where it is read" |
| 3. the size is a thread field; three readers follow it | § 4 A, "The size a thread got is recorded" |
| 4. degrade to 1 MiB on a refused allocation | § 4 A and § 6 (the declared size through the fallible allocator, Codex's round 1; the default's allocation panics, so it has no refusal to log) |
| 5. yonder's door is `/proc/self/maps`; the engine is on the main thread | § 4 A, "yonder reads its size" |
| 6. option B is nearer than it read | § 4 B |


## 10. As built

- **Kernel.** `elf_can_load(path, &declared)` reads `PT_GNU_STACK`'s
  `p_memsz` (`elf.h` now names `PT_GNU_STACK`), looking through at most
  `ELF_STACK_PHDRS_MAX` headers. `task_create` carries it through
  `task_initialize` into `createThread(task, kernel, userStack)`, whose
  `user_stack` keeps a size from `THREAD_USER_STACK_SIZE` up to
  `THREAD_USER_STACK_MAX`, asks it of `task_try_guarded_stack` (the
  fallible twin of `task_alloc_guarded_stack`), falls back to the default
  when the frames are not there, logs each, and records what it gave in
  `esp3Size`. The pre-boot test `stack_try_refuses` proves a refusal is
  clean: NULL, nothing reserved, the books unchanged, the refusal counted. The RSP seed, `stack_guard_side` with the forensics
  line, and `/proc/<pid>/maps` read `esp3Size`. Spawned threads
  (`thread_join.c`) ask 0, the default.
- **Build.** `BIG_STACK_PROGRAMS` (`userland/GNUmakefile`, yonder and
  `stacktest`) link with `BIG_STACK_LDFLAGS`, `-z stack-size=4194304`; so
  does libdom's geometry probe.
- **yonder.** `scripts.c` reads its own row from `/proc/self/maps`
  (`own_stack_bytes`, into a static buffer) once, and gives the engine
  `yonder_script_stack_limit` of it: 2 MiB on the declared 4 MiB, and
  128 KiB on 1 MiB or a size it could not read.

### Measured (QEMU, 2026-10-31)

- **`/tests/stacktest`** (in testrun): the main thread's maps row is
  4 MiB, a recursion through 3.3 MB of it returns, and a spawned thread's
  row is 1 MiB. `stackprobe`, on the default stack, still dies in its guard
  as "Stack overflow", exit 139.
- **DOM_D10.md's geometry probe**, re-run on the declared 4 MiB with the
  engine at 2 MiB (2,097,152 bytes), the engine's depth beside the stack
  used (finding 1). 61 checks, 0 failures:

| Shape | Native nesting | Engine depth | Used bytes | Headroom bytes |
|---|---:|---:|---:|---:|
| Blocks | 509 | 3040 | 3167905 | 1026399 |
| Tables | 126 | 3040 | 2443841 | 1750463 |
| Inline-blocks | 254 | 3040 | 2962625 | 1231679 |
| Absolute | 254 | 3040 | 2803953 | 1390351 |
| Blocks from a listener chain | 509 | 3008 | 3159281 | 1035023 |

  The engine reaches 3,040 calls, where 128 KiB reached 160, and the
  deepest layout under it leaves at least 1,026,399 bytes of the stack
  unused, against the 128 KiB the probe requires. The probe's descent now
  runs to 16,384 calls, so the engine's own limit, not the loop, ends it.
