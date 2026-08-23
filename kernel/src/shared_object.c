#include "shared_object.h"

#include "elf_relocate.h"
#include "memory/kmalloc.h"
#include "memory/memcpy.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "paging.h"
#include "panic.h"
#include "serial_logging.h"   // printd + DEBUG_TASK (shared_object_release's ledger reporting)
#include "sprintf.h"
#include "strings/strcmp.h"
#include "strings/strcpy.h"
#include "strings/strlen.h"
#include "vfs.h"

dlist_t *kLoadedSharedObjects = NULL;

// Guards ALL registry state: kLoadedSharedObjects (creation and membership),
// every shared_object_t's refcount, and the kSharedObjectNextVirt bump.
// task_create can run concurrently on multiple cores, and without this two
// cores racing to load the same path would each build their own
// shared_object_t at DIFFERENT load_bias values — silently defeating the
// whole cross-task sharing design. Held across the parse/registration of a
// first-time load (which does disk I/O) — that's task-creation-time cost,
// never the page-fault path, which only touches page_phys[]/io_lock below.
static uint32_t kSharedObjectRegistryLock = 0;

// Bump-allocated shared virtual base for the next never-before-seen image.
// Every task that loads a given path maps it here, at the SAME address —
// see shared_object.h for why that's what lets us relocate an image exactly
// once, at first load, instead of re-relocating it per consuming task.
// Guarded by kSharedObjectRegistryLock.
// Starts ABOVE the prelink region: an image that brought its own build-time
// address is placed exactly where it asked, and this allocator only ever hands
// out addresses for images that did not (PIE executables, hand-built .so's).
// Two halves, no negotiation — see shared_object.h's window map.
static uintptr_t kSharedObjectNextVirt = TASK_SHLIB_PRELINK_END;

static void registry_lock(void)
{
    while (__sync_lock_test_and_set(&kSharedObjectRegistryLock, 1)) {
        __builtin_ia32_pause();
    }
}

static void registry_unlock(void)
{
    __sync_lock_release(&kSharedObjectRegistryLock);
}

uintptr_t shared_object_find_symbol(shared_object_t *so, const char *name)
{
    if (so == NULL || so->image == NULL || so->image->symtab == NULL || so->image->strtab == NULL) {
        return 0;
    }

    elf_image_t *image = so->image;
    for (size_t i = 0; i < image->symtab_count; i++) {
        Elf64_Sym *sym = &image->symtab[i];
        if (sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
            continue;  // undefined here — not something we export
        }
        if (strcmp(image->strtab + sym->st_name, name) == 0) {
            return so->load_bias + sym->st_value;
        }
    }
    return 0;
}

// The per-object symbol resolution scope: `so` itself first (self-references
// and its own exports), then its DT_NEEDED dependencies in link order. This
// is deliberately NOT the faulting task's shared_objects list — a resolved
// page is cached and shared with every task that maps this object, so the
// addresses baked into it must only ever come from images that are part of
// this object's own dependency closure (which every consumer also maps, by
// construction — see task.c's task_map_shared_object_closure). Resolving
// against the first faulting task's view instead would let a page capture an
// address from an image OTHER tasks never mapped — a fault-order-dependent
// crash in whichever task shares the page later.
//
// Matches elf_symbol_resolver_t's signature (ctx is the shared_object_t*).
static uintptr_t shared_object_scoped_resolver(const char *name, void *ctx)
{
    shared_object_t *so = (shared_object_t *)ctx;

    uintptr_t addr = shared_object_find_symbol(so, name);
    if (addr != 0) {
        return addr;
    }
    for (size_t i = 0; i < so->dep_count; i++) {
        addr = shared_object_find_symbol(so->deps[i], name);
        if (addr != 0) {
            return addr;
        }
    }
    return 0;
}

// Apply just the relocations (from one table) that touch ANY byte of
// [page_vaddr, page_vaddr+PAGE_SIZE). Every supported relocation stores an
// 8-byte value at r_offset, so an entry starting up to 7 bytes BEFORE this
// page can spill into it, and an entry in the last 7 bytes of this page
// spills out into the next — in both cases only the bytes that land inside
// `page_virt` are written here (the neighbouring page gets its own slice
// when it is resolved; both computations produce the identical value, so
// the two slices always agree). Without the clipping, a straddling entry
// would write past the end of the one-page buffer (heap corruption) and the
// spilled bytes would never reach the next page at all.
// Returns false if any entry could not be resolved — a relocation type we
// don't implement, an unresolvable symbol name, or a malformed entry. The
// caller abandons the page and the faulting task is killed; this used to
// panic, which meant one bad .so anywhere on disk could take the machine
// down from ring 3.
static bool apply_page_relocations(shared_object_t *so, const Elf64_Rela *relocs, size_t count,
                                    uintptr_t page_vaddr, void *page_virt)
{
    const uintptr_t reloc_size = sizeof(uintptr_t);  // all supported types store 8 bytes
    uintptr_t window_start = page_vaddr >= reloc_size - 1 ? page_vaddr - (reloc_size - 1) : 0;
    uintptr_t page_end = page_vaddr + PAGE_SIZE;

    for (size_t i = 0; i < count; i++) {
        uintptr_t off = relocs[i].r_offset;
        if (off < window_start || off >= page_end) {
            continue;
        }

        uintptr_t value;
        if (elf_relocation_value(&relocs[i], so->load_bias,
                                  so->image->symtab, so->image->symtab_count,
                                  so->image->strtab, so->image->strtab_size,
                                  shared_object_scoped_resolver, so, &value) != 0) {
            // Name the object and the offset — with lazy resolution the
            // failure surfaces at the first touch of some page, arbitrarily
            // far from the load, so the message has to carry its own context.
            printd(DEBUG_TASK, "shared_object: unresolved relocation in %s at link-time offset 0x%lx "
                               "(type %lu) — image cannot be run\n",
                   so->path, off, (unsigned long)ELF64_R_TYPE(relocs[i].r_info));
            return false;
        }

        // Clip the 8-byte store to this page. x86-64 is little-endian, so
        // byte k of the value belongs at vaddr off+k — slicing the value's
        // bytes by the same window as the destination keeps the two halves
        // of a straddling entry consistent across separately-resolved pages.
        uintptr_t write_start = off > page_vaddr ? off : page_vaddr;
        uintptr_t write_end = (off + reloc_size) < page_end ? (off + reloc_size) : page_end;
        memcpy((uint8_t *)page_virt + (write_start - page_vaddr),
               (uint8_t *)&value + (write_start - off),
               write_end - write_start);
    }
    return true;
}

// Parameters for reading a page's file content from kernel context — see
// call_in_kernel_context (memory/vma.c). Must be kmalloc'd (HHDM-accessible
// from kKernelPML4), never a stack-local: the task's own stack isn't mapped
// once CR3 has switched to kKernelPML4.
typedef struct {
    vfs_file_t *file;
    const Elf64_Phdr *phdrs;
    Elf64_Half phnum;
    Elf64_Addr page_vaddr;   // LINK-TIME address of the page, not an index
    void *dest;
    bool result;
} kernel_read_page_params_t;

static void kernel_read_page(void *arg)
{
    kernel_read_page_params_t *params = (kernel_read_page_params_t *)arg;
    params->result = elf_read_page(params->file, params->phdrs, params->phnum, params->page_vaddr, params->dest);
}

uintptr_t shared_object_resolve_page(shared_object_t *so, size_t page_idx)
{
    // volatile: other cores write this slot concurrently; every read below
    // must really hit memory, not a stale register.
    volatile uintptr_t *slot = &so->page_phys[page_idx];

    // Fast path: already resolved by some earlier fault (this task's or
    // another's). Aligned pointer-sized reads/writes are atomic on x86-64,
    // and the publishing store below is fenced, so once a slot reads as a
    // real physical address the page's content and relocations are fully
    // visible too. This is the common case once a library has warmed up.
    uintptr_t phys = *slot;
    if (phys != 0 && phys != SHARED_OBJECT_PAGE_RESOLVING) {
        return phys;
    }

    // Claim the slot: CAS 0 -> RESOLVING. Exactly one core wins a genuine
    // first touch; everyone else either sees the final value (done) or the
    // RESOLVING sentinel (spin until the winner publishes). No lock is held
    // across the file read this way — cores resolving DIFFERENT pages of
    // this object proceed in parallel (their file I/O serialized by io_lock
    // below), and only same-page losers wait, on the slot itself.
    uintptr_t prev = __sync_val_compare_and_swap(&so->page_phys[page_idx], 0, SHARED_OBJECT_PAGE_RESOLVING);
    if (prev != 0) {
        // Another core is resolving this SAME page; wait for it to publish.
        //
        // We must spin here with interrupts ENABLED. This runs inside the page-
        // fault handler, which is entered with interrupts masked (interrupt gate).
        // The core that won the CAS may block on timer-dependent I/O while
        // fetching the page (kernel_read_page -> NVMe -> nvme_wait_for_completion
        // -> wait()). Under BSP-only scheduling the BSP's APIC timer is the ONLY
        // thing advancing kTicksSinceStart, so if we spun here with interrupts
        // masked we would freeze that clock, the resolver's wait() would never
        // return, it would never publish, and both cores would deadlock. Enabling
        // interrupts keeps the timekeeper alive so the resolver can make progress.
        //
        // This is deliberately the ONE spot in the fault path that re-enables
        // interrupts: CR2 is already captured into a parameter by the asm stub, we
        // hold no locks here, and the loop is otherwise state-free — the safest
        // possible place to become preemptible. The real fix is to USLEEP on the
        // I/O completion signal rather than busy-waiting for the page at all.
        __asm__ volatile("sti");
        while ((phys = *slot) == SHARED_OBJECT_PAGE_RESOLVING) {
            __builtin_ia32_pause();
        }
        return phys;
    }

    // We own this page's resolution from here on.
    //
    // EVERY failure from here down releases the slot back to 0 and returns 0,
    // rather than panicking (2026-08-22, when userland gained real dynamic
    // binaries). The caller — the page-fault handler — turns a 0 into a
    // segfault-kill of the faulting task if the fault came from ring 3, which
    // is the honest outcome: one program's image could not be materialised,
    // and no other task's pages are affected. Releasing the slot rather than
    // leaving it RESOLVING matters: any core spinning on this page would
    // otherwise spin forever, so a failure that killed one task would hang
    // every other task that ever touched the same page.
    void *virt = kmalloc_aligned(PAGE_SIZE);
    if (virt == NULL) {
        printd(DEBUG_TASK, "shared_object: out of memory resolving page %lu of %s\n", page_idx, so->path);
        goto fail_release_slot;
    }
    memset(virt, 0, PAGE_SIZE);

    // Reading from the file may need to touch VFS/disk-driver structures
    // (e.g. NVMe queues/MMIO) that only kKernelPML4 has mapped — this fault
    // handler runs with whatever CR3 the faulting task had loaded, so the
    // read has to happen via the same kernel-context trampoline the
    // existing static-executable demand-paging path already uses (see
    // vma.c's vma_resolve_backing_page). params must be kmalloc'd, not
    // stack-local, per call_in_kernel_context's contract.
    kernel_read_page_params_t *params = kmalloc(sizeof(*params));
    if (params == NULL) {
        printd(DEBUG_TASK, "shared_object: out of memory reading page %lu of %s\n", page_idx, so->path);
        kfree(virt);
        goto fail_release_slot;
    }
    params->file = so->image->file;
    params->phdrs = so->phdrs;
    params->phnum = so->phnum;
    // page_idx counts from the image's own lowest PT_LOAD page, so the file
    // reader gets the page's real link-time address back (vaddr_base is 0 for
    // a library, the app's link base for a non-PIE executable).
    params->page_vaddr = shared_object_page_link_vaddr(so, page_idx);
    params->dest = virt;
    params->result = false;

    // io_lock serializes seek+read pairs on the one shared file handle (a
    // second core mid-resolution of a DIFFERENT page would otherwise move
    // the file position out from under us). Held only for the I/O itself.
    while (__sync_lock_test_and_set(&so->io_lock, 1)) {
        __builtin_ia32_pause();
    }
    call_in_kernel_context(kernel_read_page, params);
    __sync_lock_release(&so->io_lock);

    bool read_ok = params->result;
    kfree(params);

    if (!read_ok) {
        printd(DEBUG_TASK, "shared_object: read failed for page %lu of %s\n", page_idx, so->path);
        kfree(virt);
        goto fail_release_slot;
    }

    phys = (uintptr_t)virt - kHHDMOffset;

    // Relocations run lock-free: the page buffer is private to us until the
    // publishing store below, and everything they read (phdrs, symtab,
    // strtab, deps[]) is immutable after load_or_get.
    // LINK-TIME, not runtime: relocation r_offsets are link-time addresses.
    uintptr_t page_vaddr = shared_object_page_link_vaddr(so, page_idx);
    if (!apply_page_relocations(so, so->image->rela, so->image->rela_count, page_vaddr, virt) ||
        !apply_page_relocations(so, so->image->jmprel, so->image->jmprel_count, page_vaddr, virt)) {
        kfree(virt);
        goto fail_release_slot;
    }

    // Publish: this store is the flag every core's fast path (and same-page
    // spinners) check. The full fence guarantees the page content and
    // relocation writes above are visible before the slot stops reading as
    // RESOLVING. Only we can write this slot (we won the CAS), so no lock.
    __sync_synchronize();
    *slot = phys;

    return phys;

fail_release_slot:
    // Hand the slot back so a later attempt (or another task) can retry, and
    // so nobody spins on RESOLVING forever. Fenced for the same reason the
    // success store is: the spinners are reading this slot without a lock.
    __sync_synchronize();
    *slot = 0;
    return 0;
}

static shared_object_t *shared_object_find(const char *path)
{
    if (kLoadedSharedObjects == NULL) {
        return NULL;
    }
    dlist_node_t *node = kLoadedSharedObjects->head;
    while (node != NULL) {
        shared_object_t *so = (shared_object_t *)node->data;
        if (strcmp(so->path, path) == 0) {
            return so;
        }
        node = node->next;
    }
    return NULL;
}

// The real loader, called with kSharedObjectRegistryLock already held (the
// public wrapper takes it). Recursive: loading an object loads its
// DT_NEEDED dependencies too, which is why the lock lives in the wrapper —
// a per-call lock here would self-deadlock on the first dependency.
static shared_object_t *shared_object_load_or_get_locked(const char *path, bool allow_exec)
{
    if (path == NULL || strlen(path) >= TASK_MAX_PATH_LEN) {
        return NULL;
    }

    if (kLoadedSharedObjects == NULL) {
        kLoadedSharedObjects = kmalloc(sizeof(dlist_t));
        if (kLoadedSharedObjects == NULL) {
            return NULL;
        }
        dlist_init(kLoadedSharedObjects);
    }

    shared_object_t *existing = shared_object_find(path);
    if (existing != NULL) {
        existing->refcount++;
        return existing;
    }

    // Mount-routed like the ELF loader: a DT_NEEDED library resolves through
    // the mount table, so "/lib/…" comes from wherever "/lib" actually lives.
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return NULL;
    }

    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        return NULL;
    }

    elf_image_t *image = NULL;
    Elf64_Phdr *phdrs = NULL;
    Elf64_Half phnum = 0;
    if (elf_parse_image(file, &image, &phdrs, &phnum) != 0) {
        if (fs->fops->close != NULL) {
            fs->fops->close(file);
        }
        return NULL;
    }

    // WHICH IMAGE TYPES MAY LIVE HERE.
    //
    // ET_DYN (a library, or a PIE executable) is always fine: it is placed at
    // a load_bias this registry picks, which is what position-independent
    // means.
    //
    // ET_EXEC is accepted ONLY for a main executable (allow_exec) — os64's
    // apps are non-PIE, linked at fixed per-app bases by app_bases.py, and an
    // ET_EXEC with a PT_DYNAMIC is the perfectly ordinary shape of a non-PIE
    // dynamically-linked program. Such an image gets load_bias 0 below: its
    // addresses are already absolute, and it is at the same address in every
    // task by construction, which is exactly what the shared page cache
    // needs. A LIBRARY that is ET_EXEC is still refused — it would have to be
    // placed at a bias it cannot tolerate, and refusing here (rather than at
    // some later relocation) names the real problem: someone linked a .so
    // without -shared.
    if (image->ehdr.e_type != ET_DYN &&
        !(allow_exec && image->ehdr.e_type == ET_EXEC)) {
        printd(DEBUG_TASK, "shared_object: %s has e_type %u — %s\n",
               path, (unsigned)image->ehdr.e_type,
               allow_exec ? "not a loadable executable"
                          : "a shared library must be ET_DYN (linked with -shared)");
        goto fail_parsed;
    }

    shared_object_t *so = kmalloc(sizeof(*so));
    if (so == NULL) {
        goto fail_parsed;
    }

    Elf64_Addr vaddr_base = 0;
    if (elf_compute_segment_ranges(phdrs, phnum, &vaddr_base, &so->total_pages, so->segs, ELF_MAX_SEGMENTS, &so->seg_count) != 0) {
        kfree(so);
        goto fail_parsed;
    }
    so->vaddr_base = vaddr_base;

    so->page_phys = kmalloc(so->total_pages * sizeof(uintptr_t));
    if (so->page_phys == NULL) {
        kfree(so);
        goto fail_parsed;
    }
    memset(so->page_phys, 0, so->total_pages * sizeof(uintptr_t));

    strcpy(so->path, path);
    so->image = image;
    so->phdrs = phdrs;  // kept for this object's whole lifetime — lazy per-page reads need it at any later point, so it is NOT kfree'd here
    so->phnum = phnum;
    so->io_lock = 0;
    so->refcount = 1;
    so->dep_count = 0;
    so->is_executable = allow_exec;   // how it was ASKED for, not guessed from its bias

    // PLACE THE IMAGE. Three cases, and two of them are the same case:
    //
    //  1. ET_EXEC — a non-PIE executable. Already placed: it runs at the
    //     addresses it was linked for. Bias 0, consumes none of the window.
    //  2. ET_DYN with a non-zero vaddr_base — a PRELINKED library. Also
    //     already placed, by the build (link/lib.ld), for the debugger's
    //     benefit. Bias 0 as well; the only difference from case 1 is WHERE
    //     the address has to fall, which is checked below.
    //  3. ET_DYN based at zero — a genuinely floating image (a PIE executable,
    //     or a .so built without a base). This is the only case that needs an
    //     address chosen for it, and it gets the next bump slot.
    size_t span = so->total_pages * PAGE_SIZE;
    if (image->ehdr.e_type == ET_EXEC) {
        so->load_bias = 0;
    } else if (so->vaddr_base != 0) {
        // A prelinked library MUST land inside the prelink region. Anywhere
        // else is a build that disagrees with the kernel about the address
        // map — app_bases.py's LIB_BASE_START versus shared_object.h's window
        // — and the failure that disagreement would otherwise produce is a
        // library quietly mapped over the heap or over the bump region's
        // tenants. Refuse it by name instead.
        if (so->vaddr_base < TASK_SHLIB_VIRT_BASE ||
            so->vaddr_base + span > TASK_SHLIB_PRELINK_END) {
            printd(DEBUG_TASK, "shared_object: %s is prelinked at 0x%lx (+0x%lx) which is outside the "
                               "prelink region 0x%lx-0x%lx — rebuild it, or the two address maps disagree\n",
                   path, so->vaddr_base, span,
                   (uint64_t)TASK_SHLIB_VIRT_BASE, (uint64_t)TASK_SHLIB_PRELINK_END);
            kfree(so->page_phys);
            kfree(so);
            goto fail_parsed;
        }
        // ...and it must not land on top of an object already loaded. Within
        // one build that cannot happen (the assigner hashes and probes for a
        // unique slot), which is exactly why this check is worth having: it
        // fires for the case the build CANNOT see — a stale .so left on the
        // disk image from an older build, carrying an address that has since
        // been reassigned to something else. Two libraries silently sharing
        // pages would be an outstanding way to lose an evening.
        for (dlist_node_t *n = kLoadedSharedObjects->head; n != NULL; n = n->next) {
            shared_object_t *other = (shared_object_t *)n->data;
            if (other == NULL || other->load_bias != 0 || other->vaddr_base == 0)
                continue;   // only prelinked neighbours can collide with us
            uintptr_t other_span = other->total_pages * PAGE_SIZE;
            if (so->vaddr_base < other->vaddr_base + other_span &&
                other->vaddr_base < so->vaddr_base + span) {
                printd(DEBUG_TASK, "shared_object: %s prelinked at 0x%lx (+0x%lx) OVERLAPS %s at 0x%lx (+0x%lx) "
                                   "— a stale binary from an older build?\n",
                       path, so->vaddr_base, span, other->path, other->vaddr_base, other_span);
                kfree(so->page_phys);
                kfree(so);
                goto fail_parsed;
            }
        }
        so->load_bias = 0;
    } else if (kSharedObjectNextVirt + span >= TASK_SHLIB_VIRT_END) {
        // No longer a panic (2026-08-22). The window is 512GB and a library
        // is measured in kilobytes, so this needs thousands of distinct
        // libraries to fire — but it is reachable from ring 3 by spawning
        // programs, and "the OS died because you ran one more program" is not
        // an answer. The task that asked simply doesn't start.
        printd(DEBUG_TASK, "shared_object: shared library virtual window exhausted loading %s "
                           "(next=0x%lx span=0x%lx end=0x%lx)\n",
               path, kSharedObjectNextVirt, span, TASK_SHLIB_VIRT_END);
        kfree(so->page_phys);
        kfree(so);
        goto fail_parsed;
    } else {
        so->load_bias = kSharedObjectNextVirt;
        kSharedObjectNextVirt += span;
    }

    // Register BEFORE loading dependencies: if some dependency (directly or
    // transitively) DT_NEEDs us back, its lookup finds this entry and the
    // recursion terminates instead of looping forever. Our path/load_bias/
    // symtab are already valid at this point, which is all a dependent
    // needs from us.
    so->registry_node = dlist_add(kLoadedSharedObjects, so);

    // Load this object's own DT_NEEDED dependencies, recursively — they are
    // its symbol resolution scope (see shared_object_scoped_resolver) and
    // every task that maps this object maps them too (see task.c). A missing
    // dependency is unrecoverable FOR THIS IMAGE and would otherwise surface
    // much later as an undecipherable relocation failure at first fault, so
    // it is diagnosed here, at load time, naming both files.
    //
    // It used to PANIC (DEBTS: "ring-3-reachable if a dynamically-linked
    // program with a bad .so is ever spawned"). That day is today — userland
    // is dynamically linked now — so this unwinds and returns NULL instead,
    // and the spawn fails the same way a typo does. Nothing else in the
    // system is damaged by one unloadable program.
    for (size_t i = 0; i < image->needed_count; i++) {
        char lib_path[TASK_MAX_PATH_LEN];
        snprintf(lib_path, sizeof(lib_path), "/lib/%s", image->needed[i]);

        // Dependencies are LIBRARIES: allow_exec is false, so an ET_EXEC
        // masquerading as a .so is refused here rather than mis-placed.
        shared_object_t *dep = shared_object_load_or_get_locked(lib_path, false);
        if (dep == NULL) {
            printd(DEBUG_TASK, "shared_object: cannot load %s, needed by %s — not loading %s\n",
                   lib_path, path, path);
            goto fail_registered;
        }
        so->deps[so->dep_count++] = dep;
    }

    return so;

fail_registered:
    // Unwind a partially-built object that is already IN the registry (it is
    // registered before its dependencies load, so a cycle can find it). Undo
    // in the reverse order of construction:
    //
    // 1. Drop the reference each successfully-loaded dependency took. They
    //    stay loaded and warm at refcount 0 — that is the retention policy
    //    everywhere else in this file, not a leak (shared_object.h explains
    //    why unload-at-zero is deliberately its own slice).
    for (size_t i = 0; i < so->dep_count; i++) {
        if (so->deps[i]->refcount > 0) {
            so->deps[i]->refcount--;
        }
    }
    // 2. Leave the registry, so no later lookup can find a half-built object.
    dlist_remove(kLoadedSharedObjects, so->registry_node);
    // 3. Give back the virtual window slot IF we were the last bump — an
    //    exact reclaim in the common case, and in the uncommon one (a
    //    dependency load bumped it after us) the span is simply abandoned.
    //    Abandoning is safe by construction: the bump only ever moves
    //    forward, so an abandoned range is never handed to anyone.
    if (so->load_bias != 0 && kSharedObjectNextVirt == so->load_bias + span) {
        kSharedObjectNextVirt = so->load_bias;
    }
    kfree(so->page_phys);
    kfree(so);

fail_parsed:
    // Common failure cleanup once the ELF has been parsed. Reached directly
    // (nothing was ever registered) or fallen into from fail_registered
    // above (which has just un-registered it) — either way the file handle
    // and the parsed image plus its tables are exclusively ours to release,
    // because no other holder can reach this object any more.
    if (fs->fops->close != NULL) {
        fs->fops->close(file);
    }
    elf_image_free(image);
    kfree(phdrs);
    return NULL;
}

shared_object_t *shared_object_load_or_get(const char *path)
{
    registry_lock();
    shared_object_t *so = shared_object_load_or_get_locked(path, false);
    registry_unlock();
    return so;
}

shared_object_t *shared_object_load_executable(const char *path)
{
    registry_lock();
    shared_object_t *so = shared_object_load_or_get_locked(path, true);
    registry_unlock();
    return so;
}

// The counterpart of the reference every shared_object_load_or_get() takes.
// See the header for the PAIRING RULE — the short version is that a task owns
// exactly one edge (on its main image), not one per entry in its
// shared_objects list, so the undertaker calls this exactly once per buried
// dynamic task.
//
// Deliberately does not unload: see the header for why retention is its own
// slice. This function's whole job is keeping the number honest.
void shared_object_release(shared_object_t *so)
{
    if (so == NULL) {
        return;
    }

    registry_lock();
    if (so->refcount > 0) {
        so->refcount--;
        printd(DEBUG_TASK | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,
               "shared_object_release: %s refcount now %u\n",
               so->path, so->refcount);
    } else {
        // A release with no matching reference. LOUD rather than silent (the
        // house rule) but not a panic: this runs inside the undertaker, and
        // killing the machine during a funeral turns a bookkeeping bug into
        // an unbootable system. The count is already wrong by the time we get
        // here; saying so is the useful act. If this ever prints, the pairing
        // rule in shared_object.h has been broken by a new call site — start
        // there.
        printd(DEBUG_TASK,
               "shared_object_release: REFCOUNT UNDERFLOW on %s — a release with no "
               "matching reference (see the pairing rule in shared_object.h)\n",
               so->path);
    }
    registry_unlock();
}
