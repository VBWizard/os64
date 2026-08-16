# libos64 — the os64 C Library (design)

*The design record for userland's foundation library. Written for whoever
builds it (human or model) so the shape is decided before the first app
links against it. libos64 is judged against its ancestor — libChrisOS from
the 32-bit OS — keeping every good instinct, dropping the 32-bit-isms and
the hand-rolled machinery that modern tooling now gives for free. Design
settled 2026-07-09.*

## What this is

The single C library every os64 userland program links: string/memory
primitives, malloc, buffered and raw I/O, process control, pipes, and the
math/util helpers a shell and its utilities need. It sits ON TOP of the
kernel ABI (`abi/include/` — the shared syscall-number/struct contract;
today `kernel/include/syscall_numbers.h` until abi/ is scaffolded) and NEVER
reaches into kernel source (libChrisOS's `#include "../../../kproj/.../
syscalls.h"` is the anti-pattern we are explicitly correcting).

Headers live under `<os64/...>`; the startup stub is `launch`
(`userland/start/launch.S`, the crt0-equivalent — see [[project-userland-roadmap]]
and ABI.md's userland plan).

## libos64 is a SHARED object (the foundational fact)

os64 already has the dynamic-linking machinery — `kernel/src/shared_object.c`
+ `elf_loader.c`'s PT_DYNAMIC/DT_* walk, regression-tested by
`kernel/test/shlib/libtest.so`. libos64 is built as a `.so` and loaded that
way, exactly as libChrisOS was. The consequences ARE the design:

1. **One copy, shared.** The loader maps libos64's read-only code pages once
   and shares them across every task that uses it. Loading it a second time
   reuses the resident copy.
2. **Writable data is CoW-private per task.** Each task gets its own copy of
   any mutable global on first write (the `shlib_counter` test proves this:
   two tasks each see it start at 42). **Design rule: never assume a libos64
   global is shared between tasks — it isn't. Never assume it's fresh per
   *thread* either — CoW is per address space, so threads in one task DO
   share it.** (This is why thread-safety, below, is a first-class concern.)
3. **Internal calls bypass the PLT via hidden aliases.** When libos64 calls
   its own functions, those calls must NOT go through the PLT — it's slower
   and, worse, interposable (an app's rogue `strlen` could hijack the
   library's own use) and, in a hand-rolled loader, the lazy-binding
   resolver path is exactly where things crash (see the failure fingerprint
   below). libChrisOS solved this by hand: every function existed twice,
   `strcmpI` (hidden impl) + `strcmp` (visible wrapper). **We keep the
   intent and drop the duplication:** build with `-fvisibility=hidden`,
   mark the public API `__attribute__((visibility("default")))`, and use a
   `LIBOS64_HIDDEN(name)` macro (the glibc `hidden_proto`/`hidden_def`
   mechanism — an aliased hidden symbol) so ONE written function yields both
   the exported symbol AND a PLT-free internal alias. No `I` twins, no
   2×-source tax, same benefit libChrisOS earned the hard way.

## Header module map

Modular headers, one convenience roll-up — not libChrisOS's single flat
umbrella that pulled the world into every TU.

| Header | Contents |
|---|---|
| `<os64/io.h>` | raw `read`/`write`/`open`/`close`/`seek`/`stat`/`getdir` on handles |
| `<os64/stdio.h>` | buffered `FILE*` layer: `fopen`/`fread`/`fwrite`/`printf`/`fprintf`/`getline` |
| `<os64/mem.h>` | `os64_map`/`os64_unmap` (os64's anonymous mmap) and the heap on top of them: `os64_malloc`/`free`/`calloc`/`realloc`, `os64_heap_verify` — engine in `heap.c`, design in MALLOC.md |
| `<os64/runtime.h>` | `os64_runtime_init` — what `launch` stands up before `main` (today: the heap) |
| `<os64/str.h>` | str*/mem* primitives |
| `<os64/proc.h>` | `spawn`, `fork`, `exec*`, `waitpid`, `exit`, `kill`, `getcwd`/`chdir` |
| `<os64/pipe.h>` | `pipe`, `dup`/`dup2` |
| `<os64/opt.h>` | the `CommandLineOption` getopt-style parser (kept from libChrisOS — it was good) |
| `<os64/time.h>` | `time`, `sleep`, `gettime` |
| `<os64.h>` | includes all of the above for quick programs |

## The unified handle model

libChrisOS was split-brained: `open()` returned opaque `void*` FILE-ish
handles, while `0/1/2` were bare int fds elsewhere. **libos64 has ONE handle
type: a small `int` index into the per-task handle table** (the kernel's
planned table — ABI.md, deferred past the shell). Files, pipes, and GUI
windows (GRAPHICS.md) are all handles in that one namespace. `0/1/2` are
just the conventional first three.

- **Raw I/O** (`<os64/io.h>`): `read(handle, buf, len)` / `write(handle,
  buf, len)` return a byte count. NO size×nmemb — libChrisOS's
  `read(h,buf,size,length)` baked `fread`'s record semantics into the raw
  call, conflating two layers.
- **Buffered I/O** (`<os64/stdio.h>`): a `FILE*` wraps a handle and adds
  buffering + formatting. It is a LAYER on the handle, never a competing
  handle kind. `fread(ptr, size, nmemb, f)` lives HERE, where records make
  sense.

## Errors: in-band only, no errno

The ABI truth is the RAX:RDX value:status pair (ABI.md). libos64 wrappers
return the value and expose the status right at the call site — the error
check sits in the logical flow, no action at a distance. **There is no
`errno`** (decided — nothing is being ported that needs it, and libChrisOS's
`int errno;`-in-a-header was a non-thread-local landmine besides). A
stateless `strerror(code)` — pure code→string, zero global state — exists
only to render an error for printing.

## Process model: spawn AND fork/exec, both first-class

- **`spawn(path, argv, envp, fdmap)`** — the 90% launch path. The explicit
  `fdmap` lets it cover most of what a shell needs (redirect the child's
  handles) WITHOUT a fork.
- **`fork()` + `exec*()`** — fully supported and lovingly so. The kshell
  pipeline proves they're irreplaceable: `pipe()` → `fork()` → rewire the
  child's std handles → `exec()` → parent `waitpid()`. You cannot express
  "set up the child's world in the child, then exec" any other way.
  `exec`'s zoo in libChrisOS (`exec`/`execa`/`execb`) collapses to `exec` +
  `execv`/`execvp` (argv / PATH-search variants), one clear blocking
  contract. Fork's kernel enabler is the unfinished `justForked`
  register-load path (SCHEDULER.md / DEBTS).

  *(Note for maintainers: fork is the author's favorite pattern — it gets
  the full, careful treatment, not a grudging shim.)*

## Startup and shutdown

`launch` (crt0-equivalent) sets up argv/envp, calls `os64_runtime_init`
(`<os64/runtime.h>` — the library's own startup, which today stands up the
heap and registers its `/proc/<pid>/heap` report), will run ELF
constructors, calls `main`, and converts its return into the `exit`
syscall. **Register discipline in that stub:** argc/argv/envp arrive in
RDI/RSI/RDX and those are caller-saved, so they are parked in RBX/R12/R13
across the init call — not pushed, because pushing would change the stack
alignment `main` is entered with. libos64's own init/teardown ride ELF
`__attribute__((constructor))`/`(destructor)` — as libChrisOS did, which was
already right — registering an at-exit cleanup that closes tracked handles
and tears down malloc. The kernel's ring-3 exit trampoline stays as a
pure-ABI backstop (the fence rule, ABI.md): kernel-shipped user code speaks
only kernel ABI; anything speaking library vocabulary links from userland.

## Thread-safety (a baseline, not an afterthought)

os64 has real threads; the old OS had one thread per process, so libChrisOS
never had to care — and didn't (a single shared `printBuffer` served all of
printf). **libos64 is thread-safe by construction:** no shared scratch
buffers — format on the stack or in per-thread storage; any mutable library
state that threads in a task share (remember: CoW privatizes per address
space, NOT per thread) is either eliminated or locked. This is the single
biggest behavioral departure from the ancestor.

## Kept / dropped / renamed from libChrisOS

- **Kept:** ELF ctor/dtor lifecycle; at-exit handle-close tracking; the
  getopt-style option parser; malloc's page-caching sub-allocator with a
  corruption marker; the good Unix names (`fork`/`exec`/`pipe`/`dup`/
  `waitpid`/`getcwd`).
- **Dropped:** the `I`/non-`I` source duplication (→ hidden aliases);
  `errno`; kernel-source includes (→ `abi/include/`); the shared
  `printBuffer`; 32-bit-isms (uint32_t args, `[ebp+52]` envp, ebx/ecx/edx
  syscall regs → the 64-bit RDI/RSI/RDX/R10/R8/R9 contract).
- **Renamed:** `setcwd`→`chdir`; `signalTask`→`kill`; the `exec*` zoo →
  `exec`/`execv`/`execvp`. `takeADump()` survives as an affectionate alias
  to a real `coredump()`/`dump()` — some traditions are load-bearing. 💩

## Failure fingerprints (symptom → cause)

- **Crash/hang every time the library calls its OWN function (esp. early, or
  the first call to a given function):** the internal call is going through
  the PLT into the lazy-binding resolver instead of a hidden alias — the
  historical libChrisOS crash, fixed there by the `I` twins and here by
  `LIBOS64_HIDDEN`. If a new libos64 internal call regresses, check it
  resolved to the hidden alias, not the exported PLT stub.
- **A global "mysteriously shared" between two running programs:** it isn't
  CoW-private because it was never written before the fork/second-load, or
  the loader's CoW privatization regressed (see the `libtest.so` model).
- **printf output interleaves/corrupts under threads:** a shared scratch
  buffer survived — the `printBuffer` sin. Per-thread or stack only.
- **App links but faults calling a libos64 function:** built without
  `-fPIC`, or the exported symbol lost `visibility("default")` under
  `-fvisibility=hidden`.

## Known gaps / future work

- The `LIBOS64_HIDDEN` macro + the `-fvisibility=hidden` build wiring don't
  exist yet — first scaffolding task, before any real internal call count
  makes the PLT cost bite.
- Buffered `<os64/stdio.h>` layer is a second-phase item; raw `<os64/io.h>`
  + the syscall wrappers come first (enough for the shell).
- Math/soft-float (`sqrt`/`modf` for ps/top-style cpu%) intersects the
  kernel's `-mno-sse` / FPU-state debt (DEBTS) — defer FP-using utilities
  until FPU context-switch state exists.
- Everything here rides the userland roadmap order (ABI.md): scaffolding →
  interruptible syscall bodies → `read` → spawn/fork+wait → the shell.
