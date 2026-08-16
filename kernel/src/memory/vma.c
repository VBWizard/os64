#include "memory/vma.h"
#include "memory/mmap.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "allocator.h"
#include "task.h"
#include "paging.h"
#include "panic.h"
#include "filesystem/filesystem.h"
#include "smp_core.h"

extern uintptr_t kKernelPML4;

static inline bool vma_contains(const vma_t* vma, uintptr_t addr)
{
    return vma != NULL && addr >= vma->start && addr < vma->end;
}

// Structure to pass parameters across the CR3 boundary
typedef struct {
    vfs_file_t *file;
    vfs_file_operations_t *fops;
    uint64_t file_offset;
    void *buffer;
    size_t size;
    int result;
} kernel_read_params_t;

// This function runs in kernel context (kKernelPML4 loaded)
static void kernel_read_file(kernel_read_params_t *params)
{
    // SEEK AND READ ARE ONE OPERATION, and this is the only place in the
    // demand pager that says so.
    //
    // A vfs_file_t owns a single position. Two threads of one task faulting on
    // two different pages of their own executable both land here on different
    // cores, and without this lock they interleave as
    //   T1 seek(0x3000) | T2 seek(0x9000) | T1 read -> gets 0x9000's bytes
    // so a code page is filled with real, valid machine code from the WRONG
    // part of the binary. Execution then wanders off and dies somewhere with
    // no relationship to the bug — the fault we chased on 2026-08-15 reported
    // a read of 0xffffffffffffff8a from an instruction that performs no read
    // at all, which is exactly what "the bytes under RIP are not the code you
    // think" looks like from the other end.
    //
    // Found by malloctest's threaded heap test: the FIRST workload in os64's
    // life where several threads of one task executed enough distinct code to
    // fault pages in simultaneously (threadtest's workers share one tiny
    // loop). Proof it is the pager and not the heap: pre-faulting the text on
    // the main thread makes 8 threads run clean, cold text faults 7 times.
    //
    // The dynamic path already knew: shared_object_t::io_lock guards the same
    // pair for shared objects, with the same one-line comment. This is that
    // lock for everybody else.
    //
    // Spin, do not sleep: os64's storage I/O is polled, ext2 already holds
    // irqsave spinlocks across whole operations, and this pair is short.
    while (__sync_lock_test_and_set(&params->file->pos_lock, 1))
        __asm__ volatile("pause");

    if (params->fops->seek(params->file, (long)params->file_offset, SEEK_SET) < 0)
        params->result = -1;
    else
        params->result = params->fops->read(params->file, params->buffer, params->size);

    __sync_lock_release(&params->file->pos_lock);
}

// call_in_kernel_context() is a NAKED asm trampoline in task_exit_asm.S.
//
// The former C version here switched RSP/CR3 in a normal C frame and then
// accessed locals (the reloaded `cls`, etc.) through an rbp still pointing at
// the task's own stack — which is unmapped once kKernelPML4 is loaded. That was
// UB, masked only by the -O0 stack layout. The asm version keeps task CR3/RSP
// and func/arg in callee-saved registers across the switch so no C local is ever
// touched in the wrong address space. See the CLAUDE.md "Context Switching
// Between Task and Kernel Space" section (Case 2 — the must-return case).

// Resolves and returns the physical address of a page for the given faulting address.
// This function handles anonymous and file-backed memory only (CoW later).
uintptr_t vma_resolve_backing_page(vma_t *vma, uintptr_t fault_addr)
{
    uintptr_t page_offset = (fault_addr & ~(PAGE_SIZE - 1)) - vma->start;
    uintptr_t phys = 0;

    if ((vma->flags & MAP_ANONYMOUS) || vma->file == NULL)
    {
        // Use physical allocator directly - kmalloc might map into kKernelPML4 only.
        // The page comes back already zeroed: allocate_memory_aligned() zeroes at
        // the allocator choke point, which is exactly the zero-initialization an
        // anonymous/BSS/heap mapping requires.
        phys = allocate_memory_aligned(PAGE_SIZE);
    }
    else
    {
        vfs_file_t *file = (vfs_file_t *)vma->file;
        vfs_file_operations_t *fops = NULL;
        uint64_t file_offset = vma->file_offset + page_offset;

        if (file == NULL)
            panic("vma_resolve_backing_page: Null file backing");

        if (file->fops != NULL)
        {
            fops = file->fops;
        }
        else if (file->owner != NULL)
        {
            fops = ((vfs_filesystem_t *)file->owner)->fops;
        }

        if (fops == NULL || fops->read == NULL || fops->seek == NULL)
            panic("vma_resolve_backing_page: File ops not available for backing");

        // Allocate both the page buffer and the params struct using kmalloc so
        // they live in the HHDM (upper half) and remain accessible after the
        // CR3 switch to kKernelPML4 inside call_in_kernel_context.
        // Stack-local params would be on the task stack, which is NOT mapped
        // in kKernelPML4, causing kernel_read_file to dereference garbage.
        void *virt = kmalloc_aligned(PAGE_SIZE);
        if (!virt)
            panic("Failed to allocate page for file-backed VMA");

        // Only the bytes up to vma->file_size (measured from vma->start) are real
        // file content; anything past that within this page is BSS and must read
        // back as zero.  This is what keeps an ELF segment whose p_filesz ends
        // partway through a page from pulling unrelated file bytes into the tail.
        size_t page_valid;
        if (page_offset >= vma->file_size)
            page_valid = 0;
        else {
            page_valid = (size_t)(vma->file_size - page_offset);
            if (page_valid > PAGE_SIZE)
                page_valid = PAGE_SIZE;
        }

        // Whenever the page is not fully file-backed, zero it first so the
        // [page_valid, PAGE_SIZE) tail is guaranteed zero regardless of what the
        // allocator handed us.
        if (page_valid < PAGE_SIZE)
            memset(virt, 0, PAGE_SIZE);

        if (page_valid > 0)
        {
            kernel_read_params_t *params = kmalloc(sizeof(kernel_read_params_t));
            if (!params)
                panic("Failed to allocate kernel_read_params_t");

            params->file = file;
            params->fops = fops;
            params->file_offset = file_offset;
            params->buffer = virt;
            params->size = page_valid;
            params->result = 0;

            // Switch to kernel context (kernel stack + kKernelPML4) and read file
            call_in_kernel_context((void (*)(void*))kernel_read_file, params);

            int read_result = params->result;
            kfree(params);

            if (read_result < 0)
                panic("vma_resolve_backing_page: Failed to read file backing");
        }

        // Get physical address from virtual (kmalloc returns HHDM addresses)
        phys = (uintptr_t)virt - kHHDMOffset;
    }
    return phys;
}

vma_t* vma_create(uintptr_t start,
                  uintptr_t end,
                  int prot,
                  int flags,
                  void* file,
                  uint64_t file_offset)
{
    vma_t* vma = kmalloc(sizeof(vma_t));
    if (!vma) {
        return NULL;
    }

    vma->start = start;
    vma->end = end;
    vma->prot = prot;
    vma->flags = flags;
    vma->file = file;
    vma->file_offset = file_offset;
    // Default: the entire VMA is file-backed.  Callers that map an ELF segment
    // with a partial-page BSS tail lower this to the true file extent so the
    // fault path zero-fills the remainder (see elf_map_segment).
    vma->file_size = end - start;
    vma->cow = false;
    vma->listItem = NULL;

    return vma;
}

/// @brief Add a VMA to a task's memory map list
/// @param task A task_t struct pointer
/// @param vma A vma_t struct pointer
void vma_add(task_t* task, vma_t* vma)
{
    if (!task || !vma) {
        return;
    }

    if (task->mmaps == NULL) {
        task->mmaps = kmalloc(sizeof(dlist_t));
        if (!task->mmaps) {
            return;
        }
        dlist_init(task->mmaps);
    }

    vma->listItem = dlist_add(task->mmaps, vma);
}

/// @brief Find a specific memory map for a given task based on address
/// @param task A task_t struct pointer
/// @param addr An address within the memory map to find
/// @return 
vma_t* vma_lookup(task_t* task, uintptr_t addr)
{
    if (!task || !task->mmaps) {
        return NULL;
    }

    dlist_node_t* node = task->mmaps->head;
    while (node) {
        vma_t* current = (vma_t*)node->data;
        if (vma_contains(current, addr)) {
            return current;
        }
        node = node->next;
    }

    return NULL;
}

/// @brief Free a vma_t
/// @param vma A vma_t struct pointer
void vma_destroy(vma_t* vma)
{
    if (!vma) {
        return;
    }

    kfree(vma);
}
