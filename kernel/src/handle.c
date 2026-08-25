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
#include "tty.h"              // pty_master_close (HANDLE_PTY_MASTER's hangup)
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
	if (task->handles[h].type == HANDLE_NONE || task->handles[h].type == HANDLE_CLOSING)
		return NULL;   // free, or mid-close — not operable either way

	return &task->handles[h];
}

// Trampoline body for handle_file_object_close: runs under kKernelPML4 on the
// core's kernel interrupt stack. The vfs_file_t is kmalloc'd (HHDM-reachable),
// so passing it straight through as the arg is fine.
// The filesystem's answer has to come BACK (Codex #29 rd14). This used to be
// `file->fops->close(file);` with the result dropped on the floor — and the
// whole chain below it was built to carry that result: fops->close returns
// int, and fat_close ALREADY computes it (f_close != FR_OK -> -1). One
// discarded return value was the entire defect.
//
// A params block rather than the bare pointer, because the trampoline hands
// back nothing: kmalloc'd, per the house rule that anything
// call_in_kernel_context touches must be HHDM-reachable and never on the
// caller's task-local stack.
typedef struct {
	vfs_file_t *file;
	volatile int result;
} file_close_params_t;

static void file_close_in_kernel(void *arg)
{
	file_close_params_t *p = (file_close_params_t *)arg;
	p->result = p->file->fops->close(p->file);
}

int handle_file_object_close(void *vfs_file)
{
	vfs_file_t *file = (vfs_file_t *)vfs_file;

	if (file == NULL || file->fops == NULL || file->fops->close == NULL)
		return 0;   // nothing to close is not a failure to close

	// Drop THIS handle's reference. Spawn redirection shares one open file
	// between parent and child (see handleRefCount in vfs.h) — same rule as
	// pipe ends: only the LAST holder's close actually closes. Atomic because
	// parent and child can close concurrently on different cores.
	if (__sync_sub_and_fetch(&file->handleRefCount, 1) > 0)
		return 0;   // somebody else still holds it; nothing was flushed here

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
	// Harvest the NAME too, for the tripwire below — after the close it is
	// freed, and "a file failed to flush" without saying which file is a
	// diagnostic nobody can act on.
	uint64_t cr3;
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	int rc;
	if (cr3 == (uint64_t)kKernelPML4)
	{
		rc = file->fops->close(file);
	}
	else
	{
		file_close_params_t *p = kmalloc(sizeof(*p));
		if (p == NULL)
			return -1;      // cannot even ask; do not claim it flushed
		p->file = file;
		p->result = -1;
		call_in_kernel_context(file_close_in_kernel, p);
		rc = (int)p->result;
		kfree(p);
	}

	// LOUD, because until rd14 this was silent and silence is the actual bug.
	// A close is where FAT commits — FatFs flushes data and metadata inside
	// f_close — so a failure here means bytes a program was told it had
	// written are NOT on the disk. Every caller learns something from the log
	// even when it has nowhere to return an error to, and the burial closer in
	// task.c is exactly such a caller: a dying task's last write failing is
	// worth knowing about and there is no one left to tell but the log.
	//
	// (ext2 makes this a formality — writes are full write-through, so there
	// is nothing left to fail at close. FAT is why the line exists, and the
	// lifeboat is FAT.)
	// DEBUG_EXCEPTIONS, and the choice is load-bearing rather than lazy:
	// printd requires ALL the bits it is given ((kDebugLevel & level) !=
	// level), and DEBUG_VFS is not in DEBUG_MINIMAL_OPTIONS — so tagging this
	// "DEBUG_VFS | something" would make the tripwire invisible on a default
	// boot, which is a check that cannot fire dressed up as one that can.
	// DEBUG_EXCEPTIONS is always on and means exactly what this is: something
	// went wrong that nobody asked to hear about.
	if (rc != 0)
		printd(DEBUG_EXCEPTIONS,
		       "handle_file_object_close: FLUSH FAILED (%d) closing '%s' — data written to this file may not be on disk\n",
		       rc, path_copy ? path_copy : "<unnamed>");

	if (path_copy != NULL)
		kfree(path_copy);
	return rc;
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
	// Everything below works through `handle` (== &task->handles[h]); the slot
	// index is closed over by that pointer, so the task_t is not needed here.
	handle_t *handle = handle_get(t, h);

	if (handle == NULL)
		return false;

	// ── CLAIM THE SLOT, THEN CLOSE (2026-08-23) ─────────────────────────────
	//
	// The slot used to be cleared AFTER the switch below, which left it live
	// for the entire duration of the closer — and a task's handles are not
	// closed by one thread alone. A MULTI-THREADED TASK BEING KILLED sends
	// every one of its threads down the exit path at once
	// (task_terminate_sibling_threads), so two of them can enter
	// handle_close_all together, both see `type != HANDLE_NONE` on the same
	// slot, and both run the closer on the same object. The second one is
	// working with memory the first already freed — and because EVERY
	// allocation in os64 is zeroed, what it reads back is not garbage but
	// NULL, so it dereferences 0 and the kernel takes a #PF three frames
	// away from anything that looks related.
	//
	// Seen once, in exactly that shape: `hog` (threads + Ctrl+C) faulting at
	// thread_join_close+0x10 on address 0x8, with RDI = 0. Intermittent by
	// nature — it needs two threads inside the same slot — which is why it
	// took a busy machine to show up and did not reproduce on demand.
	//
	// The claim is an ATOMIC EXCHANGE to a CLOSING sentinel, and the SENTINEL
	// (not HANDLE_NONE) is the fix's whole point (tightened twice):
	//
	//   Round 1 exchanged straight to HANDLE_NONE, which shut the two-CLOSERS
	//   race (xchg makes exactly one win). But NONE means "free", and
	//   handle_alloc reclaims free slots — so between our exchange and our
	//   capture of `object` below, another thread's handle_alloc could seize
	//   this very slot and install a NEW object into it. We would then close
	//   or NULL the newcomer's object, leaking ours and corrupting theirs
	//   (Codex #29 rd4).
	//
	//   CLOSING is distinct from NONE, and handle_alloc only ever reclaims a
	//   NONE slot, so a slot we are closing is untouchable until WE set it
	//   NONE — after the object is released. The TYPE is the claim token (a
	//   console handle's object is legitimately NULL, so it cannot be one);
	//   a loser sees CLOSING or NONE and bails.
	handle_type_t type = __atomic_exchange_n(&handle->type, HANDLE_CLOSING, __ATOMIC_ACQ_REL);
	if (type == HANDLE_NONE)
	{
		// We stamped CLOSING onto an already-free slot — undo it so alloc can
		// have it back. (Nobody else can be mid-close on a NONE slot.)
		__atomic_store_n(&handle->type, HANDLE_NONE, __ATOMIC_RELEASE);
		return false;
	}
	if (type == HANDLE_CLOSING)
		return false;   // another closer owns it — must NOT revert, that frees it mid-close

	// We own it, and alloc cannot touch a CLOSING slot, so `object` is stable.
	void *object = handle->object;

	// Dropping the task's reference on the object. For a pipe this is THE
	// refcount that decides EOF (last writer gone) and EPIPE (last reader
	// gone) — closing a handle is how a program says "I am done with this end".
	switch (type)
	{
		case HANDLE_PIPE_READ:
			pipe_close_read_end((pipe_t *)object);
			break;
		case HANDLE_PIPE_WRITE:
			pipe_close_write_end((pipe_t *)object);
			break;
		case HANDLE_FILE:
			handle_file_object_close(object);
			break;
		case HANDLE_DIR:
			handle_dir_object_close(object);
			break;
		case HANDLE_THREAD:
			// Detach: the thread keeps running if it is still going, and
			// nobody will ever collect its answer. The join object frees
			// itself when both references are gone.
			thread_join_close((thread_join_t *)object);
			break;
		case HANDLE_NET_TCP:
			// Orderly shutdown: sends FIN and DETACHES — the closing
			// dance and TIME_WAIT finish in the background (tcp_poll),
			// so closing a handle never blocks the program.
			tcp_conn_close((tcp_conn_t *)object);
			break;
		case HANDLE_NET_ICMP:
			icmp_conn_close((icmp_conn_t *)object);
			break;
		case HANDLE_PTY_MASTER:
			// The terminal side hung up. The slave is buried only when the
			// SEATS are also empty (PTY.md's lifetime rule) — a child still
			// running writes into a grid nobody watches, which GRID mode
			// makes benign by construction.
			pty_master_close((tty_t *)object);
			break;
		case HANDLE_NET_UDP:
			// Hang up: unbinds the ephemeral port and frees the object.
			// Safe on any CR3 (everything it touches is kmalloc'd, upper
			// half) and safe from handle_close_all at task exit (the
			// owning thread has left any blocking read by then).
			udp_conn_close((udp_conn_t *)object);
			break;
		default:
			// Console handles reference no object — nothing to release.
			break;
	}

	// The object is released; NOW free the slot. Order matters: blank the
	// object first, then publish HANDLE_NONE — so the instant alloc can claim
	// this slot (type == NONE), object is already NULL and never a dangling
	// pointer to what we just closed. The slot was UNREUSABLE the whole time
	// it held HANDLE_CLOSING, which is what shut the close-vs-alloc race.
	handle->object = NULL;
	__atomic_store_n(&handle->type, HANDLE_NONE, __ATOMIC_RELEASE);
	return true;
}

void handle_close_all(struct task *t)
{
	task_t *task = (task_t *)t;

	for (int i = 0; i < TASK_MAX_HANDLES; i++)
		if (task->handles[i].type != HANDLE_NONE)
			handle_close(t, i);
}
