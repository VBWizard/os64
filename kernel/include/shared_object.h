#ifndef SHARED_OBJECT_H
#define SHARED_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#include "dlist.h"
#include "elf_loader.h"
#include "elf_relocate.h"
#include "task.h"

// Fixed virtual window every SHARED LIBRARY is placed in. Every task that
// loads a given library maps it at the SAME address — see shared_object.c's
// kSharedObjectNextVirt for why that's the simplification that avoids
// per-task re-relocation.
//
// MOVED OUT OF THE LOW 2GB, 2026-08-22, on the day userland gained its first
// real .so. This window used to be 0x50000000..0x6f000000 — which sat SQUARELY
// INSIDE the window userland/tools/app_bases.py hands app link bases out of
// (0x400000..0x6f000000). Nothing had ever collided only because nothing
// dynamic had ever loaded; the first library to land at 0x50000000 in a task
// whose executable hashed to that same slot would have mapped one straight
// over the other. Two separate allocators, one address range, no arbiter.
//
// The fix is to stop competing for the scarce space entirely. Apps are
// non-PIE `-mcmodel=small`, so THEY are confined to the low 2GB (32-bit
// displacements, absolute addressing) — that constraint is what caps the app
// count at ~128 slots of 16MB, and it is not negotiable without going PIE.
// A LIBRARY has no such constraint: it is -fPIC (RIP-relative internally, so
// it works at any address ±2GB of itself) and every app reaches it through
// 64-bit GOT slots. So the libraries move to the top of the user address
// space, the whole low 2GB goes back to the apps, and the two allocators can
// never meet again. Carved off the top of the task heap range — TASK_HEAP_END
// (task.h) was lowered by exactly this much, so the heap's bump allocator
// cannot walk into it either.
//
// THE WINDOW HAS TWO HALVES (2026-08-22):
//
//   0x00007F0000000000  PRELINK region, 4GB — 64 slots of 64MB, assigned at
//                       BUILD time by userland/tools/app_bases.py --libs and
//                       baked into the .so by link/lib.ld. A library that
//                       arrives with a base in here is placed exactly there
//                       (load_bias 0), not relocated.
//   0x00007F0100000000  BUMP region — everything that arrives WITHOUT a base
//                       of its own: PIE executables (ET_DYN, vaddr 0), and any
//                       .so built by hand. First come, first placed.
//   0x00007F8000000000  end
//
// Why prelinking exists: the bump allocator placed libraries in LOAD ORDER, so
// an address was stable only by accident and the build could not know it —
// which meant GDB had no symbols there and `step` into a library call silently
// degraded to `next`. Deciding the address at build time is what makes the
// debugger's symbol map truthful. (The same reason Windows DLLs have carried a
// preferred base since 3.x.) The two halves keep the two allocators from ever
// having to negotiate.
#define TASK_SHLIB_VIRT_BASE    0x00007F0000000000UL
#define TASK_SHLIB_PRELINK_END  0x00007F0100000000UL
#define TASK_SHLIB_VIRT_END     0x00007F8000000000UL

typedef struct shared_object {
    char path[TASK_MAX_PATH_LEN];  // exact path this was opened with
    elf_image_t *image;            // parsed metadata (symtab/strtab/dynamic); image->file stays open for the object's lifetime, for lazy per-page reads
    Elf64_Phdr *phdrs;              // raw PT_LOAD table, kept for the object's whole lifetime (never freed — lazy reads need it at any later point)
    Elf64_Half phnum;
    // The image's fixed shared virtual base — the value ADDED to every
    // link-time address in it. A library (ET_DYN) gets a bump-allocated slot
    // in the TASK_SHLIB_VIRT_BASE window; a non-PIE main executable (ET_EXEC)
    // gets ZERO, because its addresses are already absolute — it runs at the
    // base userland/tools/app_bases.py linked it for, in every task, which is
    // exactly the "same address everywhere" property sharing requires anyway.
    uintptr_t load_bias;
    // The image's lowest page-aligned link-time PT_LOAD vaddr — 0 for a
    // library, the app's link base for an executable. page_phys[] is indexed
    // from HERE, not from vaddr 0: an app linked at 0x68400000 would otherwise
    // claim a 1.7GB page span and a multi-megabyte cache array for a forty-
    // kilobyte image. Runtime address of page i == load_bias + vaddr_base +
    // i*PAGE_SIZE, which collapses to the familiar form for both cases.
    uintptr_t vaddr_base;
    // Was this object loaded as a program, or as somebody's library? Recorded
    // rather than inferred: the obvious guess is "load_bias == 0 means
    // executable", and it is WRONG for a PIE executable (ET_DYN, so it gets a
    // window slot exactly like a library does) — /bin/dyn_consumer, the
    // dynamic-linking fixture, is precisely that and /sys/shlib called it a
    // library on its first outing. Set by whichever entry point loaded it
    // first; an object can only be one or the other in practice, since
    // nothing DT_NEEDs a program.
    bool is_executable;

    elf_segment_range_t segs[ELF_MAX_SEGMENTS];
    size_t seg_count;

    // This object's own DT_NEEDED dependencies, loaded (recursively) at
    // shared_object_load_or_get time. This is the object's SYMBOL RESOLUTION
    // SCOPE: relocations in this object's pages resolve against `self, then
    // deps[] in DT_NEEDED order` — never against whichever task happened to
    // fault the page in first. That per-object scoping is what makes the
    // shared relocated pages sound: a cached page can only ever reference
    // addresses inside this object's own dependency closure, and every task
    // that maps this object maps that same closure (see task.c's
    // task_map_shared_object_closure), so the baked-in addresses are valid
    // in every consumer's address space by construction.
    struct shared_object *deps[ELF_MAX_NEEDED];
    size_t dep_count;

    // Per-page physical-page cache: page_phys[i] is the physical page
    // backing global page index i, 0 if no task has touched it yet, or
    // SHARED_OBJECT_PAGE_RESOLVING while some core is mid-resolution.
    // This is what makes cross-task sharing work under lazy loading —
    // whichever task faults a given page in first resolves it (reads the
    // file, applies that page's relocations) and every task after gets a
    // cache hit. Entries transition 0 -> RESOLVING (claimed by CAS) ->
    // final phys, so no lock is ever held across the disk read; see
    // shared_object_resolve_page.
    size_t total_pages;
    uintptr_t *page_phys;

    // Serializes seek+read pairs on image->file only. The VFS file handle
    // has one seek position, so two cores lazily resolving two DIFFERENT
    // pages of this object must not interleave their seek/read calls. Held
    // only around the actual file I/O — never while other cores' fast-path
    // page_phys[] checks need to make progress.
    uint32_t io_lock;

    uint32_t refcount;
    dlist_node_t *registry_node;
} shared_object_t;

// Sentinel for a page_phys[] entry claimed by a core that is currently
// reading/relocating that page. Cannot collide with a real physical page
// address — those are always 4KB-aligned.
#define SHARED_OBJECT_PAGE_RESOLVING 0x1UL

// Global registry of every dynamically-linked image loaded so far — main
// executables and shared libraries alike, keyed by the exact path each was
// opened with. Same dlist-registry pattern as kBlockDeviceDList. All
// registry mutation (and refcounts) is guarded by a registry-wide lock
// inside shared_object.c — task_create can run from multiple cores.
extern dlist_t *kLoadedSharedObjects;

/// @brief The runtime virtual address of page `page_idx` of `so`.
///
/// THE ONE PLACE THIS ARITHMETIC IS WRITTEN, and it earned that status the
/// hard way on 2026-08-22. When executables joined libraries in this registry,
/// `vaddr_base` appeared and the mapping stopped being the obvious
/// `load_bias + i*PAGE_SIZE`. Three sites did the sum by hand; two were
/// updated and the third — the burial-time reclaim guard in task.c, which
/// asks "does the page cache own this frame?" — was not. It therefore
/// answered NO for every page of every dynamically-linked EXECUTABLE (its
/// index landed hundreds of thousands of pages past the end of the array), so
/// a program's own text frames were freed back to the allocator when it
/// exited while the registry went on pointing at them. The next run of that
/// same program got a cache hit on a frame that now belonged to somebody
/// else, and executed it: #UD at the entry point, on the SECOND run only.
/// One inline function instead of three copies of a sum.
static inline uintptr_t shared_object_page_va(const shared_object_t *so, size_t page_idx)
{
    return so->load_bias + so->vaddr_base + page_idx * PAGE_SIZE;
}

/// @brief The LINK-TIME address of page `page_idx` — where the linker thought
/// this page would live, with no load bias applied.
///
/// Deliberately a separate function from shared_object_page_va, because the
/// difference between the two is exactly one term and getting it backwards is
/// silent. Two things want link-time addresses and nothing else does: reading
/// the page's bytes out of the FILE (program headers record link-time vaddrs)
/// and selecting which relocations fall on this page (r_offset is a link-time
/// address too). Everything that touches a page table or a fault address
/// wants shared_object_page_va instead.
static inline uintptr_t shared_object_page_link_vaddr(const shared_object_t *so, size_t page_idx)
{
    return so->vaddr_base + page_idx * PAGE_SIZE;
}

/// @brief The inverse: which page of `so` covers runtime address `va`?
/// Returns false (and leaves *out_idx alone) if `va` is outside the image —
/// which callers must check, since a wrong index is a read of the wrong
/// array element, not an obvious crash.
static inline bool shared_object_page_index(const shared_object_t *so, uintptr_t va, size_t *out_idx)
{
    uintptr_t base = so->load_bias + so->vaddr_base;
    if (va < base) {
        return false;
    }
    size_t idx = (va - base) / PAGE_SIZE;
    if (idx >= so->total_pages) {
        return false;
    }
    *out_idx = idx;
    return true;
}

/// @brief Load (or find already-loaded) the shared LIBRARY at `path`. On
/// first load: parses the ELF (headers, dynamic section), bump-allocates a
/// fixed shared load_bias from the TASK_SHLIB_VIRT_BASE window, and
/// recursively loads every DT_NEEDED dependency into so->deps[] (a
/// dependency cycle terminates because an object is registered before its
/// deps are loaded). Does NOT read any segment content or apply any
/// relocations — that happens lazily, per page, the first time any task's
/// page fault touches that page (see shared_object_resolve_page).
///
/// Requires ET_DYN. A library MUST be position-independent: it is placed at
/// a load_bias chosen by this registry, so absolute ET_EXEC vaddrs could not
/// be honoured even in principle.
///
/// Every successful call — first load or a cache hit — increments refcount
/// by one, including the internal recursive calls that load dependencies
/// (so a dep's refcount counts each object that DT_NEEDs it, plus each
/// direct lookup). Returns NULL on ANY failure — bad path, bad ELF, wrong
/// e_type, allocation failure, an unloadable DT_NEEDED dependency, or an
/// exhausted virtual window. NONE of those panic: as of 2026-08-22 userland
/// has real dynamic binaries, so every one of these is a condition ring 3
/// can produce by naming the wrong file, and "no" is an answer a shell can
/// print (DEBTS, the elf_can_load doctrine).
shared_object_t *shared_object_load_or_get(const char *path);

/// @brief Load (or find already-loaded) a MAIN EXECUTABLE at `path`.
/// Identical to shared_object_load_or_get in every respect but one: it also
/// accepts ET_EXEC, placing such an image at load_bias 0 — i.e. exactly at
/// the addresses it was linked for.
///
/// WHY EXECUTABLES GET THE EXTRA CASE (2026-08-22). os64's apps are non-PIE
/// `-mcmodel=small` binaries, each linked at its own fixed base by
/// userland/tools/app_bases.py so that a host debugger looking at QEMU's flat
/// linear address space can tell two running programs apart. Making them
/// dynamic did NOT require making them position-independent — an ET_EXEC with
/// a PT_DYNAMIC and a DT_NEEDED is an entirely ordinary thing, and it is what
/// every non-PIE dynamically-linked program on Unix was for twenty years. So
/// the executable keeps its absolute addresses and its debuggability, the
/// LIBRARY is the position-independent half, and they meet through the GOT.
///
/// The sharing guarantee is unaffected: an ET_EXEC image is at the same
/// address in every task by construction (it has only one address it can be
/// at), which is the very property the shared page cache needs.
shared_object_t *shared_object_load_executable(const char *path);

/// @brief Drop ONE reference on `so`. Decrements refcount under the registry
/// lock and does NOTHING else — the object stays loaded, its page_phys[]
/// cache stays warm, its deps keep their edges, and it is never unregistered.
///
/// WHY UNLOAD IS NOT PART OF THIS (Chris's ruling, 2026-08-13): until today
/// refcount only ever went UP, which made it a tally of "times anyone ever
/// asked for this object" rather than a count of live holders — useful for
/// nothing, and a number that could never become useful, since the moment a
/// task was buried the count was permanently wrong. Decrementing makes it
/// TRUE again: refcount is now (live task edges) + (dep edges from loaded
/// objects) + (direct lookups nobody released). Whether a zero refcount
/// should trigger an actual unload — dropping the registry node, the cached
/// pages, the still-open backing file — is a RETENTION policy, and it is
/// deliberately a separate slice, the same split already ruled for the block
/// read cache: correctness now, eviction when there is a reason to evict. At
/// this size of OS a warm library sitting at refcount 0 is a feature.
///
/// PAIRING RULE — read this before adding a call site. task_create takes
/// exactly ONE reference per dynamically-linked task: a single
/// shared_object_load_or_get() on the MAIN image (elf_resolve_dynamic_
/// dependencies). Dependencies are referenced ONCE, system-wide, when they
/// are first loaded recursively — NOT once per task. But
/// task_map_shared_object_closure puts the WHOLE closure on
/// task->shared_objects, so that list is NOT a list of references this task
/// owns. Releasing once per list node would drive every dependency's count
/// negative on the second burial. The undertaker therefore releases exactly
/// one edge, on the object whose image is task->elf. (Underflow is reported
/// loudly rather than silently clamped — see the implementation.)
void shared_object_release(shared_object_t *so);

/// @brief Resolve (or return the already-cached) physical page backing
/// `page_idx` of `so`. Called from the page-fault path (simple_exceptions.c)
/// when a task first touches a page of a library or dynamically-linked
/// executable. On a genuine first resolution: claims the page_phys[] slot
/// via CAS, allocates a page, reads its file content, and applies every
/// relocation (from both .rela.dyn and .rela.plt) that touches any byte of
/// this page, resolving symbols against `so`'s OWN dependency scope (self,
/// then so->deps[] in order — never the faulting task's view). Safe to call
/// concurrently from multiple cores: different pages resolve in parallel
/// (file I/O serialized by so->io_lock only), and a core that loses the
/// race for the same page spins until the winner publishes the result.
uintptr_t shared_object_resolve_page(shared_object_t *so, size_t page_idx);

/// @brief Look up a symbol by name in `so`'s own export table (its DT_SYMTAB
/// entries that are actually defined, i.e. st_shndx != SHN_UNDEF). Returns
/// the resolved absolute address (so->load_bias + st_value), or 0 if not
/// found. Building block for the per-object scoped resolver inside
/// shared_object.c, which searches `so` itself and then so->deps[] in order
/// (a library's own self-references resolve the same way an executable's
/// references to a library do).
uintptr_t shared_object_find_symbol(shared_object_t *so, const char *name);

#endif
