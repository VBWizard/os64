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

**REALIZED 2026-08-22.** This section was a design intention for six weeks;
it is now a description. `/lib/libos64.so` exists, all 64 apps carry a
`DT_NEEDED` on it, and the whole userland runs against one shared copy.

What it actually cost and bought, measured on the day:

| | Before | After |
|---|---|---|
| `userland/bin` on disk | 13MB | 1.8MB |
| `hello` | 197KB | 16KB |
| `ls` | 210KB | 22KB |
| `husk` | 237KB | 52KB |
| libos64 in RAM, whole system | one private copy per running program | **45KB, once** (11 resident pages, `refs: 5`) |

`/sys/shlib` reports all of it live — every loaded object, its base, its
resident pages, its refcount, its dependency edges. Read that first when
something about linking looks wrong.

The machinery it rides was already there and already tested —
`kernel/src/shared_object.c` + `elf_loader.c`'s PT_DYNAMIC/DT_* walk,
regression-tested by `kernel/test/shlib/libtest.so`. Which was the whole
point: os64 had exactly one shared library, and one proves capability. The
consequences ARE the design:

1. **One copy, shared.** The loader maps libos64's read-only code pages once
   and shares them across every task that uses it. Loading it a second time
   reuses the resident copy.
2. **Writable data is CoW-private per task.** Each task gets its own copy of
   any mutable global on first write (the `shlib_counter` test proves this:
   two tasks each see it start at 42). **Design rule: never assume a libos64
   global is shared between tasks — it isn't. Never assume it's fresh per
   *thread* either — CoW is per address space, so threads in one task DO
   share it.** (This is why thread-safety, below, is a first-class concern.)
3. **Internal calls bypass the PLT — via `-Bsymbolic-functions`, one linker
   flag.** When libos64 calls its own functions those calls must NOT go
   through the PLT: it's slower, it's interposable (an app's rogue `strlen`
   could hijack the library's own use), and in a hand-rolled loader the
   lazy-binding resolver path is exactly where things crash. libChrisOS
   solved it by hand — every function existed twice, `strcmpI` (hidden impl)
   + `strcmp` (visible wrapper).

   This file used to plan the glibc answer: `-fvisibility=hidden` plus a
   `LIBOS64_HIDDEN(name)` alias macro on every public function. **We didn't
   need it.** `ld -Bsymbolic-functions` binds the library's references to its
   own definitions at LINK time, which is the entire benefit with no source
   changes at all — measured on the first build: **libos64.so has 85
   RELATIVE + 6 GLOB_DAT relocations and ZERO JUMP_SLOT**, i.e. not one
   intra-library call goes through a PLT. glibc needs the macro machinery
   because it must keep *some* symbols interposable (malloc, above all).
   os64 has no interposition story and wants none, so the blunt flag is
   strictly better than the careful mechanism.

   A second reason the old hazard is gone entirely: **os64 has no lazy
   binding to crash in.** Every relocation on a page is resolved when that
   page is first touched, so there is no `_dl_runtime_resolve` equivalent
   anywhere in the system.

   (`-fvisibility=hidden` is still worth doing someday, for a different
   reason: it would shrink the exported symbol table, which the kernel's
   resolver LINEAR-SCANS. 158 exports is nothing; a few thousand would not
   be. Booked in DEBTS, not urgent.)

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
| `<os64/url.h>` | `os64_url_parse` — the `scheme://host:port/path` grammar RFC 1738 wrote once for http and gopher as siblings. Grammar only: no scheme table, no default ports, no percent-decoding, so a caller says which schemes it serves and what each implies |
| `<os64/proc.h>` | `spawn`, `fork`, `exec*`, `waitpid`, `exit`, `kill`, `getcwd`/`chdir` |
| `<os64/pipe.h>` | `pipe`, `dup`/`dup2` |
| `<os64/opt.h>` | the `CommandLineOption` getopt-style parser (kept from libChrisOS — it was good) |
| `<os64/time.h>` | `time`, `sleep`, `gettime` |
| `<os64/clip.h>` | `os64_clip_copy`/`paste`/`length` — the system clipboard. NO syscall behind it: it is open/read/write/close on `/sys/clipboard`, and a program that spells that out longhand gets the identical clipboard (CLIPBOARD.md) |
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

- **Crash/hang every time the library calls its OWN function:** an internal
  call went through the PLT — the historical libChrisOS crash, fixed there by
  the `I` twins and here by `-Bsymbolic-functions`. Check with
  `readelf -rW bin/libos64.so | grep JUMP_SLOT`: the correct answer is
  *nothing*. If that flag is ever dropped from `LIBOS64_LDFLAGS`, this comes
  back.
- **A breakpoint INSIDE a library function is hit, but `step` at the call site
  in the app steps OVER it** (and only "works" when a breakpoint happens to sit
  in the callee): the app has no `.plt` SECTION. `foo@plt` symbols are not in
  the file — BFD synthesizes them, and it finds the PLT **by section name**
  (`.plt`, `.plt.sec`, `.plt.got`). Fold the stubs into `.text` (as app.ld
  first did) and the synthetic symbols silently cease to exist, so GDB sees the
  stub as anonymous `.text` with no line info and none of the markings that say
  "trampoline — follow it through"; its rule for stepping into code with no
  line info is to step back out. Check with
  `objdump -d bin/<app> | grep @plt` — silence means broken. THE GENERAL
  LESSON, learned twice in one day: **ld and BFD look up the special ELF
  sections by their canonical names.** Any output section you invent instead
  loses machinery, and never with an error.
- **A dynamically-linked binary builds clean and has no `.dynamic` section
  at all** (`readelf -d` says "There is no dynamic section in this file", and
  undefined library symbols are left undefined with no complaint from ld):
  the link script folded the dynamic sections into other output sections by
  pattern (`*(.dynsym)` inside `.rodata`, etc.). ld places those
  linker-created sections itself and wants them as output sections under
  their own canonical names — anything else and they silently evaporate.
  Cost an hour on 2026-08-22; both `link/app.ld` and `link/lib.ld` carry the
  warning now.
- **An app faults at its very first instruction (#UD at the entry point), and
  only on the SECOND run of that program:** a shared-object cache page was
  freed at burial and handed to someone else while the registry still pointed
  at it — the next run got a cache hit on recycled memory. Look at
  `task_frame_is_shared_object_cache` and whether it is using
  `shared_object_page_index` rather than open-coded arithmetic.
- **`step` into a libos64 call behaves like `next` — the debugger refuses to
  enter the library:** GDB has no symbols at the library's address, and with
  no line info there is nothing for `step` to step into. Two independent
  mechanisms are supposed to supply them, because **CLI gdb and VS Code take
  different routes and only one of them reads the generated file**:
  1. `userland/bin/app_bases.gdb` carries an `add-symbol-file .../libos64.so
     <.text addr>` line — sourced by `./.gdbinit`, i.e. **CLI gdb only**.
  2. The kernel ANNOUNCES every shared object in a task's closure through
     `debug_task_loaded` (task.c), and the autoloader in
     `utility/os64_symbols.gdb` add-symbol-file's it — this is the route **VS
     Code** uses, since its `launch.json` sources that file and not the
     generated one. You should see `[os64] symbols: …/libos64.so` in the
     console during boot; if you don't, that hook is the thing that broke.

  Both only work because libraries are PRELINKED at a build-time address
  (link/lib.ld). While the kernel chose the address by load order, neither
  mechanism had a truthful address to report. Set breakpoints on library
  functions freely — `set breakpoint pending on` is already in
  os64_symbols.gdb, so one named before boot resolves the moment the library
  is announced.
- **A kernel #GP inside `strlen` with a register holding ASCII text:**
  something read a `MAP_SHARED_LIBRARY` VMA's `file` pointer as a
  `vfs_file_t*`. It is a `shared_object_t*`, whose first field is an inline
  `char path[]` — so the "pointer" is literally a piece of the library's own
  filename. See vma.h's comment on the field.
- **A global "mysteriously shared" between two running programs:** it isn't
  CoW-private because it was never written before the fork/second-load, or
  the loader's CoW privatization regressed (see the `libtest.so` model).
- **printf output interleaves/corrupts under threads:** a shared scratch
  buffer survived — the `printBuffer` sin. Per-thread or stack only.
- **App links but faults calling a libos64 function:** built without
  `-fPIC`, or the exported symbol lost `visibility("default")` under
  `-fvisibility=hidden`.

## Known gaps / future work

- `-fvisibility=hidden` is not wired up. It is no longer needed for the
  PLT-free internal calls it was originally planned for (`-Bsymbolic-functions`
  did that, see above) — the remaining reason is symbol-table SIZE, because
  the kernel's relocation resolver linear-scans `.dynsym`. At 158 exports
  that is free; revisit if libos64 ever exports thousands.
- Buffered `<os64/stdio.h>` layer is a second-phase item; raw `<os64/io.h>`
  + the syscall wrappers come first (enough for the shell).
- Math APIs (`sqrt`/`modf` for ps/top-style cpu%) are not implemented yet;
  x87/SSE2 execution itself is supported and its state is switched per thread.
- Everything here rides the userland roadmap order (ABI.md): scaffolding →
  interruptible syscall bodies → `read` → spawn/fork+wait → the shell.
