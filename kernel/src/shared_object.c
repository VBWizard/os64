#include "shared_object.h"

#include "elf_relocate.h"
#include "memory/kmalloc.h"
#include "memory/memcpy.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "paging.h"
#include "panic.h"
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
static uintptr_t kSharedObjectNextVirt = TASK_SHLIB_VIRT_BASE;

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
static void apply_page_relocations(shared_object_t *so, const Elf64_Rela *relocs, size_t count,
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
                                  so->image->symtab, so->image->strtab,
                                  shared_object_scoped_resolver, so, &value) != 0) {
            panic("shared_object_resolve_page: failed to relocate %s at offset 0x%lx",
                  so->path, off);
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
}

// Parameters for reading a page's file content from kernel context — see
// call_in_kernel_context (memory/vma.c). Must be kmalloc'd (HHDM-accessible
// from kKernelPML4), never a stack-local: the task's own stack isn't mapped
// once CR3 has switched to kKernelPML4.
typedef struct {
    vfs_file_t *file;
    const Elf64_Phdr *phdrs;
    Elf64_Half phnum;
    size_t page_idx;
    void *dest;
    bool result;
} kernel_read_page_params_t;

static void kernel_read_page(void *arg)
{
    kernel_read_page_params_t *params = (kernel_read_page_params_t *)arg;
    params->result = elf_read_page(params->file, params->phdrs, params->phnum, params->page_idx, params->dest);
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
    void *virt = kmalloc_aligned(PAGE_SIZE);
    if (virt == NULL) {
        panic("shared_object_resolve_page: failed to allocate page %lu of %s", page_idx, so->path);
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
        panic("shared_object_resolve_page: failed to allocate read params for %s", so->path);
    }
    params->file = so->image->file;
    params->phdrs = so->phdrs;
    params->phnum = so->phnum;
    params->page_idx = page_idx;
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
        panic("shared_object_resolve_page: failed to read page %lu of %s", page_idx, so->path);
    }

    phys = (uintptr_t)virt - kHHDMOffset;

    // Relocations run lock-free: the page buffer is private to us until the
    // publishing store below, and everything they read (phdrs, symtab,
    // strtab, deps[]) is immutable after load_or_get.
    uintptr_t page_vaddr = page_idx * PAGE_SIZE;
    apply_page_relocations(so, so->image->rela, so->image->rela_count, page_vaddr, virt);
    apply_page_relocations(so, so->image->jmprel, so->image->jmprel_count, page_vaddr, virt);

    // Publish: this store is the flag every core's fast path (and same-page
    // spinners) check. The full fence guarantees the page content and
    // relocation writes above are visible before the slot stops reading as
    // RESOLVING. Only we can write this slot (we won the CAS), so no lock.
    __sync_synchronize();
    *slot = phys;

    return phys;
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
static shared_object_t *shared_object_load_or_get_locked(const char *path)
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

    if (kRootFilesystem == NULL || kRootFilesystem->fops == NULL || kRootFilesystem->fops->open == NULL) {
        return NULL;
    }

    vfs_file_t *file = NULL;
    if (kRootFilesystem->fops->open(&file, path, "r", kRootFilesystem) != 0) {
        return NULL;
    }

    elf_image_t *image = NULL;
    Elf64_Phdr *phdrs = NULL;
    Elf64_Half phnum = 0;
    if (elf_parse_image(file, &image, &phdrs, &phnum) != 0) {
        if (kRootFilesystem->fops->close != NULL) {
            kRootFilesystem->fops->close(file);
        }
        return NULL;
    }

    // Everything placed in the shared window is addressed as load_bias +
    // link-time vaddr, which only works for ET_DYN (vaddrs start near 0).
    // An ET_EXEC image — main executable OR a mislinked "library" — has
    // already-absolute vaddrs, so it can't live here; checking every image
    // (not just the main executable, as task.c used to) closes the hole
    // where a bad DT_NEEDED target would get a bias added to absolute
    // addresses and a page cache sized from a huge vaddr.
    if (image->ehdr.e_type != ET_DYN) {
        goto fail_parsed;
    }

    shared_object_t *so = kmalloc(sizeof(*so));
    if (so == NULL) {
        goto fail_parsed;
    }

    if (elf_compute_segment_ranges(phdrs, phnum, &so->total_pages, so->segs, ELF_MAX_SEGMENTS, &so->seg_count) != 0) {
        kfree(so);
        goto fail_parsed;
    }

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

    // Bump-allocate this image's fixed shared virtual base. Every task that
    // maps it uses this same address — see shared_object.h.
    size_t span = so->total_pages * PAGE_SIZE;
    if (kSharedObjectNextVirt + span >= TASK_SHLIB_VIRT_END) {
        panic("shared_object_load_or_get: exhausted shared library virtual window loading %s", path);
    }
    so->load_bias = kSharedObjectNextVirt;
    kSharedObjectNextVirt += span;

    // Register BEFORE loading dependencies: if some dependency (directly or
    // transitively) DT_NEEDs us back, its lookup finds this entry and the
    // recursion terminates instead of looping forever. Our path/load_bias/
    // symtab are already valid at this point, which is all a dependent
    // needs from us.
    so->registry_node = dlist_add(kLoadedSharedObjects, so);

    // Load this object's own DT_NEEDED dependencies, recursively — they are
    // its symbol resolution scope (see shared_object_scoped_resolver) and
    // every task that maps this object maps them too (see task.c). A
    // missing dependency is unrecoverable and would otherwise surface much
    // later as an undecipherable relocation failure at first fault, so fail
    // loudly here, at load time, with both names.
    for (size_t i = 0; i < image->needed_count; i++) {
        char lib_path[TASK_MAX_PATH_LEN];
        snprintf(lib_path, sizeof(lib_path), "/lib/%s", image->needed[i]);

        shared_object_t *dep = shared_object_load_or_get_locked(lib_path);
        if (dep == NULL) {
            panic("shared_object_load_or_get: failed to load dependency %s needed by %s", lib_path, path);
        }
        so->deps[so->dep_count++] = dep;
    }

    return so;

fail_parsed:
    // Common failure cleanup once the ELF has been parsed: nothing has been
    // registered yet, so the file handle and the parsed image (plus its
    // tables) are still exclusively ours to release.
    if (kRootFilesystem->fops->close != NULL) {
        kRootFilesystem->fops->close(file);
    }
    elf_image_free(image);
    kfree(phdrs);
    return NULL;
}

shared_object_t *shared_object_load_or_get(const char *path)
{
    registry_lock();
    shared_object_t *so = shared_object_load_or_get_locked(path);
    registry_unlock();
    return so;
}
