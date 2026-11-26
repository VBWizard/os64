#include "task.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "memory/vma.h"
#include "memory/mmap.h"

/* A task's list of mappings through vma.h's doors, on a task of the test's
 * own (MAPPING_LIST.md): a lookup holds, a delisted mapping lives while held
 * and is found by nothing, the last release or an unheld delist frees it,
 * unmap's lookup takes only anonymous mappings, a second delist says so,
 * facts are copies, burial takes everything and counts the holds left, and
 * the counts read since the last read. The rules beside a model, and their
 * mutants, are tools/test_maplist_host.sh's; threads racing on the list are
 * /tests/maprace's. */
bool test_vma_list_doors(void)
{
    task_t *t = kmalloc(sizeof(*t));
    if (t == NULL)
        return false;
    maplist_init(&t->mmaps);
    t->mapLock = (counted_lock_t){0};

    vma_t *anon = vma_create(0x70000000, 0x70004000, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
    vma_t *file = vma_create(0x60000000, 0x60002000, PROT_READ, MAP_PRIVATE, NULL, 0);
    if (anon == NULL || file == NULL)
        return false;
    vma_add(t, file);
    vma_add(t, anon);
    bool ok = t->mmaps.count == 2 && t->mmaps.head == &anon->node;

    /* A lookup holds; a lookup of nothing holds nothing. */
    vma_t *held = vma_lookup_hold(t, 0x70001000);
    ok &= held == anon && anon->node.holds == 1;
    ok &= vma_lookup_hold(t, 0x70004000) == NULL;

    /* Facts are a copy, made without a hold. */
    vma_facts_t facts;
    ok &= vma_lookup_facts(t, 0x60001fff, &facts) && facts.start == 0x60000000 &&
          facts.prot == PROT_READ && file->node.holds == 0;
    ok &= !vma_lookup_facts(t, 0x60002000, &facts);

    /* unmap's lookup: anonymous mappings by exact start, never the others. */
    ok &= vma_lookup_anon_start_hold(t, 0x60000000) == NULL && file->node.holds == 0;
    ok &= vma_lookup_anon_start_hold(t, 0x70001000) == NULL;
    vma_t *unmapping = vma_lookup_anon_start_hold(t, 0x70000000);
    ok &= unmapping == anon && anon->node.holds == 2;

    /* Delisted while held: found by nothing, alive for its holders, and a
     * second delist says it was not the one. */
    ok &= vma_delist(t, anon);
    ok &= !anon->node.listed && t->mmaps.count == 1 && vma_lookup_hold(t, 0x70001000) == NULL;
    ok &= !vma_delist(t, anon) && anon->start == 0x70000000;
    vma_release(t, unmapping);
    ok &= anon->node.holds == 1 && anon->end == 0x70004000;
    vma_release(t, held);   /* the last hold of a delisted mapping: freed */

    /* The counts: the walks and the lock since the task began and since the
     * last read, which starts the windows again. */
    vma_counts_t c;
    vma_read_counts(t, &c);
    ok &= c.listed == 1 && c.walks.lookups >= 7 && c.walks_since.lookups == c.walks.lookups &&
          c.lock.acquires > 0 && c.lock.window_acquires == c.lock.acquires;
    (void)vma_lookup_hold(t, 0x90000000);
    vma_read_counts(t, &c);
    ok &= c.walks_since.lookups == 1 && c.walks_since.steps == 1 && c.lock.window_acquires == 2;

    /* Burial takes every mapping, held or not, and counts the holds left. */
    vma_t *more = vma_create(0x70010000, 0x70011000, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
    if (more == NULL)
        return false;
    vma_add(t, more);
    (void)vma_lookup_hold(t, 0x70010000);
    uint64_t left = 0;
    uint32_t taken = 0;
    for (vma_t *v = vma_take_all(t, &left); v != NULL;) {
        vma_t *next = vma_chain_next(v);
        ok &= !v->node.listed;
        vma_destroy(v);
        taken++;
        v = next;
    }
    ok &= taken == 2 && left == 1 && t->mmaps.head == NULL && t->mmaps.count == 0;

    kfree(t);
    return ok;
}
