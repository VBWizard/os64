#ifndef MEMORY_VMA_H
#define MEMORY_VMA_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/maplist.h"
#include "counted_lock.h"
#include "task.h"

#ifndef PROT_READ
#define PROT_READ  0x1
#endif

#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif

#ifndef PROT_EXEC
#define PROT_EXEC  0x4
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x01
#endif

#ifndef MAP_SHARED
#define MAP_SHARED  0x02
#endif

// Marks a VMA as backed by a shared_object_t (a dynamically-linked library
// or executable's demand-paged segment) rather than a plain vfs_file_t.
// When set, `vma->file` is actually a `shared_object_t*` — see
// shared_object.h and the page-fault handler in simple_exceptions.c, which
// checks this flag to route resolution through the per-library page cache
// (shared_object_resolve_page) instead of the ordinary per-VMA file read.
#ifndef MAP_SHARED_LIBRARY
#define MAP_SHARED_LIBRARY 0x04
#endif

//VMA tracking structure
typedef struct vma {
    uintptr_t start;              // Inclusive start address
    uintptr_t end;                // Exclusive end address
    int prot;                     // PROT_READ, PROT_WRITE, etc.
    int flags;                    // MAP_PRIVATE, MAP_SHARED, etc.
    // Optional backing — AND IT IS TWO DIFFERENT TYPES DEPENDING ON `flags`.
    // READ THIS BEFORE DEREFERENCING IT:
    //   MAP_SHARED_LIBRARY set  -> shared_object_t*  (shared_object.h)
    //   otherwise               -> vfs_file_t*       (vfs.h)
    // A VMA is only ever one or the other, which is why this is one field
    // instead of two. The cost of that economy is that every reader has to
    // check the flag, and on 2026-08-22 one didn't: /proc/<pid>/maps read a
    // library VMA as a vfs_file_t and #GP'd in the kernel the first time
    // anything on the system actually had a shared library mapped. See
    // procfs.c's proc_map_file_path for the (genuinely funny) details.
    void* file;
    uint64_t file_offset;         // File offset for mmap
    // Number of bytes from `start` that are backed by real file content.
    // For a fully file-backed VMA this equals (end - start).  For an ELF
    // segment whose file data ends partway through its final page (p_filesz <
    // p_memsz), it is set smaller than the VMA span so the page-fault path
    // reads only the genuine file bytes and zero-fills the BSS tail of that
    // page instead of reading past p_filesz into unrelated file bytes.
    uint64_t file_size;
    bool cow;                     // Is this region CoW-enabled?
    // Its place on the task's list and its holds (memory/maplist.h): the
    // list's links live in the mapping, so adding and delisting allocate and
    // free nothing under the list's lock. Every field above is fixed once the
    // mapping is listed; the node changes only under the task's mapLock.
    maplist_node_t node;
} vma_t;

// What a caller that needs only facts gets: a copy made under the lock, true
// when it was made and not after.
typedef struct {
    uintptr_t start, end;
    int prot, flags;
    bool cow;
    void *file;
    uint64_t file_offset;
} vma_facts_t;

vma_t* vma_create(uintptr_t start,
                  uintptr_t end,
                  int prot,
                  int flags,
                  void* file,
                  uint64_t file_offset);

// ONCE ANOTHER THREAD CAN REACH IT, A TASK'S LIST OF MAPPINGS IS READ AND
// WRITTEN THROUGH THESE, each under the task's mapLock (task.h). That lock is a leaf, irqsave, held for
// a walk: nothing is allocated, freed, formatted or slept on under it, no
// lock is taken under it, and nothing under it touches user memory through
// a user address (a fault on one would come back here and wait on the lock
// its own thread holds).

// `vma` listed on the task's list, at its head.
void vma_add(task_t* task, vma_t* vma);
// The listed mapping holding `addr`, with a hold its caller gives back with
// vma_release; NULL for none.
vma_t* vma_lookup_hold(task_t* task, uintptr_t addr);
// One hold given back. The last hold of a delisted mapping frees it.
void vma_release(task_t* task, vma_t* vma);
// The facts of the listed mapping holding `addr`, copied under the lock;
// false for none.
bool vma_lookup_facts(task_t* task, uintptr_t addr, vma_facts_t *out);
// The listed ANONYMOUS mapping starting exactly at `start`, with a hold;
// NULL for none (unmap's lookup: a region map() made, never the loader's).
vma_t* vma_lookup_anon_start_hold(task_t* task, uintptr_t start);
// `vma` delisted: no lookup finds it from now on. True when this call
// delisted it, false when another already had.
bool vma_delist(task_t* task, vma_t* vma);
// Every mapping delisted at once, for burial, as a chain newest first
// (vma_chain_next walks it); `*held` counts the holds found outstanding.
vma_t* vma_take_all(task_t* task, uint64_t *held);
vma_t* vma_chain_next(vma_t* vma);
// The list's counts for /proc/<id>/status, copied under the lock: how many
// mappings are listed, the walks and the lock's waits and holds, each since
// the task began and since the last read, which starts the windows again.
typedef struct {
    uint32_t listed;
    maplist_walks_t walks, walks_since;
    counted_lock_stats_t lock;
} vma_counts_t;
void vma_read_counts(task_t* task, vma_counts_t *out);

// Up to `cap` mappings' facts copied under the lock, newest first; returns
// how many were copied, and `*listed` (when not NULL) how many are listed.
uint32_t vma_copy_all_facts(task_t* task, vma_facts_t *out, uint32_t cap, uint32_t *listed);

uintptr_t vma_resolve_backing_page(vma_t *vma, uintptr_t fault_addr, bool *did_read);

void vma_destroy(vma_t* vma);

// Runs `func(arg)` with kKernelPML4 and the core's kernel interrupt stack
// loaded instead of whatever task context called this — needed whenever
// code invoked from inside a page-fault handler must touch kernel-only
// resources (e.g. a VFS/disk driver's own buffers or MMIO, which aren't
// necessarily mapped into an arbitrary task's own page tables). `arg` must
// point to kmalloc'd (HHDM-accessible) memory, never a task-stack address —
// see vma.c's kernel_read_file for the reference use of this pattern.
void call_in_kernel_context(void (*func)(void*), void *arg);

#endif // MEMORY_VMA_H
