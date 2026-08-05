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
#include "vfs.h"
#include "serial_logging.h"
#include "CONFIG.h"
#include "memory/paging.h"    // kKernelPML4 (the already-in-kernel-context test)
#include "memory/kmalloc.h"   // kfree (the f_path copy owned by HANDLE_FILE)
#include "memory/vma.h"       // call_in_kernel_context
#include "thread_join.h"           // thread_join_close — HANDLE_THREAD's release
#include "driver/net/udp_conn.h"   // udp_conn_close — HANDLE_NET_UDP's release
#include "driver/net/tcp.h"        // tcp_conn_close — HANDLE_NET_TCP's release
#include "driver/net/icmp_conn.h"  // icmp_conn_close — HANDLE_NET_ICMP's release

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

// Trampoline body for handle_file_object_close: runs under kKernelPML4 on the
// core's kernel interrupt stack. The vfs_file_t is kmalloc'd (HHDM-reachable),
// so passing it straight through as the arg is fine.
static void file_close_in_kernel(void *arg)
{
	vfs_file_t *file = (vfs_file_t *)arg;
	file->fops->close(file);
}

void handle_file_object_close(void *vfs_file)
{
	vfs_file_t *file = (vfs_file_t *)vfs_file;

	if (file == NULL || file->fops == NULL || file->fops->close == NULL)
		return;

	// Drop THIS handle's reference. Spawn redirection shares one open file
	// between parent and child (see handleRefCount in vfs.h) — same rule as
	// pipe ends: only the LAST holder's close actually closes. Atomic because
	// parent and child can close concurrently on different cores.
	if (__sync_sub_and_fetch(&file->handleRefCount, 1) > 0)
		return;

	// Harvest f_path BEFORE closing — the VFS close frees the file object, and
	// for a HANDLE_FILE, f_path is always the kmalloc'd copy syscall_open made
	// (the fs stores whatever pointer open() was given, so open() must hand it
	// one with handle lifetime — and we are the end of that lifetime).
	char *path_copy = file->f_path;

	// A close can flush to disk, and disk I/O (NVMe/AHCI DMA structures) lives
	// in mappings only kKernelPML4 has — same lesson spawn learned the
	// triple-fault way. But call_in_kernel_context resets RSP to the interrupt
	// stack's TOP, so calling it while ALREADY on that stack (task-exit cleanup
	// closing a dead task's handles) would overwrite our own live frames.
	// CR3 tells us which world we're in.
	uint64_t cr3;
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	if (cr3 == (uint64_t)kKernelPML4)
		file->fops->close(file);
	else
		call_in_kernel_context(file_close_in_kernel, file);

	if (path_copy != NULL)
		kfree(path_copy);
}

// Directory sibling of the file pair above — same kernel-context discipline
// (a directory close COULD flush fs state), same f_path-copy ownership, no
// refcount (spawn rejects HANDLE_DIR, so a dir object has exactly one owner).
static void dir_close_in_kernel(void *arg)
{
	vfs_directory_t *dir = (vfs_directory_t *)arg;
	dir->dops->close(dir);
}

void handle_dir_object_close(void *vfs_dir)
{
	vfs_directory_t *dir = (vfs_directory_t *)vfs_dir;

	if (dir == NULL || dir->dops == NULL || dir->dops->close == NULL)
		return;

	// Harvest before close — the VFS close frees the directory object, and
	// f_path is the kmalloc'd copy syscall_open made (dir flavor).
	char *path_copy = dir->f_path;

	uint64_t cr3;
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	if (cr3 == (uint64_t)kKernelPML4)
		dir->dops->close(dir);
	else
		call_in_kernel_context(dir_close_in_kernel, dir);

	if (path_copy != NULL)
		kfree(path_copy);
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
		case HANDLE_FILE:
			handle_file_object_close(handle->object);
			break;
		case HANDLE_DIR:
			handle_dir_object_close(handle->object);
			break;
		case HANDLE_THREAD:
			// Detach: the thread keeps running if it is still going, and
			// nobody will ever collect its answer. The join object frees
			// itself when both references are gone.
			thread_join_close((thread_join_t *)handle->object);
			break;
		case HANDLE_NET_TCP:
			// Orderly shutdown: sends FIN and DETACHES — the closing
			// dance and TIME_WAIT finish in the background (tcp_poll),
			// so closing a handle never blocks the program.
			tcp_conn_close((tcp_conn_t *)handle->object);
			break;
		case HANDLE_NET_ICMP:
			icmp_conn_close((icmp_conn_t *)handle->object);
			break;
		case HANDLE_NET_UDP:
			// Hang up: unbinds the ephemeral port and frees the object.
			// Safe on any CR3 (everything it touches is kmalloc'd, upper
			// half) and safe from handle_close_all at task exit (the
			// owning thread has left any blocking read by then).
			udp_conn_close((udp_conn_t *)handle->object);
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
