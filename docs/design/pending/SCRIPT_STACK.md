# SCRIPT_STACK.md — a script stack real pages fit in

*Opus, 2026-10-31, at Chris's ask after he caught a `stack overflow` in
yonder's status bar. A design for review: the change it asks for is the
kernel's, so it goes to Fable before any code (CLAUDE.md's rule for kernel
designs).*

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
- Layout of the deepest page libhtml admits used **870 KB** of it under the
  engine (509 nested blocks, from a listener chain). A 256 KiB engine left
  71 KB of headroom, so the browser kept 128 KiB.

So the engine's limit cannot rise by itself: the thread under it has to.

## 3. What a stack costs today

`task_alloc_guarded_stack` (kernel `task.c`) allocates a stack **physically,
up front and contiguous**, with unmapped guard pages on each side. Raising
the constant for everyone multiplies that by every thread of every program
(yonder alone runs a window thread and a pool of workers). So the change is
**per program, not global**.

## 4. Options

**A. A program declares its main thread's stack size (my lean).** The
linker already writes one: `ld -z stack-size=N` sets `p_memsz` of the
`PT_GNU_STACK` program header (checked with our `x86_64-elf-ld`:
`-z stack-size=4194304` gives `GNU_STACK` a MemSiz of `0x400000`). The ELF loader reads it. When it is present
and within bounds (say 1 to 16 MiB, a multiple of the page), the main thread
gets that stack, allocated as today's is; otherwise 1 MiB, as now. yonder
links with `-z stack-size=4M`, and its engine limit becomes the stack less
layout's headroom (`scripts.c` reads the size it was given). Every other
program is unchanged.
- Kernel cost: reading one program header, and threading a size through
  main-thread creation instead of the constant. Spawned threads keep 1 MiB.
- RAM cost: 3 MiB more for yonder's window thread, one per yonder process.
- Risk: a contiguous 4 MiB physical allocation can fail where 1 MiB would
  not. Then exec fails and says so, rather than handing yonder a stack its
  engine limit does not fit.

**B. Demand-paged stacks.** Reserve a large virtual stack and back pages on
fault, as Linux does (8 MiB by default). Every program could have a big
stack at no cost until used. It is the right long-term shape, and a larger
kernel change (the fault path, guard pages that grow, accounting), so it is
named here, not proposed.

**C. Userland only: run each script task on a stack yonder allocates.**
A small trampoline switches the window thread to a large heap block for the
task. Rejected: heap memory has no guard page, so a native overflow (layout
past its measured depth, under a script) corrupts the heap silently instead
of faulting. And it hides yonder's real stack from the kernel's accounting.

## 5. Sizing (option A)

| Part | Size | Why |
|---|---|---|
| Engine limit | 2 MiB | about 13× today's depth, about 2,000 levels, measured as § 7 says |
| Native headroom under it | 2 MiB | layout's 870 KB worst case, doubled, plus the dispatch frames |
| Main thread stack | 4 MiB | the two |

`scripts.c` computes the engine limit as stack − 2 MiB, clamped to
[128 KiB, 8 MiB]. So a 1 MiB stack, an old kernel or a failed declaration,
gives today's 128 KiB, never more than fits.

## 6. Lifecycle

| Event | Stack | Engine limit |
|---|---|---|
| exec of a program with no `PT_GNU_STACK` size | 1 MiB, as today | (not yonder) |
| exec of yonder (declares 4 MiB) | 4 MiB, allocated at exec | read once at start: 2 MiB |
| the allocation fails | exec fails, logged | — |
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
  layout. Sampled headroom must stay above layout's measured worst case.
- **Kernel:** a program with no size gets 1 MiB, one with 4 MiB gets 4 MiB
  (its usable range checked from inside), an out-of-range size is refused,
  and a refused allocation fails exec cleanly.
- **The P5:** the 15 loads' pages, before and after, on the console.

## 8. Not in this

- Option B, demand-paged stacks.
- Spawned threads' sizes (`os64_thread` with a size): no caller needs one yet.
- The parser's own recursion limit, which is the engine's and follows its
  stack limit.
