# Dynamic linking feature — review companion

This is a walkthrough of everything added/changed to bring shared-library
support to os64, for module-by-module review. Nothing here is meant to be
committed to the repo as documentation — it's scaffolding for you to read
through, question, and then discard (or keep locally) once you're satisfied
you understand the code. The actual reference for "what the code does" is
always the code itself; this is just a guided tour of *why* it looks the way
it does.

**This is the second revision of this doc.** The first version described an
*eager* design (whole image loaded and relocated up front, at task-creation
time). After building and verifying that version, we talked through why it
gave up more laziness than necessary, and rebuilt it as the *lazy,
fault-driven* design described here — closer to how the old 32-bit loader
(and real OSes) actually do it. If any of this sounds like it's arguing with
an eager design you don't remember reading about, that's why.

Reading order matches dependency order: types → parsing → relocation engine
→ registry → task wiring → fault handler → test fixtures → build system.

**THIS IS A SNAPSHOT OF THE CODE AS IT WAS, not a document anyone maintains.**
It describes the loader at the moment it was written, and the loader has moved
since — the retention rules in particular. Where it says an object is resident
forever, that refcounts never decrement, and that teardown must not free a
dynamic task's image, read `shared_object.h`: refcounts have decremented since
2026-08-13, and since 2026-08-28 an object is unloaded when nothing holds it
and retired when its file is replaced on disk. Take the *reasoning* from here
and the *facts* from the code.

---

## 0. The big picture

Before this work, `elf_loader.c` only understood **static** ELF64 binaries:
walk `PT_LOAD` program headers, create a VMA (virtual memory area) per
segment, let the existing demand-paging fault handler fill in physical pages
lazily as the program touches them. No `PT_DYNAMIC`, no relocations, no
shared libraries.

The new code adds a second, parallel loading path for **dynamically-linked**
images (anything with a `PT_DYNAMIC` segment) — both shared libraries *and*
main executables, since it turns out they need identical treatment. Like the
static path, it's fully **demand-paged**: nothing is read from disk or
relocated until a real page fault touches that specific page. The difference
from the static path is what happens *on* that fault — see §6.

The key trick that makes cross-task sharing work under laziness: each
dynamically-linked image gets a small **per-page physical-page cache** (see
§4) living in a new global registry (`shared_object_t`), keyed by path.
Whichever task faults a given page in *first* pays the cost (disk read +
that page's relocations); every task after it — including a second instance
of the very same program run concurrently — gets a cache hit and reuses the
exact same physical page. Writable pages still get privatized per-task via
the **existing, completely unmodified** CoW fault handler the first time
anyone actually writes to them — resolving a page (shared) and writing to it
(private) are two independent trigger events.

Deliberate simplifications, so you know what's *not* here:
- No lazy PLT binding — every relocation on a given page is resolved once,
  eagerly, the moment that page is first touched. There's no
  `_dl_runtime_resolve`-style trampoline.
- No `DT_HASH`/`DT_GNU_HASH`-accelerated symbol lookup — plain linear scan.
- No TLS, no IFUNC, no `.init_array`/`.fini_array` execution, no symbol
  versioning, no `RPATH`/`RUNPATH`. Library search is a fixed prefix:
  `DT_NEEDED` name `foo.so` → path `/lib/foo.so`.
- No unloading — once a shared object is loaded it's resident forever.
  Refcounted for bookkeeping, but hitting zero doesn't free anything.
- Every dynamically-linked image (executable or library) gets a **fixed
  virtual address**, identical across every task that loads it. This is
  the single biggest simplification — it means a library's own relocations
  are computed once, ever, rather than per consuming task.
- No transitive library dependencies (a library can't itself have
  `DT_NEEDED` entries resolved) — symbol resolution searches a task's own
  flattened `shared_objects` list (main executable + every direct
  dependency, self included), not a real dependency graph.

---

## 1. `kernel/include/elf.h` — new ELF64 types

Unchanged from the first revision. Pure data definitions:

- **`Elf64_Dyn`**: the struct type for entries in a `PT_DYNAMIC` segment —
  just a tag (`d_tag`) and a value/pointer union.
- **`DT_*` tags** (`DT_NEEDED`, `DT_SYMTAB`, `DT_STRTAB`, `DT_RELA`,
  `DT_HASH`, `DT_JMPREL`, etc.) — the vocabulary used to find things inside
  a `PT_DYNAMIC` segment. Only the subset we actually use.
- **`R_X86_64_*` relocation type constants** (`RELATIVE`, `GLOB_DAT`,
  `JUMP_SLOT`, `64`, `PC32`, `NONE`) — what kind of fixup each relocation
  entry describes.

**Ask yourself when reviewing:** do you recognize `DT_HASH`? It's used in a
slightly unusual way here (see §2) — worth understanding *why* before moving
on.

---

## 2. `kernel/src/elf_loader.c` / `elf_loader.h` — parsing + per-page reads

### 2a. `elf_parse_image()` — unchanged from the first revision

```c
int elf_parse_image(vfs_file_t *file, elf_image_t **out_image,
                     Elf64_Phdr **out_phdrs, Elf64_Half *out_phnum);
```
Validates the ELF header, reads program/section headers, parses the dynamic
section if present (`PT_DYNAMIC` → `Elf64_Dyn` array → `symtab`/`strtab`/
`rela`/`jmprel`/`needed[]`, see §2b). Hands back the raw `PT_LOAD` table
(caller owns it). `elf_load_from_file()` (the static-executable path) is
still a thin wrapper around this, unchanged — this metadata parsing always
happens eagerly, dynamic or not, regardless of paging strategy: it's small
(one header, a handful of table entries) and needed for symbol resolution no
matter when the actual segment content gets read.

### 2b. `PT_DYNAMIC` parsing — unchanged from the first revision

Same two-pass walk of the `Elf64_Dyn` array (locate table addresses, then
resolve `DT_NEEDED` names now that `strtab` is loaded), same
`elf_vaddr_to_offset()` helper for translating link-time vaddrs to file
offsets.

**The `DT_HASH` trick** (worth understanding — it surprised me too, see the
plan-mode toolchain spike): the ELF spec gives you `DT_SYMTAB`'s *address*
but not its *size* or *entry count* — there's no `DT_SYMTABSZ` tag. Real
dynamic linkers derive the symbol count from `DT_HASH`'s second word
(`nchain`, which by construction equals the total number of dynamic
symbols). Confirmed this empirically against a real compiled `.so` before
trusting it (hash table layout: `[nbucket, nchain, buckets[nbucket],
chain[nchain]]`).

### 2c. `elf_compute_segment_ranges()` — **replaces** `elf_load_segments_contiguous()`

```c
int elf_compute_segment_ranges(const Elf64_Phdr *phdrs, Elf64_Half phnum,
                                size_t *out_total_pages,
                                elf_segment_range_t *out_segs, size_t max_segs, size_t *out_seg_count);
```
This is the bookkeeping half of what used to be one eager function: for each
`PT_LOAD` segment, compute its page-aligned `vaddr_off`/`pages`/`prot` and
the image's total page span. **No allocation, no I/O at all.** Used by
`shared_object_load_or_get` to size the per-page cache (§4) and by
`task_map_shared_object` (§5) to know what VMAs to create.

### 2d. `elf_read_page()` — new, the lazy read

```c
bool elf_read_page(vfs_file_t *file, const Elf64_Phdr *phdrs, Elf64_Half phnum,
                    size_t page_idx, void *dest);
```
Reads *one page's* worth of file content — `page_idx` is a global page index
into the image's own vaddr space (vaddr 0 = page 0), the same indexing the
per-page cache uses. Walks the raw `phdrs` to find which segment (if any)
overlaps this page, computes the file-offset overlap (same math
`elf_map_segment` already used, just narrowed to one page), and reads just
that overlap into `dest`. Anything past a segment's `p_filesz` (BSS) or
outside every segment (padding) is correctly left zero — `dest` must already
be zeroed by the caller.

Called from `shared_object_resolve_page` (§4) the first time *any* task
touches a given page — this is the thing that used to be
`elf_load_segments_contiguous`'s eager whole-image read, now happening one
page at a time, on demand.

### 2e. `elf_is_dynamic()` — unchanged from the first revision

```c
bool elf_is_dynamic(const char *path);
```
Opens the file, checks for a `PT_DYNAMIC` phdr, closes it. `task_create()`
(§5) calls this up front to choose which loading path to use — still the
one place a dynamically-linked image's file gets opened twice (once here,
once for real inside `shared_object_load_or_get`), still a deliberate,
accepted one-time cost.

`elf_read_at()` is still exposed (non-`static`) for the same reason as
before — `shared_object.c` needs it (now via `elf_read_page`, not directly).

---

## 3. `kernel/include/elf_relocate.h` / `elf_relocate.c` — unchanged

The relocation-applying engine itself didn't need to change at all for the
lazy redesign:
```c
int elf_apply_relocations(const Elf64_Rela *relocs, size_t count,
                          uintptr_t value_base, uintptr_t write_base,
                          const Elf64_Sym *symtab, const char *strtab,
                          elf_symbol_resolver_t resolver, void *ctx);
```

**The `value_base`/`write_base` split is still the crux of the whole
feature** — if anything, the lazy design leans on it *more* than the eager
one did:
- `value_base`: the image's *real, eventual* virtual address (`load_bias`).
  Baked into `R_X86_64_RELATIVE` entries (`value_base + addend`) — the
  address other code will actually dereference through, later.
- `write_base`: added to `r_offset` to compute *where to physically write
  right now*. Under the lazy design, this is `page_virt - page_vaddr` (see
  §4's `apply_page_relocations`) — a translation chosen specifically so that
  `write_base + r_offset` lands inside the one just-allocated page buffer,
  even though `r_offset` is relative to the whole image's vaddr space, not
  just this page.

Still handles exactly three relocation types (`RELATIVE`, `GLOB_DAT`/
`JUMP_SLOT` — identical handling, no lazy PLT to distinguish them for — and
`64`). Anything else fails hard rather than silently producing a broken
image.

---

## 4. `kernel/include/shared_object.h` / `shared_object.c` — the per-page cache

This is where almost all of the redesign's real complexity landed.

```c
typedef struct {
    char path[TASK_MAX_PATH_LEN];
    elf_image_t *image;             // metadata; image->file stays open for the object's lifetime
    Elf64_Phdr *phdrs;               // raw PT_LOAD table, kept forever — lazy reads need it at any later point
    Elf64_Half phnum;
    uintptr_t load_bias;

    elf_segment_range_t segs[ELF_MAX_SEGMENTS];
    size_t seg_count;

    size_t total_pages;
    uintptr_t *page_phys;            // page_phys[i] = physical page for page i, or 0 if untouched by anyone yet
    uint32_t lock;                   // spinlock guarding page_phys[] resolution

    uint32_t refcount;
    dlist_node_t *registry_node;
} shared_object_t;
```

Compare this to the first revision's `phys_base` (one physical address for
the whole image, everything already resident). Now nothing is resident until
something asks for it.

### `shared_object_load_or_get(path)` — much shorter now

1. Cache-hit path unchanged: linear-scan the registry, `refcount++`, return.
2. Cache-miss path: `elf_parse_image()`, then `elf_compute_segment_ranges()`
   to size things, `kmalloc` the `page_phys[]` array **zeroed** (nothing
   resolved yet), bump-allocate `load_bias` same as before.
3. **That's it.** No segment loading, no self-relocation happens here at
   all anymore — both moved into `shared_object_resolve_page`, invoked
   lazily. `so->phdrs` is deliberately *not* freed (unlike the eager
   design's temporary phdrs, which were only needed transiently) — lazy
   reads might need them at any point in the future, for any page.

### `shared_object_resolve_page(so, page_idx, resolver, ctx)` — new, the heart of it

```c
uintptr_t shared_object_resolve_page(shared_object_t *so, size_t page_idx,
                                      elf_symbol_resolver_t resolver, void *ctx);
```
Called from the page-fault handler (§6) whenever a task touches an
unresolved page of a dynamically-linked image. Three paths:

1. **Fast path, no lock**: `so->page_phys[page_idx]` already non-zero →
   return it immediately. This is the common case once a library has warmed
   up — every subsequent task, and every subsequent access by the *same*
   task, hits this.
2. **Slow path, first-ever touch**: acquire `so->lock` (a
   `__sync_lock_test_and_set` spinlock — same primitive `log.c`'s
   `kLogDWorkLock` already uses elsewhere in this codebase), *double-check*
   the cache (another core may have just finished resolving this exact page
   while we were spinning for the lock), then: `kmalloc_aligned` a page,
   read its content via `elf_read_page` — **routed through
   `call_in_kernel_context`**, see the callout below — apply every
   relocation (from both `.rela.dyn` and `.rela.plt`) whose `r_offset` falls
   within this page via the small `apply_page_relocations` helper, and
   finally write `so->page_phys[page_idx]` — **last**, since that's the
   flag every core's fast path checks unlocked. Release the lock, return.

**Why the disk read needs `call_in_kernel_context` (memory/vma.c) — this was
the one real bug in the redesign, not just a design nuance.** This function
runs *inside* the page-fault handler, which runs with whatever CR3 the
faulting task had loaded — not `kKernelPML4`. The NVMe/FAT driver code that
`elf_read_at` eventually calls into touches kernel-only buffers and MMIO
that simply aren't mapped in an arbitrary task's own page tables (they're
outside the shared upper-half region every task's PML4 copies from
`kKernelPML4`). The *existing* static-executable demand-paging path
(`vma_resolve_backing_page` in `vma.c`) already solved this exact problem
with a trampoline that switches to `kKernelPML4` + the core's kernel
interrupt stack, runs the read, switches back. My first draft of the lazy
reader called `elf_read_page` directly and immediately crashed — write
fault deep inside `nvme_submit_command`, CR2 pointing at some low physical
address that's only mapped in `kKernelPML4`. Fixed by exposing
`call_in_kernel_context` from `vma.c` (it was `static`) and routing the read
through it, mirroring `vma_resolve_backing_page`'s own `kernel_read_file`
pattern exactly — heap-allocated params struct and all (a stack-local params
struct would live on the *task's* stack, unreachable once CR3 has switched).

**The SMP race the lock exists for**: two cores can genuinely both first-
touch the same never-resolved page at the same moment (two tasks starting
up around the same time, both calling a function neither has called
before). Without the lock, both would read the file, both would allocate a
page, both would apply relocations, and whichever wrote `page_phys[idx]`
last would win — the loser's physical page becomes an orphaned duplicate,
silently defeating the sharing guarantee for that one page. The eager design
never had this problem (everything happened synchronously during
`task_create`, before any task could race it); going lazy reintroduced it,
and the lock is the fix.

### `shared_object_find_symbol` — unchanged

Still a linear scan over `image->symtab`, skipping undefined entries,
string-compared against `image->strtab`. Only its *caller* changed — see §5.

---

## 5. `kernel/src/task.c` / `task.h` — much thinner now

### `task_map_shared_object(task, so)` — no longer maps any physical pages

```c
static void task_map_shared_object(task_t *task, shared_object_t *so)
```
For each segment: create a demand-paged VMA (`MAP_SHARED_LIBRARY` flag, see
§6) at `so->load_bias + seg->vaddr_off` — **no `paging_map_pages()` call at
all anymore.** `vma->cow = true` on writable segments, exactly as before —
that part of the design didn't need to change, since CoW privatization is
triggered by a *write*, independent of how the page got resolved in the
first place. Still lazily creates `task->shared_objects` and appends `so` to
it.

### `task_resolve_shared_symbol(name, ctx)` — replaces `task_resolve_symbol`/`task_resolve_ctx_t`

```c
uintptr_t task_resolve_shared_symbol(const char *name, void *task_ctx);
```
The old version took a fixed-size local array built during `task_create`
(`needed_objs[]`) — that doesn't work anymore, because under the lazy design
a relocation might not get resolved until long after `task_create` returns,
so there's no "current call" to hold a local array. This version's `ctx` is
just the `task_t*` itself, and it walks `task->shared_objects` (the same
dlist `task_map_shared_object` populates) directly. It matches
`elf_symbol_resolver_t`'s signature exactly, so it's passed straight through
to `shared_object_resolve_page` from the fault handler with zero glue code.
A library's own self-references resolve through this same search (the
library is in its own task's `shared_objects` list too) — there's no
separate "self resolver" anymore.

### `elf_resolve_dynamic_dependencies(task, path)` — drastically shorter

No more relocation application here at all — just:
1. `shared_object_load_or_get(path)` for the main executable (still rejects
   non-`ET_DYN`, same reasoning as before).
2. `task_map_shared_object()` for it.
3. For each `DT_NEEDED` name: `shared_object_load_or_get` + `task_map_shared_object`.
4. Set `task->entryPoint`/`RIP` to `main_so->load_bias + e_entry`.

That's the whole function now. Everything that used to be steps 4-5 in the
eager version (walking `.rela.dyn`/`.rela.plt`, building a resolver context)
is gone — deferred entirely to whenever a page fault first needs it.

### The dispatch in `task_create()` — unchanged

Still one `if (elf_is_dynamic(...)) { ... } else if (elf_load_from_path(...) != 0) { panic(...); }`.

---

## 6. `kernel/src/driver/system/exceptions/simple_exceptions.c` — one new branch

Unlike the first revision (which needed zero changes here), the lazy design
adds exactly one new branch to `handle_page_fault`, in the "page not
present" section:

```c
if (vma->flags & MAP_SHARED_LIBRARY)
{
    shared_object_t *so = (shared_object_t *)vma->file;   // repurposed field — see memory/vma.h
    size_t page_idx = (aligned - so->load_bias) / PAGE_SIZE;

    uintptr_t phys = shared_object_resolve_page(so, page_idx, task_resolve_shared_symbol, task);
    if (!phys)
        panic("Failed to resolve shared-object page during fault resolution");

    paging_map_page((pt_entry_t *)task->pml4v, aligned, phys, PAGE_PRESENT | PAGE_USER);
    kPageFaultCount++;
    return;
}
```
Deliberately **never sets `PAGE_WRITE`**, even for a segment whose VMA
`prot` includes `PROT_WRITE` — that's still `vma->cow`'s job, checked
*earlier* in the same function, completely unmodified from before. This is
the same "first resolve is shared, first write is private, two independent
triggers" property called out in §0 and §4.

**What's still true from the first revision**: the CoW branch itself —
allocate a private page, `memcpy`, remap writable, `invlpg` — needed *zero*
changes. It was already generic enough (keyed only on `vma->cow` and the
page being present-but-read-only) to work regardless of how the page got
mapped in the first place.

New flag, in `memory/vma.h`:
```c
#define MAP_SHARED_LIBRARY 0x04
```
When set, `vma->file` is a `shared_object_t*` instead of the usual
`vfs_file_t*` — a deliberate field repurposing, not a new field, since a VMA
is only ever one or the other.

Also newly exposed from `memory/vma.c`/`.h`: `call_in_kernel_context` (was
`static`) — see §4 for why `shared_object_resolve_page` needs it.

---

## 7. Test fixtures: `kernel/test/shlib/libtest.c`, `kernel/test/elf/dyn_consumer.c`

Unchanged from the first revision — same two tiny freestanding programs,
same two real bugs found while building them (both still relevant, neither
specific to the lazy-vs-eager question):

1. **Port I/O vs memory-mapped I/O** in `dyn_consumer.c`'s first draft —
   treated COM1 (`0x3f8`) as a memory address instead of port-mapped I/O.
   Fixed with a tiny `outb()` inline-asm wrapper.
2. **Missing `-mno-red-zone`** — without it, a function's locals can live in
   the 128-byte SysV red zone below `%rsp` without `%rsp` ever being
   adjusted. An interrupt firing mid-function (here: the CoW page fault)
   pushes its exception frame starting *at* the current `%rsp`, landing
   directly on top of those locals. Fixed in both test Makefiles, matching
   what the kernel's own `CFLAGS` already do for exactly this reason.

One more debugging note worth recording here since it happened during the
lazy rework and is a good general lesson: after fixing the
`call_in_kernel_context` bug (§4), the test appeared to hang completely —
both tasks finished, printed correct results, called `task_exit`, and then
total silence, no more scheduler ticks, for 90+ real seconds. Extensive
tracing suggested a scheduler deadlock. It wasn't one: running the exact
same kernel/disk image *without* QEMU's `-d int,cpu_reset,pcall,guest_errors`
verbose tracing flag (which drastically slows and distorts emulated
per-core timing) completed normally in under a second past that point. The
"hang" was an artifact of my own test harness, not a bug in the kernel.
Lesson: when a timing-sensitive system misbehaves only under heavy
instrumentation, suspect the instrumentation before the system.

---

## 8. Build system

Unchanged from the first revision — `kernel/test/shlib/Makefile` (new),
`kernel/test/elf/Makefile` (extended for `dyn_consumer`), `kernel/GNUmakefile`
(`test-shlib` target + the `TEST_CFILES` glob fix so the fixtures don't get
compiled into the kernel binary itself), root `GNUmakefile`'s
`disk-populate` (`::/lib` + the two new binaries).

---

## 9. The regression test: `kernel/test/test_main.c`

Same test, same assertions, same expected values as the first revision —
and that's worth sitting with for a second: **the test didn't need to
change at all** when the entire loading strategy underneath it was rebuilt
from eager to lazy. `test_dynamic_linking()` still creates two tasks running
`/bin/dyn_consumer`, waits for both to exit, and checks:
1. Both got the identical packed retVal (`0x300031` — proves CoW isolation).
2. `shared_object_load_or_get("/lib/libtest.so")`'s refcount is exactly 3.
3. Both tasks' page tables map `libtest.so`'s code segment to the same
   physical page (proves true sharing, not per-task copies).

All three still hold under lazy loading — the whole point of keeping the
externally-observable contract (what a consuming task sees) identical while
changing *when* the work happens.

---

## Suggested review order, if doing this interactively

1. `elf.h` (5 min — just types, confirm you recognize the DT_HASH trick)
2. `elf_loader.c`/`.h` (parsing, unchanged from before, plus the new
   ranges-only/per-page-read split)
3. `elf_relocate.c`/`.h` (small, unchanged, but the value_base/write_base
   split is worth re-confirming now that it's used per-page)
4. `shared_object.c`/`.h` (the biggest section now — the per-page cache,
   the lock, and the `call_in_kernel_context` reuse)
5. `task.c`/`.h` (much thinner than before — see how little is left)
6. `simple_exceptions.c` + `memory/vma.h` (the one new fault-handler branch
   and the `MAP_SHARED_LIBRARY` flag)
7. Test fixtures + build system (lighter, the two bugs are still instructive)
8. `test_main.c`'s test (notice it's *identical* to the eager version —
   what does that tell you about what changed vs. what stayed a contract?)

Ask questions as we go; I'd rather over-explain a design decision now than
have it be a mystery six months from now when something in this area needs
to change.

---

# Post-review changes (third revision of the design)

A review pass over the code described above found five significant issues.
All five are now fixed; where a section above contradicts this addendum,
this addendum wins. Verified end-to-end in QEMU after the rework: all 27
tests pass, including `test_dynamic_linking` with strengthened assertions.

## 1. Page-straddling relocations (real bug — heap corruption)

Every supported relocation stores an 8-byte value at `r_offset`, so an
entry starting in the last 7 bytes of a page spills into the next page.
The old `apply_page_relocations` selected entries by "does r_offset fall in
this page" and then wrote 8 bytes — a straddling entry would write past the
end of the one-page `kmalloc_aligned` buffer (kernel heap corruption), and
the spilled bytes would never reach the next page at all (it reads raw file
content when resolved separately).

Fix: `elf_apply_relocations` was replaced by `elf_relocation_value()`
(elf_relocate.c), which computes the 8-byte value but does not write it.
`apply_page_relocations` (shared_object.c) now considers every entry that
touches *any* byte of the page (window widened 7 bytes left) and clips the
store to the page: each of two separately-resolved pages gets its own byte
slice of the same computed value, and the slices always agree because the
computation is deterministic.

## 2. `elf_read_page` stopped at the first covering segment (real bug)

Two `PT_LOAD` segments sharing a page is legal (small `p_align`, custom
linker scripts); the early `return true` meant the second segment's bytes
on a shared page were silently left zero. The loop now continues across all
segments; every one that overlaps the page contributes its slice.

## 3. The registry itself was not SMP-safe (latent race)

§4's `so->lock` protected page resolution, but `shared_object_load_or_get`
mutated `kLoadedSharedObjects`, refcounts, and the `kSharedObjectNextVirt`
bump with no lock at all — two cores racing `task_create` for the same path
would build two objects at *different* load_bias values, silently defeating
sharing. Now guarded by `kSharedObjectRegistryLock`, taken in the public
wrapper; the internal loader (`shared_object_load_or_get_locked`) is
recursive (see #4) and runs entirely under it. First-load parsing does disk
I/O under this lock — that's task-creation-time cost, never the fault path.

## 4. Symbol resolution is now scoped per-object, not per-faulting-task

**This supersedes §5's `task_resolve_shared_symbol` design entirely** (that
function is gone). The old design resolved a page's relocations against
whichever task faulted it first; since the resolved page is cached and
shared, a page could bake in an address from an image other consumers never
mapped — a fault-order-dependent crash. (This also matches the old 32-bit
loader's behavior, which resolved a library against the library's own
dependency set.)

- `shared_object_t` now carries `deps[]`: its own `DT_NEEDED` objects,
  loaded **recursively** at `load_or_get` time (transitive dependencies now
  work; an object is registered *before* its deps load, so dependency
  cycles terminate).
- Relocations resolve against `self, then deps[] in DT_NEEDED order`
  (`shared_object_scoped_resolver`) — the fault handler no longer passes a
  resolver at all; `shared_object_resolve_page(so, page_idx)` is the whole
  signature.
- `task.c` maps the **full dependency closure** into each task
  (`task_map_shared_object_closure`, with an already-mapped check for
  diamonds/cycles), which is what makes the scoping sound: any address a
  cached page can reference is inside the owning object's closure, and
  every consumer maps that closure.
- The `ET_DYN` check moved from task.c into `shared_object_load_or_get`,
  so it now also covers libraries (an ET_EXEC `DT_NEEDED` target previously
  would have gotten a bias added to absolute vaddrs and a page cache sized
  from a huge vaddr).
- **Refcount semantics changed**: every `load_or_get` call bumps — direct
  lookups and internal dependency loads. A dep loaded via `DT_NEEDED` is
  counted once per *dependent object*, not once per task. The regression
  test now asserts dyn_consumer refcount == 3 (task A + task B + test
  lookup) and libtest.so refcount == 2 (dyn_consumer's dep edge + test
  lookup), plus identity of `exe_so->deps[0]` with the registry entry.

## 5. No lock is held across the disk read anymore

**This supersedes §4's single `so->lock` design.** `page_phys[]` entries now
transition `0 → SHARED_OBJECT_PAGE_RESOLVING → phys`: a first-toucher
claims the slot with a CAS, does the read + relocations lock-free, and
publishes the final address with a fence. Cores resolving *different* pages
of the same object proceed in parallel; only their seek+read pairs are
serialized by a new `io_lock` (the VFS file handle has one seek position),
held strictly around the I/O. A core that loses the race for the *same*
page spins (with `pause`) on the slot until the winner publishes — see
"deferred" below.

## Smaller fixes in the same pass

- **Error-path leaks**: `elf_image_free()` (elf_loader.c) frees an image and
  all its tables, safe on partially-parsed ones; used by `elf_parse_image`'s
  dynamic-parse failure path (which previously leaked
  dynamic/strtab/symtab/rela/jmprel) and `elf_load_from_file`'s segment
  failure path. `shared_object_load_or_get` now closes the file and frees
  the image/phdrs on every post-parse failure.
- **DT_HASH pinned**: both fixture Makefiles now pass `--hash-style=sysv`.
  The kernel derives the dynamic symbol count from DT_HASH's `nchain`;
  bare-metal binutils emits it by default today (all os64 userland is built
  freestanding against yogi's own libc), but hosted toolchains default to
  gnu-only hashing — pinning guards against toolchain drift that would
  surface as a relocation panic far from the cause.
- **Intel asm syntax**: `dyn_consumer.c`'s `outb` rewritten Intel-style
  (`out dx, al`), `-masm=intel` added to `DYN_CFLAGS`. Project rule: all
  asm, kernel and fixtures alike, is Intel syntax.

# Known limitations deliberately deferred (do not forget these)

- **Input validation on ELF metadata**: `elf_relocation_value` doesn't
  bounds-check the relocation's symbol index against `symtab_count`, and
  nothing validates that `r_offset` lands inside the image's page span. The
  kernel is effectively a linker running in ring 0 — a malformed or
  malicious `.so` on disk is kernel memory corruption, not a clean load
  failure. Fine while every binary on the disk image is built by our own
  Makefiles; must be fixed before loading anything untrusted.
- **`task->elf` aliases the registry-owned `so->image`** for dynamic tasks
  (shared across every task running that binary). Future task-teardown code
  must NOT free `task->elf` for tasks with `shared_objects != NULL`.
- **Future physical-page reclamation must skip shared pages.** Today task
  reaping only frees thread structures. Whenever teardown learns to walk a
  task's PML4 and free pages, it must skip pages belonging to
  `MAP_SHARED_LIBRARY` VMAs (they're owned by the shared_object page cache
  and referenced by other tasks) — only CoW-privatized copies are the
  task's own.
- **Refcounts never decrement** — task exit doesn't release its objects
  (there is no unloading, so nothing is lost). When exit-time release is
  added, the regression test's refcount assertions (== 3 / == 2) must be
  revisited.
- **Same-page fault collisions still spin.** A core that faults a page
  mid-resolution by another core busy-waits (interrupts likely off) for the
  duration of that one page's disk read. Fixing this properly means
  sleeping the faulting thread and waking it on publish — scheduler
  integration that isn't worth it until it shows up in practice.
- Everything in §0's original simplification list still applies (no lazy
  PLT, no TLS/IFUNC/init_array, no unloading, fixed shared addresses,
  `/lib/` prefix search only) — except "no transitive library
  dependencies", which #4 above removed: `DT_NEEDED` now loads recursively.
