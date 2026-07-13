// handle.c — the per-task handle table. See handle.h for the contract.
//
// Deliberately dumb: a fixed array, no locking. A task's handle table is
// touched by that task's own syscalls, plus by spawn while the child is still
// being BUILT (before it is ever submitted to the scheduler, so nothing else
// can see it). No concurrent access, no lock. If handles ever become shareable
// between threads of one task, this grows a lock — and that will be an obvious
// change, not a subtle one.

#include "handle.h"
#include "task.h"
#include "pipe.h"
#include "serial_logging.h"
#include "CONFIG.h"

void handle_table_init(struct task *t)
{
	task_t *task = (task_t *)t;

	for (int i = 0; i < TASK_MAX_HANDLES; i++)
	{
		task->handles[i].type = HANDLE_NONE;
		task->handles[i].object = NULL;
	}

	// Every task is born wired to the console. The shell overwrites 0 and/or 1
	// with pipe ends when it builds a pipeline; a task that isn't in a pipeline
	// just talks to the terminal, exactly as before handles existed.
	task->handles[0].type = HANDLE_CONSOLE_IN;
	task->handles[1].type = HANDLE_CONSOLE_OUT;
	task->handles[2].type = HANDLE_CONSOLE_ERR;
}

int handle_alloc(struct task *t, handle_type_t type, void *object)
{
	task_t *task = (task_t *)t;

	// Start at 3: 0/1/2 are the standard streams and are never handed out as
	// fresh handles (a pipe landing in slot 1 by accident would silently
	// redirect the task's own stdout — a fun afternoon of debugging).
	for (int i = 3; i < TASK_MAX_HANDLES; i++)
	{
		if (task->handles[i].type == HANDLE_NONE)
		{
			task->handles[i].type = type;
			task->handles[i].object = object;
			return i;
		}
	}

	printd(DEBUG_TASK, "handle_alloc: task %s is out of handles (max %u)\n",
		task->exename, TASK_MAX_HANDLES);
	return -1;
}

bool handle_install(struct task *t, int slot, handle_type_t type, void *object)
{
	task_t *task = (task_t *)t;

	if (slot < 0 || slot >= TASK_MAX_HANDLES)
		return false;

	// Replacing a live handle means giving up whatever was there.
	if (task->handles[slot].type != HANDLE_NONE)
		handle_close(t, slot);

	task->handles[slot].type = type;
	task->handles[slot].object = object;
	return true;
}

handle_t *handle_get(struct task *t, int h)
{
	task_t *task = (task_t *)t;

	// This IS the range check: ring 3 passes whatever integer it feels like.
	if (h < 0 || h >= TASK_MAX_HANDLES)
		return NULL;
	if (task->handles[h].type == HANDLE_NONE)
		return NULL;

	return &task->handles[h];
}

bool handle_close(struct task *t, int h)
{
	task_t *task = (task_t *)t;
	handle_t *handle = handle_get(t, h);

	if (handle == NULL)
		return false;

	// Dropping the task's reference on the object. For a pipe this is THE
	// refcount that decides EOF (last writer gone) and EPIPE (last reader
	// gone) — closing a handle is how a program says "I am done with this end".
	switch (handle->type)
	{
		case HANDLE_PIPE_READ:
			pipe_close_read_end((pipe_t *)handle->object);
			break;
		case HANDLE_PIPE_WRITE:
			pipe_close_write_end((pipe_t *)handle->object);
			break;
		default:
			// Console handles reference no object — nothing to release.
			break;
	}

	task->handles[h].type = HANDLE_NONE;
	task->handles[h].object = NULL;
	return true;
}

void handle_close_all(struct task *t)
{
	task_t *task = (task_t *)t;

	for (int i = 0; i < TASK_MAX_HANDLES; i++)
		if (task->handles[i].type != HANDLE_NONE)
			handle_close(t, i);
}
