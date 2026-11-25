#include "task.h"
#include "handle.h"
#include "memory/kmalloc.h"

/* The handle table grows in blocks and nothing in it moves (handle_blocks.h,
 * handle.c § The blocks). Each case works on a task of its own that no
 * scheduler sees; console tags hold no object, so a thousand of them cost
 * nothing to make or to close. */

static task_t *table_task(void)
{
    task_t *t = kmalloc(sizeof(*t));
    handle_table_init(t);
    return t;
}

static void table_task_done(task_t *t)
{
    handle_close_all(t);
    handle_table_free(t);
    kfree(t);
}

/* A thousand handles: the numbers come in order across every block, an
 * entry taken early is the same entry after more blocks arrive, a closed
 * number is the next one handed out, a share reaches a handle in an added
 * block, and burial closes every block's. */
bool test_handle_table_grows(void)
{
    task_t *t = table_task();
    bool ok = handle_table_room(t) == 64 && t->handleCounts.held == 3;
    for (int i = 0; i < 1000; i++)
        ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 3 + i;
    ok &= handle_table_room(t) == 1024 && t->handleCounts.held == 1003 && t->handleCounts.most == 1003;
    handle_t *early = handle_entry(t, 40), *mid = handle_entry(t, 700);
    for (int i = 0; i < 100; i++)
        ok &= handle_alloc(t, HANDLE_CONSOLE_ERR, NULL) == 1003 + i;
    ok &= handle_table_room(t) == 2048 && handle_entry(t, 40) == early && handle_entry(t, 700) == mid;
    ok &= mid->type == HANDLE_CONSOLE_OUT && handle_entry(t, 1050)->type == HANDLE_CONSOLE_ERR;
    ok &= handle_entry(t, -1) == NULL && handle_entry(t, 2048) == NULL && handle_entry(t, 4096) == NULL;
    /* Closed in a mixed order, the lowest closed comes back first. */
    ok &= handle_close(t, 900) && handle_close(t, 70) && handle_close(t, 512);
    ok &= t->handleCounts.held == 1100 && t->handleCounts.most == 1103;
    ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 70 && handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 512 &&
          handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 900 && handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 1103;
    /* A reservation in an added block commits and cancels like one in block 0. */
    int r = handle_reserve(t);
    ok &= r == 1104 && handle_cancel_reserved(t, r) && handle_reserve(t) == 1104;
    ok &= handle_commit_reserved(t, 1104, HANDLE_CONSOLE_IN, NULL) && handle_entry(t, 1104)->type == HANDLE_CONSOLE_IN;
    handle_t shared;
    ok &= handle_share(t, 700, &shared) && shared.type == HANDLE_CONSOLE_OUT;
    handle_close_all(t);
    ok &= t->handleCounts.held == 0 && handle_entry(t, 1050)->type == HANDLE_NONE &&
          handle_entry(t, 40)->type == HANDLE_NONE;
    handle_table_free(t);
    kfree(t);
    return ok;
}

/* At the ceiling every block is there, and an open or a reservation is
 * refused and counted. */
bool test_handle_table_ceiling(void)
{
    task_t *t = table_task();
    bool ok = true;
    for (int i = 3; i < (int)TASK_HANDLES_CEILING; i++)
        ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == i;
    ok &= handle_table_room(t) == TASK_HANDLES_CEILING && t->handleCounts.held == TASK_HANDLES_CEILING;
    ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == -1 && handle_reserve(t) == -1;
    ok &= t->handleCounts.failed == 2 && t->handleCounts.held == TASK_HANDLES_CEILING;
    ok &= handle_close(t, 4000) && handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 4000;
    table_task_done(t);
    return ok;
}

/* Once the task is tearing down no block is added: the open that would
 * need one is refused and counted, and block 1 never arrives. */
bool test_handle_table_no_grow_in_teardown(void)
{
    task_t *t = table_task();
    bool ok = true;
    for (int i = 3; i < 64; i++)
        ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == i;
    t->tearingDown = true;
    ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == -1 && t->handleBlocks[0] == NULL &&
          t->handleCounts.failed == 1 && handle_table_room(t) == 64;
    t->tearingDown = false;
    ok &= handle_alloc(t, HANDLE_CONSOLE_OUT, NULL) == 64 && handle_table_room(t) == 128;
    table_task_done(t);
    return ok;
}
