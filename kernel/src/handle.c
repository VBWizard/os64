// handle.c — the per-task handle table. See handle.h for the contract.
//
// Deliberately dumb: a fixed array — but NOT "no concurrent access". Every
// thread of a task shares this table, so two threads can open, close, or exit
// against the same slot at once. The slot's TYPE is the claim token, moved
// only by atomics: handle_alloc claims a free slot through CLOSING; two-phase
// allocation claims one as RESERVED and later commits through CLOSING;
// handle_close claims a non-closing state with an exchange. Nothing touches a
// slot it did not win. Spawn still builds a child's table with plain stores,
// and may: the child has not been submitted to the scheduler, so nothing else
// can see it.
//
// (Codex #29 rd4 made close atomic and argued "alloc only ever reclaims a
// NONE slot" — which is only true once alloc's claim is atomic too. Fable's
// review of rd15, 2026-08-25, found alloc still scanning-and-storing: two
// threads opening at once could both win slot N, orphaning one object with no
// handle at all and handing the other thread a handle of the wrong type.)
//
// ── THE PIN (Codex #101 rd4, two P1s with one cause) ────────────────────────
//
// The claim token protects the SLOT. It never protected the OBJECT: a syscall
// resolved a handle to a bare pointer, and from that instant a sibling thread
// could close the same handle, run the closer, and free the object while the
// first thread was still inside an operation on it — a reader parked in
// pipe_read woke into a freed pipe; spawn took a TCP reference on a
// connection the closer had just released. telnetd is the first program to
// share handles between threads AND park in a syscall while its task tears
// down, and it found both.
//
// So a handle is now resolved PINNED. handle_pin does two things in one
// critical section under the task's handleLock: it checks the slot is live,
// and it takes a reference ON THE OBJECT in the object's own currency (a pipe
// end's readers/writers, a file's holds, a TCP conn's pins, a
// listener's busy, a directory's handleRefCount, a join object's refcount, a
// pty's holds, a UDP or ICMP conn's holders). handle_close claims the slot
// under the same lock and only then releases the table's reference — outside
// the lock, because a release can do disk I/O. The two cannot interleave: a
// pin that saw the slot
// live holds its reference before the closer can drop the table's, and a pin
// that arrives after the claim sees CLOSING and refuses. The lock is held for
// a handful of instructions on both sides, never across a park.
//
// The consequence worth stating: an operation in flight IS a holder. A parked
// reader whose handle a sibling closed is the one who frees the pipe when it
// leaves; a file whose last table handle closed mid-read gets its real close
// from the reader's unpin. Spawn's redirections are the other use of the
// lock: handle_share takes the CHILD's own reference in the same critical
// section, so the object is the child's before the load begins (a pin would
// keep a net conn's row but not its line). Lock order: handleLock, then the
// object's own lock (pipe, listener) inside it — never the reverse, because
// no object code ever touches a handle table.

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
#include "driver/net/tcp.h"        // tcp_conn_release / tcp_listener_close — the TCP kinds' releases
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
		// CLAIM WITH A CAS, NOT A LOOK-THEN-STORE (see the file header). A
		// sibling thread scanning this same table sees either NONE (and races
		// us for the CAS — exactly one wins) or CLOSING (and moves on). The
		// reservation is CLOSING rather than the final type so that a handle
		// number leaked to a sibling before `object` is stored cannot be used:
		// handle_get treats CLOSING as "not operable". Object first, then the
		// real type with release semantics, so whoever reads the type sees the
		// object behind it.
		handle_type_t expected = HANDLE_NONE;
		if (__atomic_compare_exchange_n(&task->handles[i].type, &expected, HANDLE_CLOSING,
		                                false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		{
			task->handles[i].object = object;
			__atomic_store_n(&task->handles[i].type, type, __ATOMIC_RELEASE);
			return i;
		}
	}

	printd(DEBUG_TASK, "handle_alloc: task %s is out of handles (max %u)\n",
		task->exename, TASK_MAX_HANDLES);
	return -1;
}

int handle_reserve(struct task *t)
{
	task_t *task = (task_t *)t;

	for (int i = 3; i < TASK_MAX_HANDLES; i++)
	{
		handle_type_t expected = HANDLE_NONE;
		if (__atomic_compare_exchange_n(&task->handles[i].type, &expected,
		                                HANDLE_RESERVED, false,
		                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		{
			task->handles[i].object = NULL;
			return i;
		}
	}

	printd(DEBUG_TASK, "handle_reserve: task %s is out of handles (max %u)\n",
	       task->exename, TASK_MAX_HANDLES);
	return -1;
}

bool handle_commit_reserved(struct task *t, int slot, handle_type_t type, void *object)
{
	task_t *task = (task_t *)t;
	if (slot < 3 || slot >= TASK_MAX_HANDLES || type == HANDLE_NONE ||
	    type == HANDLE_RESERVED || type == HANDLE_CLOSING)
		return false;

	// Claim the reservation through CLOSING before publishing its object. A
	// concurrent task teardown either wins first (and commit refuses, leaving
	// the object with the caller) or sees CLOSING and leaves this owner to
	// finish. After publication, a teardown that already passed the slot is
	// detected through tearingDown and this owner closes the new handle.
	handle_type_t expected = HANDLE_RESERVED;
	if (!__atomic_compare_exchange_n(&task->handles[slot].type, &expected,
	                                 HANDLE_CLOSING, false,
	                                 __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return false;

	task->handles[slot].object = object;
	__atomic_store_n(&task->handles[slot].type, type, __ATOMIC_RELEASE);
	if (__atomic_load_n(&task->tearingDown, __ATOMIC_ACQUIRE))
		(void)handle_close(t, slot);
	return true;
}

bool handle_cancel_reserved(struct task *t, int slot)
{
	task_t *task = (task_t *)t;
	if (slot < 3 || slot >= TASK_MAX_HANDLES)
		return false;

	handle_type_t expected = HANDLE_RESERVED;
	if (!__atomic_compare_exchange_n(&task->handles[slot].type, &expected,
	                                 HANDLE_CLOSING, false,
	                                 __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return false;
	task->handles[slot].object = NULL;
	__atomic_store_n(&task->handles[slot].type, HANDLE_NONE, __ATOMIC_RELEASE);
	return true;
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

// An UNPINNED look at a slot — the table's own use only (handle_close's
// range and liveness pre-check). Nothing may dereference `object` through
// this: the moment it returns, a sibling's close can free what it points at.
// Every syscall resolves through handle_pin.
static handle_t *handle_get(struct task *t, int h)
{
	task_t *task = (task_t *)t;

	// This IS the range check: ring 3 passes whatever integer it feels like.
	if (h < 0 || h >= TASK_MAX_HANDLES)
		return NULL;
	if (task->handles[h].type == HANDLE_NONE ||
	    task->handles[h].type == HANDLE_RESERVED ||
	    task->handles[h].type == HANDLE_CLOSING)
		return NULL;   // free, reserved, or mid-close — none is operable

	return &task->handles[h];
}

// The object-reference half of a pin, in each object's own currency. Runs
// under handleLock, so nothing here may block: every arm is an atomic
// increment or a short spinlock section. The console tags reference no
// object, so they have no arm.
static void handle_object_ref(handle_type_t type, void *object)
{
	switch (type)
	{
		case HANDLE_PIPE_READ:     pipe_ref_read_end((pipe_t *)object); break;
		case HANDLE_PIPE_WRITE:    pipe_ref_write_end((pipe_t *)object); break;
		case HANDLE_FILE:
			// A holder AND a pin, in one step — the closer needs to know
			// whether what remains can answer for the close (a handle) or
			// cannot (an operation), and must never see one without the
			// other (vfs.h `holds`).
			__sync_fetch_and_add(&((vfs_file_t *)object)->holds, VFS_HOLD_PIN | 1);
			break;
		case HANDLE_DIR:
			__sync_fetch_and_add(&((vfs_directory_t *)object)->handleRefCount, 1);
			break;
		case HANDLE_THREAD:        thread_join_ref((thread_join_t *)object); break;
		case HANDLE_NET_UDP:       udp_conn_ref((udp_conn_t *)object); break;
		case HANDLE_NET_TCP:       tcp_conn_pin((tcp_conn_t *)object); break;   // NOT a handle: the last handle's close still hangs up (tcp.h pins)
		case HANDLE_NET_ICMP:      icmp_conn_ref((icmp_conn_t *)object); break;
		case HANDLE_NET_LISTENER:  tcp_listener_hold((tcp_listener_t *)object); break;
		case HANDLE_PTY_MASTER:    pty_master_hold((tty_t *)object); break;
		default: break;
	}
}

bool handle_pin(struct task *t, int h, handle_t *out)
{
	task_t *task = (task_t *)t;

	if (h < 0 || h >= TASK_MAX_HANDLES)
		return false;

	uint64_t flags = spinlock_acquire_irqsave(&task->handleLock);
	handle_type_t type = __atomic_load_n(&task->handles[h].type, __ATOMIC_ACQUIRE);
	if (type == HANDLE_NONE || type == HANDLE_RESERVED || type == HANDLE_CLOSING)
	{
		spinlock_release_irqrestore(&task->handleLock, flags);
		return false;   // free, reserved, or mid-close — none is operable
	}
	// The slot is live and the closer cannot claim it while we hold the
	// lock, so the object is alive and its own reference count can be
	// taken safely. (alloc published `object` before the live type with
	// release semantics; the acquire load above sees it.)
	void *object = task->handles[h].object;
	handle_object_ref(type, object);
	spinlock_release_irqrestore(&task->handleLock, flags);

	out->type = type;
	out->object = object;
	return true;
}

void handle_unpin(const handle_t *pinned)
{
	// The same releases handle_close runs for the table's reference where
	// the object's currency is one count — an in-flight operation is a
	// holder like any other, and whichever holder goes last does the real
	// close (EOF/EPIPE for a pipe end, the VFS close for a file, burial for
	// a pty). The net conns keep hanging up and freeing apart: their close
	// is the handle count's alone, and a pin only keeps the memory.
	switch (pinned->type)
	{
		case HANDLE_PIPE_READ:     pipe_close_read_end((pipe_t *)pinned->object); break;
		case HANDLE_PIPE_WRITE:    pipe_close_write_end((pipe_t *)pinned->object); break;
		case HANDLE_FILE:
		{
			// The pin and its hold go in one atomic step (vfs.h `holds`).
			// If this unpin is the last holder it does the real close, and
			// the verdict goes to the log alone — the syscall that closed
			// the handle has already answered "deferred" (os64/file.h).
			bool deferred;
			(void)handle_file_object_close_verdict(pinned->object, true, &deferred);
			break;
		}
		case HANDLE_DIR:           handle_dir_object_close(pinned->object); break;
		case HANDLE_THREAD:        thread_join_close((thread_join_t *)pinned->object); break;
		case HANDLE_NET_UDP:       udp_conn_release((udp_conn_t *)pinned->object); break;
		case HANDLE_NET_TCP:       tcp_conn_unpin((tcp_conn_t *)pinned->object); break;
		case HANDLE_NET_ICMP:      icmp_conn_release((icmp_conn_t *)pinned->object); break;
		case HANDLE_NET_LISTENER:  tcp_listener_release((tcp_listener_t *)pinned->object); break;
		case HANDLE_PTY_MASTER:    pty_master_unhold((tty_t *)pinned->object); break;
		default: break;
	}
}

bool handle_share(struct task *t, int h, handle_t *out)
{
	task_t *task = (task_t *)t;

	if (h < 0 || h >= TASK_MAX_HANDLES)
		return false;

	uint64_t flags = spinlock_acquire_irqsave(&task->handleLock);
	handle_type_t type = __atomic_load_n(&task->handles[h].type, __ATOMIC_ACQUIRE);
	void *object = task->handles[h].object;
	// The child's reference, in the handle's own currency, taken while the
	// slot is provably live — the closer's claim cannot straddle this lock.
	bool shareable = true;
	switch (type)
	{
		case HANDLE_CONSOLE_IN:
		case HANDLE_CONSOLE_OUT:
		case HANDLE_CONSOLE_ERR:  break;   // a tag, not an object: nothing to hold
		case HANDLE_PIPE_READ:    pipe_ref_read_end((pipe_t *)object); break;
		case HANDLE_PIPE_WRITE:   pipe_ref_write_end((pipe_t *)object); break;
		case HANDLE_FILE:
			__sync_fetch_and_add(&((vfs_file_t *)object)->holds, 1);   // a handle: no pin
			break;
		case HANDLE_NET_TCP:      tcp_conn_ref((tcp_conn_t *)object); break;
		default:                  shareable = false; break;   // free, mid-close, or not a stream
	}
	spinlock_release_irqrestore(&task->handleLock, flags);

	if (!shareable)
		return false;
	out->type = type;
	out->object = object;
	return true;
}

void handle_unshare(const handle_t *shared)
{
	switch (shared->type)
	{
		case HANDLE_PIPE_READ:    pipe_close_read_end((pipe_t *)shared->object); break;
		case HANDLE_PIPE_WRITE:   pipe_close_write_end((pipe_t *)shared->object); break;
		case HANDLE_FILE:         handle_file_object_close(shared->object); break;
		case HANDLE_NET_TCP:      tcp_conn_release((tcp_conn_t *)shared->object); break;
		default: break;
	}
}

// Trampoline body for handle_file_object_close: runs under kKernelPML4 on the
// core's kernel interrupt stack, and carries the filesystem's answer BACK
// (Codex #29 rd14). This used to be `file->fops->close(file);` with the
// result dropped on the floor — and the whole chain below it was built to
// carry that result: fops->close returns int, and fat_close ALREADY computes
// it (f_close != FR_OK -> -1). One discarded return value was the entire
// defect.
//
// A params block rather than the bare vfs_file_t pointer, because the
// trampoline hands back nothing: kmalloc'd, per the house rule that anything
// call_in_kernel_context touches must be HHDM-reachable and never on the
// caller's task-local stack. (The vfs_file_t itself is kmalloc'd too, which
// is what made the old bare-pointer version legal.)
//
// There is deliberately NO "close without a status block" fallback for the
// kmalloc below failing. Rd15 built one; Fable's review found it unreachable:
// the allocator PANICS on exhaustion (allocator.c, and it says so at the site
// — it used to `cli;hlt` under its own lock), and kmalloc adds kHHDMOffset to
// whatever it gets, so `kmalloc() == NULL` cannot happen in this kernel. A
// fallback for an impossible branch is a comment that lies about what can go
// wrong.
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
	bool deferred;
	return handle_file_object_close_verdict(vfs_file, false, &deferred);
}

int handle_file_object_close_verdict(void *vfs_file, bool as_pin, bool *deferred)
{
	vfs_file_t *file = (vfs_file_t *)vfs_file;
	*deferred = false;

	if (file == NULL || file->fops == NULL || file->fops->close == NULL)
		return 0;   // nothing to close is not a failure to close

	// Drop THIS hold — a handle's, or a pin's (which drops its pin too, in
	// the same step). Spawn redirection shares one open file between parent
	// and child (see `holds` in vfs.h) — same rule as pipe ends: only the
	// LAST holder's close actually closes. Atomic because parent and child
	// can close concurrently on different cores.
	//
	// WHO IS LEFT decides what this close can honestly answer, and the one
	// subtraction hands back both counts from the same instant. Another
	// HANDLE will close in its own time and its close answers for the file.
	// A PIN — an operation in flight on another thread — cannot: its unpin
	// does the real close with nowhere to return to, so this close says
	// "deferred" rather than 0. Because a pin and its hold are one word,
	// there is no order of events in which a pin's hold is counted while its
	// pin is not (or the reverse): whatever remains is exactly what the
	// answer describes.
	uint64_t left = __sync_sub_and_fetch(&file->holds, as_pin ? (VFS_HOLD_PIN | 1) : 1);
	if (VFS_HOLDERS(left) > 0)
	{
		*deferred = VFS_PINS(left) > 0 && VFS_HOLDERS(left) <= VFS_PINS(left);
		return 0;   // somebody else still holds it; nothing was flushed here
	}

	// Harvest f_path BEFORE closing — the VFS close frees the file object, and
	// for a HANDLE_FILE, f_path is always the kmalloc'd copy syscall_open made
	// (the fs stores whatever pointer open() was given, so open() must hand it
	// one with handle lifetime — and we are the end of that lifetime). The
	// same pointer is the NAME the flush tripwire below prints: after the
	// close, `file` is gone and this is the only way left to say WHICH file
	// failed — a diagnostic without a name is one nobody can act on.
	char *path_copy = file->f_path;

	// A close can flush to disk, and disk I/O (NVMe/AHCI DMA structures) lives
	// in mappings only kKernelPML4 has — same lesson spawn learned the
	// triple-fault way. But call_in_kernel_context resets RSP to the interrupt
	// stack's TOP, so calling it while ALREADY on that stack (task-exit cleanup
	// closing a dead task's handles) would overwrite our own live frames.
	// CR3 tells us which world we're in.
	uint64_t cr3;
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	int rc;
	if (cr3 == (uint64_t)kKernelPML4)
	{
		rc = file->fops->close(file);
	}
	else
	{
		// kmalloc cannot return NULL (see the params-block comment above), so
		// the block is simply ours. We are past the refcount drop — this
		// handle IS the last one and the close below is the act itself.
		file_close_params_t *p = kmalloc(sizeof(*p));
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
	//
	// THE LINE SAYS WHAT IS TRUE OF EVERY CLOSE THAT FAILS, and no more: a
	// close is also where a file that JUDGES what it was given says no —
	// /sys/console/font refuses a font there — and nothing was on its way to
	// a disk in that case. A message that names a flush on those sends the
	// reader looking for a disk problem that does not exist.
	// DEBUG_EXCEPTIONS, and the choice is load-bearing rather than lazy:
	// printd requires ALL the bits it is given ((kDebugLevel & level) !=
	// level), and DEBUG_VFS is not in DEBUG_MINIMAL_OPTIONS — so tagging this
	// "DEBUG_VFS | something" would make the tripwire invisible on a default
	// boot, which is a check that cannot fire dressed up as one that can.
	// DEBUG_EXCEPTIONS is always on and means exactly what this is: something
	// went wrong that nobody asked to hear about.
	if (rc != 0)
		printd(DEBUG_EXCEPTIONS,
		       "handle_file_object_close: CLOSE FAILED (%d) on '%s' - what was written was not committed (a disk file: may not be on disk; a /sys file: it refused, and says why when read)\n",
		       rc, path_copy ? path_copy : "<unnamed>");

	if (path_copy != NULL)
		kfree(path_copy);
	return rc;
}

// Directory sibling of the file pair above — same kernel-context discipline
// (a directory close COULD flush fs state), same f_path-copy ownership, and
// the same last-holder rule: the handle is one reference and every pinned
// readdir in flight is another (spawn rejects HANDLE_DIR, so those are the
// only two kinds).
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

	if (__sync_sub_and_fetch(&dir->handleRefCount, 1) > 0)
		return;   // a pinned operation still holds it; its unpin closes

	// Harvest before close — the VFS close frees the directory object, and
	// f_path is the kmalloc'd copy syscall_open made (dir flavor).
	// mount_prefix is syscall_open's canonical-path copy (mount-aware
	// readdir), and owner is where the open_dir_count this close must
	// balance lives (vfs.h) — all three die with the object if read after.
	char *path_copy = dir->f_path;
	char *mount_prefix = (char *)dir->mount_prefix;
	vfs_filesystem_t *owner = (vfs_filesystem_t *)dir->owner;

	uint64_t cr3;
	__asm__ volatile("mov %0, cr3" : "=r"(cr3));
	if (cr3 == (uint64_t)kKernelPML4)
		dir->dops->close(dir);
	else
		call_in_kernel_context(dir_close_in_kernel, dir);

	// The decrement pairs with syscall_open's increment BY CALL PATH, not by
	// flag: every dir that reaches this closer came from syscall_open's
	// handle (nothing else mints HANDLE_DIR), so owner-set is the whole
	// condition on both sides — mount_prefix can be NULL here (its kmalloc
	// is allowed to fail) and the count must still balance.
	if (owner != NULL)
		__sync_fetch_and_sub(&owner->open_dir_count, 1);
	if (mount_prefix != NULL)
		kfree(mount_prefix);
	if (path_copy != NULL)
		kfree(path_copy);
}

// Close, and say how it went, as TWO facts because they live in different
// number spaces: the return is whether there was a handle to close at all
// (false for an empty slot, or one another closer owns), and *rc is the
// filesystem's own answer for the LAST close of a file — 0, or its nonzero
// code when what was written could not be committed. A filesystem's -1 and
// "no such handle" must never be one value, which is why this is not a
// single int with a sentinel (it was, for an hour, and a refused font read
// back as a bad handle). The slot is freed whenever the return is true; the
// verdict is about the bytes (os64/file.h).
// *deferred is the third fact: the verdict went to an operation still in
// flight on another thread, and this close cannot have it.
bool handle_close_rc(struct task *t, int h, int *rc, bool *deferred)
{
	*rc = 0;
	*deferred = false;
	// Everything below works through `handle` (== &task->handles[h]); the slot
	// index is closed over by that pointer. The task is needed for one thing,
	// its handleLock.
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
	//
	// UNDER handleLock (the file header's § The pin): the claim is what a
	// handle_pin in progress must not straddle. The lock covers the exchange
	// and the read of `object` only — the release below can sleep in the
	// VFS, and it does not need the lock, because a pin that got in first
	// already holds its own reference on the object.
	task_t *task = (task_t *)t;
	uint64_t lockFlags = spinlock_acquire_irqsave(&task->handleLock);
	handle_type_t type = __atomic_exchange_n(&handle->type, HANDLE_CLOSING, __ATOMIC_ACQ_REL);
	if (type == HANDLE_NONE)
	{
		// We stamped CLOSING onto an already-free slot — undo it so alloc can
		// have it back. (Nobody else can be mid-close on a NONE slot.)
		__atomic_store_n(&handle->type, HANDLE_NONE, __ATOMIC_RELEASE);
		spinlock_release_irqrestore(&task->handleLock, lockFlags);
		return false;
	}
	if (type == HANDLE_CLOSING)
	{
		spinlock_release_irqrestore(&task->handleLock, lockFlags);
		return false;   // another closer owns it — must NOT revert, that frees it mid-close
	}

	// We own it, and alloc cannot touch a CLOSING slot, so `object` is stable.
	void *object = handle->object;
	spinlock_release_irqrestore(&task->handleLock, lockFlags);

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
			// The one closer with something to say — a flush that did not
			// happen, or a file that judged what it was given and refused —
			// when it is the last holder. When an operation on another
			// thread still holds the file, the real close is that thread's
			// unpin, and this one can only say so.
			*rc = handle_file_object_close_verdict(object, false, deferred);
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
			// One handle fewer names the conn; the LAST one's close is the
			// orderly shutdown — FIN, then DETACH, the closing dance and
			// TIME_WAIT finishing in the background (tcp_poll), so closing
			// a handle never blocks the program. An earlier close is a
			// parent stepping away from a session it handed to a child.
			tcp_conn_release((tcp_conn_t *)object);
			break;
		case HANDLE_NET_LISTENER:
			// The door shuts: no new SYN finds it, what was queued is
			// reset, and tcp_poll frees the row once nobody is inside it.
			tcp_listener_close((tcp_listener_t *)object);
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
			// Hang up: unbinds the ephemeral port, wakes a parked reader to
			// its CLOSED verdict, and drops the handle's hold — the last
			// holder frees. Safe on any CR3 (everything it touches is
			// kmalloc'd, upper half).
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

// For the callers that only need to know whether there was a handle to
// close — burial, the redirect plumbing. What the close could not commit is
// already in the log by the time it gets here (handle_file_object_close).
bool handle_close(struct task *t, int h)
{
	int rc;
	bool deferred;
	return handle_close_rc(t, h, &rc, &deferred);
}

void handle_close_all(struct task *t)
{
	task_t *task = (task_t *)t;

	for (int i = 0; i < TASK_MAX_HANDLES; i++)
		if (task->handles[i].type != HANDLE_NONE)
			handle_close(t, i);
}
