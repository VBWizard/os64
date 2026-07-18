#ifndef HANDLE_H
#define HANDLE_H

#include <stdbool.h>
#include <stdint.h>

// handle.h — the per-task handle table.
//
// THE CONTRACT (and why it never has to change again):
// A handle is a small int index into a per-task table. Handles 0/1/2 are
// stdin/stdout/stderr by convention — a genuinely good Unix idea, kept. Every
// program reads handle 0 and writes handle 1 and NEVER asks what is behind
// them. That indirection is the whole game: today 0/1/2 are the console;
// tomorrow the shell points them at a pipe; later they can be a file or a TTY.
// The routing changes, the CONTRACT does not, and not one line of any program
// has to be touched. (This is exactly the non-regret guarantee the userland
// roadmap promised when read/write were first made handle-based.)
//
// ONE handle type for everything (files, pipes, later windows) — a handle is a
// tagged reference to a kernel object, and read/write dispatch on the tag.

#define TASK_MAX_HANDLES 16

typedef enum handle_type
{
	HANDLE_NONE = 0,       // free slot
	HANDLE_CONSOLE_IN,     // the keyboard (blocking console_read)
	HANDLE_CONSOLE_OUT,    // the console
	HANDLE_CONSOLE_ERR,    // the console (a distinct tag so it can diverge later)
	HANDLE_PIPE_READ,      // object = pipe_t*, read end
	HANDLE_PIPE_WRITE,     // object = pipe_t*, write end
	HANDLE_FILE,           // object = vfs_file_t*, open file on the root fs
} handle_type_t;

typedef struct handle
{
	handle_type_t type;
	void *object;          // pipe_t* for pipes; NULL for the console tags
} handle_t;

struct task;

// Seed a task's table: 0/1/2 = console in/out/err, everything else free.
void handle_table_init(struct task *t);

// Claim the lowest free slot. Returns the handle, or -1 if the table is full.
int handle_alloc(struct task *t, handle_type_t type, void *object);

// Force an object into a SPECIFIC slot, closing whatever was there. This is
// how spawn redirects a child's stdin/stdout: the child is simply born with a
// pipe end sitting in slot 0 or 1, and it never knows the difference.
bool handle_install(struct task *t, int slot, handle_type_t type, void *object);

// Resolve a handle. Returns NULL for out-of-range or free slots — every
// syscall that takes a handle must treat NULL as "invalid handle", which is
// also the range check (ring 3 hands us whatever integer it likes).
handle_t *handle_get(struct task *t, int h);

// Close one handle: drops the task's reference on the underlying object (for a
// pipe, that is the refcount that decides EOF/EPIPE) and frees the slot.
bool handle_close(struct task *t, int h);

// Close every handle a task holds. Called on task exit — WITHOUT this, a task
// that dies holding a pipe end keeps that end open forever, and the process on
// the other side waits for an EOF that can never come. Death must release the
// ends, or a crashed program hangs its pipeline partner.
void handle_close_all(struct task *t);

// Close a HANDLE_FILE's underlying vfs_file_t from ANY context — a syscall on
// the caller's CR3 (routes through call_in_kernel_context, since a close may
// flush to disk and disk I/O needs the kernel tables) or code already running
// under kKernelPML4 (task-exit cleanup; calls the VFS directly, because
// re-entering call_in_kernel_context from the kernel interrupt stack would
// reset RSP to that stack's TOP and smash the live frames beneath it).
// Also frees the file's f_path — for HANDLE_FILE objects that is ALWAYS the
// kmalloc'd copy syscall_open made (see the lifetime note there).
// Exposed so syscall_open can unwind a file it opened but couldn't table.
void handle_file_object_close(void *vfs_file);

#endif // HANDLE_H
