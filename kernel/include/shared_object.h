#ifndef SHARED_OBJECT_H
#define SHARED_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#include "dlist.h"
#include "elf_loader.h"
#include "elf_relocate.h"
#include "task.h"

// Fixed virtual window every dynamically-linked image (main executable or
// shared library) is placed in. Every task that loads a given image maps it
// at the SAME address — see shared_object.c's kSharedObjectNextVirt for why
// that's the simplification that avoids per-task re-relocation. Sits below
// the existing TASK_ARGV_VIRT (0x6f000000).
#define TASK_SHLIB_VIRT_BASE 0x50000000UL
#define TASK_SHLIB_VIRT_END  0x6f000000UL

typedef struct shared_object {
    char path[TASK_MAX_PATH_LEN];  // exact path this was opened with
    elf_image_t *image;            // parsed metadata (symtab/strtab/dynamic); image->file stays open for the object's lifetime, for lazy per-page reads
    Elf64_Phdr *phdrs;              // raw PT_LOAD table, kept for the object's whole lifetime (never freed — lazy reads need it at any later point)
    Elf64_Half phnum;
    uintptr_t load_bias;           // fixed shared virtual base for this image

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

/// @brief Load (or find already-loaded) the dynamically-linked image at
/// `path`. On first load: parses the ELF (headers, dynamic section),
/// bump-allocates a fixed shared load_bias from the TASK_SHLIB_VIRT_BASE
/// window, and recursively loads every DT_NEEDED dependency into
/// so->deps[] (a dependency cycle terminates because an object is
/// registered before its deps are loaded). Does NOT read any segment
/// content or apply any relocations — that happens lazily, per page, the
/// first time any task's page fault touches that page (see
/// shared_object_resolve_page). Rejects non-ET_DYN images: everything in
/// the shared window is placed at load_bias, so already-absolute ET_EXEC
/// vaddrs can't live here.
///
/// Every successful call — first load or a cache hit — increments refcount
/// by one, including the internal recursive calls that load dependencies
/// (so a dep's refcount counts each object that DT_NEEDs it, plus each
/// direct lookup). Returns NULL on any failure (bad path, bad ELF, wrong
/// e_type, allocation failure); panics if the shared virtual window is
/// exhausted or a DT_NEEDED dependency can't be loaded.
shared_object_t *shared_object_load_or_get(const char *path);

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
