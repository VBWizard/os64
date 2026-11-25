#include "task.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"

/* The task's one door for its heap range (task_va_reserve): ranges come in
 * order with a guard page between, a refusal claims nothing, and nothing is
 * handed out twice. Threads racing for it are /tests/maprace's. */
bool test_task_va_reserve(void)
{
    task_t *t = kmalloc(sizeof(*t));
    t->heapEnd = TASK_HEAP_START;
    uintptr_t a = 0, b = 0, c = 0;
    bool ok = task_va_reserve(t, PAGE_SIZE, &a) && a == TASK_HEAP_START;
    ok &= task_va_reserve(t, 3 * PAGE_SIZE, &b) && b == a + 2 * PAGE_SIZE;
    ok &= t->heapEnd == b + 4 * PAGE_SIZE;
    /* A size that is zero or not whole pages claims nothing. */
    uint64_t before = t->heapEnd;
    ok &= !task_va_reserve(t, 0, &c) && !task_va_reserve(t, PAGE_SIZE + 1, &c) && t->heapEnd == before;
    /* Nothing reaches TASK_HEAP_END, and a size that would wrap is refused. */
    t->heapEnd = (TASK_HEAP_END & ~(uint64_t)(PAGE_SIZE - 1)) - 2 * PAGE_SIZE;
    before = t->heapEnd;
    ok &= !task_va_reserve(t, 2 * PAGE_SIZE, &c) && t->heapEnd == before;
    ok &= task_va_reserve(t, PAGE_SIZE, &c) && c == before && t->heapEnd == before + 2 * PAGE_SIZE;
    ok &= !task_va_reserve(t, UINT64_MAX & ~(uint64_t)(PAGE_SIZE - 1), &c);
    kfree(t);
    return ok;
}
