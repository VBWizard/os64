#include "memory/vma.h"
#include "memory/mmap.h"
#include "memory/kmalloc.h"
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
    if (params->fops->seek(params->file, (long)params->file_offset, SEEK_SET) < 0) {
        params->result = -1;
        return;
    }

    params->result = params->fops->read(params->file, params->buffer, params->size);
}

// Trampoline: switches to kernel stack and CR3, calls function, switches back
static void call_in_kernel_context(void (*func)(void*), void *arg)
{
    core_local_storage_t *cls = get_core_local_storage();

    // Save parameters before stack switch (use CLS for SMP safety)
    cls->cikc_saved_func = func;
    cls->cikc_saved_arg = arg;

    // Save task context
    __asm__ volatile("mov %0, cr3" : "=r"(cls->cikc_saved_cr3));
    __asm__ volatile("mov %0, rsp" : "=r"(cls->cikc_saved_rsp));

    // Save kernel stack pointer in local var (on current stack, before switch)
    uintptr_t kernel_rsp = cls->kernel_interrupt_stack_top - 16;

    // Switch to kernel stack
    __asm__ volatile("mov rsp, %0" : : "r"(kernel_rsp));

    // Switch to kKernelPML4
    __asm__ volatile("mov cr3, %0" : : "r"((uint64_t)kKernelPML4) : "memory");

    // CRITICAL: Reload CLS pointer after stack/CR3 switch!
    // The previous 'cls' variable is on the old stack and no longer accessible.
    // get_core_local_storage() reads from GS:0, which is always valid.
    cls = get_core_local_storage();

    // Call function in kernel context (using saved parameters from reloaded cls)
    cls->cikc_saved_func(cls->cikc_saved_arg);

    // Restore task CR3
    __asm__ volatile("mov cr3, %0" : : "r"(cls->cikc_saved_cr3) : "memory");

    // Restore task stack
    __asm__ volatile("mov rsp, %0" : : "r"(cls->cikc_saved_rsp));
}

// Resolves and returns the physical address of a page for the given faulting address.
// This function handles anonymous and file-backed memory only (CoW later).
uintptr_t vma_resolve_backing_page(vma_t *vma, uintptr_t fault_addr)
{
    uintptr_t page_offset = (fault_addr & ~(PAGE_SIZE - 1)) - vma->start;
    uintptr_t phys = 0;

    if ((vma->flags & MAP_ANONYMOUS) || vma->file == NULL)
    {
        // Use physical allocator directly - kmalloc might map into kKernelPML4 only
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

        // Allocate buffer using kmalloc (mapped in kKernelPML4)
        void *virt = kmalloc_aligned(PAGE_SIZE);
        if (!virt)
            panic("Failed to allocate page for file-backed VMA");

        // Static params in kernel .bss (upper-half, accessible from both contexts)
        static kernel_read_params_t params;
        params.file = file;
        params.fops = fops;
        params.file_offset = file_offset;
        params.buffer = virt;
        params.size = PAGE_SIZE;
        params.result = 0;

        // Switch to kernel context (kernel stack + kKernelPML4) and read file
        call_in_kernel_context((void (*)(void*))kernel_read_file, &params);

        if (params.result < 0)
            panic("vma_resolve_backing_page: Failed to read file backing");

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
    vma->cow = false;
    vma->loaded = false;
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
