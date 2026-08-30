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

// The same lock, for the ONE reader outside this file: /sys/shlib walks the
// registry and dereferences every object in it. Until 2026-08-23 it did so
// bare, while fail_registered on another core could dlist_remove + kfree the
// very node it was reading — so the file CLAUDE.md says to read FIRST when
// linking looks wrong was the one thing in the system a failing spawn could
// panic. A reader holds this for the walk; the price is that `cat /sys/shlib`
// waits out a first-time load's disk I/O, which is the correct answer.
void shared_object_registry_lock(void)   { registry_lock(); }
void shared_object_registry_unlock(void) { registry_unlock(); }

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
        goto fail_free_page;
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
        goto fail_free_page;
    }

    phys = (uintptr_t)virt - kHHDMOffset;

    // Relocations run lock-free: the page buffer is private to us until the
    // publishing store below, and everything they read (phdrs, symtab,
    // strtab, deps[]) is immutable after load_or_get.
    // LINK-TIME, not runtime: relocation r_offsets are link-time addresses.
    uintptr_t page_vaddr = shared_object_page_link_vaddr(so, page_idx);
    if (!apply_page_relocations(so, so->image->rela, so->image->rela_count, page_vaddr, virt) ||
        !apply_page_relocations(so, so->image->jmprel, so->image->jmprel_count, page_vaddr, virt)) {
        goto fail_free_page;
    }

    // Publish: this store is the flag every core's fast path (and same-page
    // spinners) check. The full fence guarantees the page content and
    // relocation writes above are visible before the slot stops reading as
    // RESOLVING. Only we can write this slot (we won the CAS), so no lock.
    __sync_synchronize();
    *slot = phys;

    return phys;

fail_free_page:
    // The page buffer exists but was never published, so it is still ours
    // to free. ONE label for the three failures past the allocation (review
    // 2026-08-23): "free the page, then release the slot" is a fact of
    // construction, and a fall-through ladder states it once where three
    // copies would each have to remember it — the same shape as the "three
    // copies of the sum, fixed two" bug this file's history confesses to.
    kfree(virt);
    /* fall through */
fail_release_slot:
    // Hand the slot back so a later attempt (or another task) can retry, and
    // so nobody spins on RESOLVING forever. Fenced for the same reason the
    // success store is: the spinners are reading this slot without a lock.
    __sync_synchronize();
    *slot = 0;
    return 0;
}

// The close hop's parameter block — kmalloc'd, never a stack local, because
// the continuation runs under kKernelPML4, which does not map a task's own
// stack (call_in_kernel_context's contract).
typedef struct {
    vfs_file_t *file;
    vfs_file_operations_t *fops;
} shared_object_close_params_t;

static void shared_object_close_in_kernel_context(void *arg)
{
    shared_object_close_params_t *p = (shared_object_close_params_t *)arg;
    p->fops->close(p->file);
}

// Close a file this module opened — an unloaded object's backing handle, or
// the handle a revalidation opened just to read an identity off.
//
// A close is real disk I/O since ext2 learned to reap orphaned inodes at last
// close, and reaping is the LIKELY outcome here: a replaced binary is exactly
// what an unload usually follows. So it needs kKernelPML4's mappings. But
// call_in_kernel_context resets RSP to the per-core interrupt stack's TOP, and
// an unload reached from the load path is ALREADY standing on that stack
// (task_create runs inside spawn_do_create's own hop), so hopping again would
// overwrite our own live frames. CR3 says which world we are in; handle.c's
// file closer answers the identical question the identical way.
//
// `name` is for diagnostics, and it is a parameter rather than file->f_path
// because an fs open stores the CALLER's pointer there — and this module's
// caller for a dependency is a stack buffer that dies with the load (vfs.h's
// f_path lifetime rule). The object's own copy of the path outlives it.
static void shared_object_close_file(vfs_file_t *file, const char *name)
{
    if (file == NULL) {
        return;
    }
    vfs_file_operations_t *fops = file->fops;
    if (fops == NULL && file->owner != NULL) {
        fops = ((vfs_filesystem_t *)file->owner)->fops;
    }
    if (fops == NULL || fops->close == NULL) {
        return;
    }

    uint64_t cr3;
    __asm__ volatile("mov %0, cr3" : "=r"(cr3));
    if (cr3 == (uint64_t)kKernelPML4) {
        fops->close(file);
        return;
    }

    shared_object_close_params_t *p = kmalloc(sizeof(*p));
    if (p == NULL) {
        // Out of memory at unload time. Say so rather than closing from the
        // wrong address space and faulting: the cost of not closing is one
        // leaked handle and one inode that waits for the next boot, and both
        // of those are survivable in a way a #PF in kworker is not.
        printd(DEBUG_TASK, "shared_object: no memory to close %s — file left open\n", name);
        return;
    }
    p->file = file;
    p->fops = fops;
    call_in_kernel_context(shared_object_close_in_kernel_context, p);
    kfree(p);
}

static void shared_object_drop_ref_locked(shared_object_t *so);

// Free everything `so` owns and strike it from the registry.
//
// CALLED ONLY AT REFCOUNT ZERO, and that is what makes it safe: every task
// that maps an object holds its main image's reference, and every object in
// that image's closure holds an edge from the image (task.c's
// task_map_shared_object_closure maps exactly the set that shared_object.c's
// scoped resolver referenced), so an object anything can still reach counts at
// least one. Zero means no page table points at these frames, no deps[] array
// points at this struct, and no VMA's `file` does either.
//
// The dependency edges are dropped LAST, from a copy taken before the struct
// dies, so a cascading unload never walks memory this call has already freed.
static void shared_object_unload_locked(shared_object_t *so)
{
    // The cached page frames — the whole point of unloading. kmalloc_aligned
    // gave them out (shared_object_resolve_page), so kfree takes them back,
    // and kfree HHDM-unmaps the page: anything still holding this address
    // faults loudly instead of reading a recycled frame.
    //
    // The count is of frames actually FREED, not of the image's page span:
    // resident is what the object was costing, and span is a property of the
    // file. /sys/shlib draws exactly the same distinction in its `pages` line.
    size_t freed = 0;
    if (so->page_phys != NULL) {
        for (size_t i = 0; i < so->total_pages; i++) {
            uintptr_t phys = so->page_phys[i];
            if (phys == 0) {
                continue;
            }
            if (phys == SHARED_OBJECT_PAGE_RESOLVING) {
                // Unreachable by the refcount argument above — a core mid-
                // resolution is a core whose task maps this object. Leak the
                // frame and say so rather than free one somebody is writing:
                // if this ever prints, the argument has a hole in it.
                printd(DEBUG_TASK, "shared_object: page %lu of %s is still RESOLVING at unload "
                                   "— leaking its frame (the refcount invariant is broken)\n",
                       (uint64_t)i, so->path);
                continue;
            }
            kfree((void *)(phys + kHHDMOffset));
            freed++;
        }
        kfree(so->page_phys);
        so->page_phys = NULL;
    }

    printd(DEBUG_TASK, "shared_object: unloading %s%s — %lu resident page(s) of %lu freed\n",
           so->path, so->retired ? " (retired)" : "",
           (uint64_t)freed, (uint64_t)so->total_pages);

    // The backing file, held open since load for the lazy per-page reads.
    // Closing it is what releases ext2's open-inode refcount — and so what
    // finally frees a replaced binary's orphaned storage.
    if (so->image != NULL) {
        shared_object_close_file(so->image->file, so->path);
        so->image->file = NULL;
    }
    elf_image_free(so->image);
    kfree(so->phdrs);

    dlist_remove(kLoadedSharedObjects, so->registry_node);

    // Give the virtual window slot back IF we were the last bump — the same
    // exact-reclaim-or-abandon rule the failed-load unwind uses, and for the
    // same reason it is safe: the bump only ever moves forward, so an
    // abandoned range is never handed to anyone. It matters more now than it
    // did there, because unloading makes bump slots something a running system
    // consumes repeatedly rather than once per distinct image. (Only images
    // that arrived WITHOUT an address of their own are here at all: a
    // prelinked library and a non-PIE executable both have load_bias 0.)
    if (so->load_bias != 0 &&
        kSharedObjectNextVirt == so->load_bias + so->total_pages * PAGE_SIZE) {
        kSharedObjectNextVirt = so->load_bias;
    }

    // Copy the edges out before the struct goes, then drop them: releasing a
    // dependency can unload IT, which drops its edges in turn, and none of
    // that recursion may reach back into memory we have already freed.
    shared_object_t *deps[ELF_MAX_NEEDED];
    size_t dep_count = so->dep_count;
    for (size_t i = 0; i < dep_count; i++) {
        deps[i] = so->deps[i];
    }

    kfree(so);

    for (size_t i = 0; i < dep_count; i++) {
        shared_object_drop_ref_locked(deps[i]);
    }
}

// Drop one reference, and unload at zero. THE RETENTION POLICY lives in that
// one `if` — see shared_object_release's header comment for why warmth is kept
// exactly as long as somebody is using it, and no longer.
static void shared_object_drop_ref_locked(shared_object_t *so)
{
    if (so == NULL) {
        return;
    }
    if (so->refcount == 0) {
        // A release with no matching reference. LOUD rather than silent (the
        // house rule) but not a panic: the most common caller is the
        // undertaker, and killing the machine during a funeral turns a
        // bookkeeping bug into an unbootable system. The count is already
        // wrong by the time we get here; saying so is the useful act. If this
        // ever prints, the pairing rule in shared_object.h has been broken by
        // a new call site — start there.
        printd(DEBUG_TASK,
               "shared_object: REFCOUNT UNDERFLOW on %s — a release with no "
               "matching reference (see the pairing rule in shared_object.h)\n",
               so->path);
        return;
    }

    so->refcount--;
    printd(DEBUG_TASK | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,
           "shared_object: %s refcount now %u\n", so->path, so->refcount);

    if (so->refcount == 0) {
        shared_object_unload_locked(so);
    }
}

// Find the object CURRENTLY serving `path`. A retired object is invisible
// here by design: it is still loaded and still running in whichever tasks
// hold it, but the name has been given to a different file, and answering
// with it would hand a new program the code it was refreshed away from.
static shared_object_t *shared_object_find(const char *path)
{
    if (kLoadedSharedObjects == NULL) {
        return NULL;
    }
    dlist_node_t *node = kLoadedSharedObjects->head;
    while (node != NULL) {
        shared_object_t *so = (shared_object_t *)node->data;
        if (!so->retired && strcmp(so->path, path) == 0) {
            return so;
        }
        node = node->next;
    }
    return NULL;
}

// `so` no longer describes the file at its own path. It is struck from lookups
// and left to its holders, who go on running the code they started with. A
// retired object keeps its window slot and its frames until the last of them
// exits; that is the honest cost of letting a refresh take effect without
// disturbing what is already running, and /sys/shlib reports it.
//
// RETIRING FREES NOTHING, EVER — the unload always comes later, from the
// release that drops the last holder. A registered object is created with one
// reference and unloaded the moment it has none, so every object this walk can
// reach counts at least one, and there is never anything here to sweep.
//
// RETIREMENT IS TRANSITIVE, and that is not tidiness. A dependent's cached
// pages have the retired object's addresses baked into them
// (shared_object_scoped_resolver), so a program whose own binary did not
// change can still only ever run against the old library — retiring it makes
// the next load rebuild it against the new one, which is what a dynamic linker
// does at every exec.
//
// It is also what keeps two builds of one library from meeting inside a single
// task's closure. They share a prelink address (the build assigns it by name),
// and a task maps its WHOLE closure, so a diamond — A needing B and C, where B
// was resolved before the replacement and C after — would map both at the same
// virtual address. A dependent retired here can never be found by a later
// closure walk, so the old copy is reachable only through objects that also
// predate the replacement.
static void shared_object_retire_locked(shared_object_t *so)
{
    so->retired = true;

    // Marked to a fixpoint: retiring an object can make a dependent stale,
    // which can make ITS dependents stale, and the registry is a list in no
    // particular order. Each pass either marks something new or ends the loop.
    bool changed = true;
    while (changed) {
        changed = false;
        for (dlist_node_t *n = kLoadedSharedObjects->head; n != NULL; n = n->next) {
            shared_object_t *o = (shared_object_t *)n->data;
            if (o == NULL || o->retired) {
                continue;
            }
            for (size_t i = 0; i < o->dep_count; i++) {
                if (o->deps[i] != NULL && o->deps[i]->retired) {
                    printd(DEBUG_TASK, "shared_object: retiring %s too — it is linked against %s\n",
                           o->path, o->deps[i]->path);
                    o->retired = true;
                    changed = true;
                    break;
                }
            }
        }
    }
}

// Is the file at `dep->path` still the one this registry loaded? Asked the
// way the cache-hit path asks it about the object itself — by opening the
// name and reading its identity — for the same reason: a name is not a file.
// A name that no longer resolves answers "no": the fresh load that follows
// fails on that dependency, naming it, which beats serving a program built
// against a library that has been deleted.
static bool shared_object_dep_is_current(shared_object_t *dep)
{
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(dep->path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return false;
    }
    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        return false;
    }
    bool current = (dep->ident != 0 && dep->ident == file->f_ident);
    shared_object_close_file(file, dep->path);
    return current;
}

// Numbers the closure walks (shared_object_closure_current_locked). Only ever
// advanced under the registry lock, which every walk holds.
static uint32_t kSharedObjectWalkPass;

// Is every library in `so`'s dependency closure still the file at its path?
//
// A cached executable is only as current as the libraries its cached pages
// were relocated against, so a cache hit on the executable has to ask this of
// everything beneath it. Without the question, replacing /lib/libos64.so ALONE
// leaves every resident program handing the OLD library to each new task it
// starts, for as long as anything keeps that program resident — which for
// husk is until reboot. A stale library is RETIRED here, and retirement is
// transitive, so `so` itself comes back retired and the caller falls through
// to a fresh load that rebuilds against the new file.
//
// Takes no references: the count of holders must not move for a question
// (test_dynamic_linking's expected counts depend on exactly that). Recursive
// over deps[], stamped per pass so a DT_NEEDED cycle ends the walk.
static bool shared_object_closure_current_locked(shared_object_t *so, uint32_t pass)
{
    so->walk_pass = pass;
    for (size_t i = 0; i < so->dep_count; i++) {
        shared_object_t *dep = so->deps[i];
        if (dep == NULL || dep->walk_pass == pass) {
            continue;
        }
        if (!shared_object_dep_is_current(dep)) {
            printd(DEBUG_TASK, "shared_object: %s is not the file that was loaded (needed by %s) "
                               "— retiring the loaded copy and everything linked against it\n",
                   dep->path, so->path);
            shared_object_retire_locked(dep);
            return false;
        }
        if (!shared_object_closure_current_locked(dep, pass)) {
            return false;
        }
    }
    return true;
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

    // Mount-routed like the ELF loader: a DT_NEEDED library resolves through
    // the mount table, so "/lib/…" comes from wherever "/lib" actually lives.
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return NULL;
    }

    // OPEN FIRST, EVEN ON A CACHE HIT, because the open IS the lookup. Nothing
    // else can say whether the name still means the file this registry read:
    // the string is the same after a refresh and the file is not. A name that
    // no longer resolves at all fails here rather than serving the cached image
    // of a deleted program, which is the answer Unix's exec has always given.
    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        return NULL;
    }

    shared_object_t *existing = shared_object_find(path);
    if (existing != NULL) {
        if (existing->ident != 0 && existing->ident == file->f_ident) {
            // Still the same file. Its libraries are asked the same question,
            // and only if every one of them is still current does the warm
            // cache answer — the handle opened to ask has then done its whole
            // job.
            if (shared_object_closure_current_locked(existing, ++kSharedObjectWalkPass)) {
                shared_object_close_file(file, path);
                existing->refcount++;
                return existing;
            }
            // A library beneath it changed. The walk retired that library,
            // and retirement is transitive, so `existing` is retired with it;
            // the handle stays open, because the fresh load below is its job.
        } else {
            // A different file wears this name now — or neither can be
            // identified, which is answered the same way (see shared_object_t's
            // `ident`).
            printd(DEBUG_TASK, "shared_object: %s is not the file that was loaded (disk id %lu, "
                               "loaded id %lu) — retiring the loaded copy\n",
                   path, (uint64_t)file->f_ident, existing->ident);
            shared_object_retire_locked(existing);
        }
    }

    elf_image_t *image = NULL;
    Elf64_Phdr *phdrs = NULL;
    Elf64_Half phnum = 0;
    if (elf_parse_image(file, &image, &phdrs, &phnum) != 0) {
        shared_object_close_file(file, path);
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
    so->ident = file->f_ident;        // which FILE this is, for every later load of this path
    so->retired = false;

    if (so->ident == 0) {
        // Said once, here, where it is a property of the FILESYSTEM rather
        // than of any one load: with no identity to compare, every later load
        // of this path has to assume the file changed, so this object will be
        // rebuilt on every use. No filesystem that can host a program answers
        // zero today; if this line ever appears, that stopped being true.
        printd(DEBUG_TASK, "shared_object: %s comes from a filesystem that gives its files no "
                           "identity — it will be reloaded on every use\n", path);
    }

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
        // Already placed — but by the FILE, which ring 3 can hand us. The
        // static loader refuses a PT_LOAD at or above the HHDM; this path
        // had no check at all (review 2026-08-23), so a crafted ET_EXEC with
        // a segment in the kernel half, or inside the shared window on top
        // of a live library, was accepted and mapped. An executable belongs
        // BELOW the window, full stop — the window is the libraries' and
        // everything above it is the kernel's.
        if (so->vaddr_base == 0 || so->vaddr_base + span < so->vaddr_base ||
            so->vaddr_base + span > TASK_SHLIB_VIRT_BASE) {
            printd(DEBUG_TASK, "shared_object: %s is linked at 0x%lx (+0x%lx), outside the executable "
                               "range below 0x%lx — refusing to load it\n",
                   path, so->vaddr_base, span, (uint64_t)TASK_SHLIB_VIRT_BASE);
            goto fail_placed;
        }
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
            goto fail_placed;
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
            if (other->retired)
                continue;   // a predecessor of ours, still serving the tasks
                            // that were started from it. Two builds of one
                            // library share a prelink slot BY DESIGN, and no
                            // task can map both: retirement is transitive, so
                            // nothing reachable from the old copy answers to a
                            // name any more (shared_object_retire_locked). The
                            // tripwire below is for two DIFFERENT libraries
                            // claiming one slot, which is a stale build.
            uintptr_t other_span = other->total_pages * PAGE_SIZE;
            if (so->vaddr_base < other->vaddr_base + other_span &&
                other->vaddr_base < so->vaddr_base + span) {
                printd(DEBUG_TASK, "shared_object: %s prelinked at 0x%lx (+0x%lx) OVERLAPS %s at 0x%lx (+0x%lx) "
                                   "— a stale binary from an older build?\n",
                       path, so->vaddr_base, span, other->path, other->vaddr_base, other_span);
                goto fail_placed;
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
        goto fail_placed;
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
    // 1. Drop the reference each successfully-loaded dependency took, through
    //    the same door every other release uses — a dependency this failed
    //    load was the only holder of gets unloaded here, rather than sitting
    //    resident for a program that never started. Dropping one edge can
    //    unload the object it names, never one of ITS siblings in this list:
    //    an edge we have not dropped yet is a reference that keeps its target
    //    alive.
    for (size_t i = 0; i < so->dep_count; i++) {
        shared_object_drop_ref_locked(so->deps[i]);
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
    // 4. The reason we were registered early is the reason we may not be
    //    freeable now: a DEPENDENCY CYCLE. If some library we just loaded
    //    DT_NEEDs us back, its lookup found this entry, took a reference,
    //    and stored a raw pointer in its own deps[] — and that library stays
    //    in the registry, pointing at whatever this kfree would recycle.
    //    refcount started at 1 (ours); anything above that is a holder we
    //    cannot reach from here. Leak the struct rather than dangle it
    //    (review 2026-08-23): a few hundred bytes per failed cyclic load,
    //    loudly reported, versus a use-after-free in the page fault path of
    //    an unrelated program.
    if (so->refcount > 1) {
        printd(DEBUG_TASK, "shared_object: %s failed to load but %u other object(s) already "
                           "reference it (a DT_NEEDED cycle) — keeping its struct unreachable "
                           "rather than freeing under them\n",
               path, so->refcount - 1);
        goto fail_parsed;
    }
    /* fall through */
fail_placed:
    // The struct and its page table exist but nothing else points at them
    // — the four placement refusals arrive here directly, fail_registered
    // falls in once it has left the registry. One unwind for five failures.
    kfree(so->page_phys);
    kfree(so);
    /* fall through */
fail_parsed:
    // Common failure cleanup once the ELF has been parsed. Reached directly
    // (nothing was ever registered) or fallen into from fail_registered
    // above (which has just un-registered it) — either way the file handle
    // and the parsed image plus its tables are exclusively ours to release,
    // because no other holder can reach this object any more.
    shared_object_close_file(file, path);
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
// The lock-taking wrapper around the drop; the policy and the unload live in
// shared_object_drop_ref_locked above. NOTE FOR CALLERS: `so` may be freed by
// the time this returns — it is the last reference that frees it, and only the
// caller knows whether it was holding one.
void shared_object_release(shared_object_t *so)
{
    if (so == NULL) {
        return;
    }

    registry_lock();
    shared_object_drop_ref_locked(so);
    registry_unlock();
}
