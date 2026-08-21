// devfs.c — /dev: the kernel's objects as files. See devfs.h for the
// doctrine (why live fops instead of synthfs's snapshots, why /dev/tty is a
// handle alias and not a file, and where each resident comes from).
//
// The labor division with synthfs is the same one procfs and sysfs keep: this
// file owns the path grammar, the node vocabulary, and the behavior of each
// device; synthfs owns the mount dance and the component parser. What is new
// is that nothing here renders TEXT — a device is not a report, so
// synth_text_t and the snapshot fops never appear below.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/filesystem/dev/devfs.h"
#include "driver/filesystem/vfs/vfs.h"
#include "driver/filesystem/vfs/synthfs.h"
#include "handle.h"
#include "kmalloc.h"
#include "memset.h"
#include "strings/strings.h"
#include "serial_logging.h"
#include "CONFIG.h"

// ── The node vocabulary ─────────────────────────────────────────────────────

typedef enum
{
	DEV_NODE_NONE = 0,   // no such path
	DEV_NODE_ROOT,       // "/" — the directory itself
	DEV_NODE_NULL,       // "/null"
	DEV_NODE_ZERO,       // "/zero"
	DEV_NODE_FULL,       // "/full"
	DEV_NODE_TTY         // "/tty" — never opened as a file (see devfs.h)
} dev_node_t;

// ONE table, walked by every path lookup and by readdir alike. Counted, never
// a literal: sysfs learned on 2026-08-20 that a hardcoded entry count is how a
// newly added node silently fails to list, and a listing that drops a name
// reads exactly like a name that does not exist.
typedef struct
{
	const char *name;
	dev_node_t  node;
} dev_entry_t;

static const dev_entry_t kDevNodes[] = {
	{ "null", DEV_NODE_NULL },
	{ "zero", DEV_NODE_ZERO },
	{ "full", DEV_NODE_FULL },
	{ "tty",  DEV_NODE_TTY  },
};
static const int kDevNodeCount = (int)(sizeof(kDevNodes) / sizeof(kDevNodes[0]));

// The open file's private state. There is no position, no snapshot, and no
// buffer — a device answers from its nature, not from stored bytes. The node
// tag is the entire handle, which is the honest measure of how little state a
// void requires.
typedef struct
{
	dev_node_t node;
} dev_file_handle_t;

// The directory handle: a cursor into kDevNodes, nothing more.
typedef struct
{
	dev_node_t node;    // which directory (only ROOT exists today)
	int        index;   // resume cursor for readdir
} dev_dir_handle_t;

// ── Path parsing ────────────────────────────────────────────────────────────
// The grammar is one component deep and deliberately stays that way in this
// slice: "/" is the directory, "/<name>" is a device, anything else is
// nothing. Block devices and terminals will add a second level when they
// arrive, and this is the function that will grow to meet them.
//
// Paths arrive fs-local — vfs_resolve_mount has already stripped "/dev" —
// and always absolute, so "/dev" itself reaches here as "/".
static dev_node_t dev_parse_path(const char *path)
{
	char comp[64];
	size_t pos = 0;

	if (path == NULL)
		return DEV_NODE_NONE;

	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
		return DEV_NODE_ROOT;   // "/" — the mount root itself

	dev_node_t found = DEV_NODE_NONE;
	for (int i = 0; i < kDevNodeCount; i++)
		if (strcmp(comp, kDevNodes[i].name) == 0)
		{
			found = kDevNodes[i].node;
			break;
		}

	if (found == DEV_NODE_NONE)
		return DEV_NODE_NONE;

	// A trailing component makes it a different path, not this one: "/null/x"
	// is not /dev/null, and saying so is cheaper than the confusion of a
	// permissive parse (synthfs.h's ruling about "7x" not being entry 7).
	char extra[64];
	if (synth_next_component(path, &pos, extra, sizeof(extra)))
		return DEV_NODE_NONE;

	return found;
}

// ── File operations ─────────────────────────────────────────────────────────

// Every device here accepts every access mode: a void that refused to be
// opened for reading would be a strange void, and /dev/full's whole job is to
// accept the open and then fail the WRITE. ("d" is the directory mode and
// belongs to dops — a file open that asks for it is a caller error, not a
// device.)
static int dev_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                    vfs_filesystem_t *vfs_fs)
{
	dev_node_t node = dev_parse_path(path);

	if (mode == NULL || mode[1] != '\0' ||
	    (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a' && mode[0] != 'c'))
		return -1;

	// ROOT is a directory (dops), NONE does not exist, and TTY never gets
	// here at all — syscall_open answers it as a handle alias before the
	// filesystem is ever asked to open anything (devfs.h, THE ALIAS). If it
	// somehow arrives, refusing is the honest answer: this layer cannot
	// produce a terminal.
	if (node != DEV_NODE_NULL && node != DEV_NODE_ZERO && node != DEV_NODE_FULL)
		return -1;

	dev_file_handle_t *h = kmalloc(sizeof(dev_file_handle_t));
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (h == NULL || *vfs_file == NULL)
	{
		if (h) kfree(h);
		if (*vfs_file) kfree(*vfs_file);
		*vfs_file = NULL;
		return -1;
	}

	h->node = node;

	// The allocator zeroes everything, so every field not set here — the
	// pipe pointers, the open-registry links, the position lock — starts at
	// its natural default.
	(*vfs_file)->filetype = FILETYPE_DEVFILE;
	(*vfs_file)->handle   = h;
	(*vfs_file)->f_path   = (char *)path;   // caller's pointer, caller's lifetime
	                                        // (the handle closer frees it — vfs.h)
	(*vfs_file)->fops     = vfs_fs->fops;
	(*vfs_file)->owner    = vfs_fs;
	return 0;
}

// Reading a device. Runs under kKernelPML4 on a kernel bounce buffer (the
// HANDLE_FILE path ferries through call_in_kernel_context), so `buffer` is
// always kernel memory here and a plain memset is safe.
//
// Note what /dev/zero does NOT do: it never returns short and never returns
// 0. Zero means EOF everywhere in os64, and a faucet has no end — `cat
// /dev/zero` is meant to run until you stop it, exactly as it has since
// System V. /dev/full reads as zeros for the same reason Linux's does: its
// divergence is on the WRITE side, and a reader should not have to care.
static int dev_read(vfs_file_t *vfs_file, void *buffer, size_t size)
{
	dev_file_handle_t *h = (dev_file_handle_t *)vfs_file->handle;

	if (h == NULL || buffer == NULL)
		return -1;

	switch (h->node)
	{
		case DEV_NODE_NULL:
			return 0;   // EOF, immediately and forever — the void is empty

		case DEV_NODE_ZERO:
		case DEV_NODE_FULL:
			if (size == 0)
				return 0;
			memset(buffer, 0, size);
			return (int)size;

		default:
			return -1;
	}
}

// Writing to a device. Same kernel-context guarantee as read.
//
// null and zero both CONSUME: they report the full count so the caller never
// retries the tail. (A short write here would make husk's `>` loop forever
// against a void, which is a memorable way to hang a shell.) /dev/full is the
// deliberate opposite — the one surface in os64 that fails a write on demand,
// so that error paths can be exercised without corrupting a real filesystem.
static int dev_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	dev_file_handle_t *h = (dev_file_handle_t *)vfs_file->handle;

	if (h == NULL || buffer == NULL)
		return -1;

	switch (h->node)
	{
		case DEV_NODE_NULL:
		case DEV_NODE_ZERO:
			return (int)size;   // swallowed whole

		case DEV_NODE_FULL:
			return -1;          // no space, on purpose, every time

		default:
			return -1;
	}
}

// A device has no position, so seeking is a no-op that SUCCEEDS. That is not
// laziness: a program that seeks to the start before writing (a perfectly
// ordinary thing to do) should not fail against /dev/null, and there is no
// state here for the seek to be wrong about. tell answers 0 for the same
// reason — the honest report of a file that is never anywhere.
static int dev_seek(vfs_file_t *vfs_file, long offset, int whence)
{
	(void)vfs_file; (void)offset; (void)whence;
	return 0;
}

static int dev_tell(vfs_file_t *vfs_file)
{
	(void)vfs_file;
	return 0;
}

static int dev_close(vfs_file_t *vfs_file)
{
	if (vfs_file == NULL)
		return 0;

	if (vfs_file->handle != NULL)
		kfree(vfs_file->handle);
	kfree(vfs_file);
	return 0;
}

// ── Directory operations ────────────────────────────────────────────────────

static int dev_open_dir(vfs_directory_t **vfs_dir, const char *path,
                        vfs_filesystem_t *vfs_fs)
{
	if (dev_parse_path(path) != DEV_NODE_ROOT)
		return -1;   // devfs is one directory deep in this slice

	dev_dir_handle_t *h = kmalloc(sizeof(dev_dir_handle_t));
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	if (h == NULL || *vfs_dir == NULL)
	{
		if (h) kfree(h);
		if (*vfs_dir) kfree(*vfs_dir);
		*vfs_dir = NULL;
		return -1;
	}

	h->node  = DEV_NODE_ROOT;
	h->index = 0;

	(*vfs_dir)->handle = h;
	(*vfs_dir)->f_path = (char *)path;   // same lifetime contract as files
	(*vfs_dir)->dops   = vfs_fs->dops;
	(*vfs_dir)->owner  = vfs_fs;
	return 0;
}

// One entry per call, walking kDevNodes in table order. Every resident is a
// regular file as far as the dirent contract goes — os64 has no "character
// device" flag and deliberately does not invent one here: the flag would have
// exactly one consumer (an `ls` that wanted to color it differently), and
// nothing in the system behaves differently for having read it. If a real
// consumer turns up, OS64_DE_* has room.
//
// Size is 0 for all of them, which is the truest number available: /dev/null
// holds nothing and /dev/zero holds an infinity that no uint64_t can report.
static int dev_read_dir(vfs_directory_t *vfs_dir, os64_dirent_t *entry)
{
	dev_dir_handle_t *h = (dev_dir_handle_t *)vfs_dir->handle;

	memset(entry, 0, sizeof(*entry));

	if (h == NULL || h->node != DEV_NODE_ROOT)
		return -1;

	if (h->index >= kDevNodeCount)
		return 0;   // end of directory

	strncpy(entry->name, kDevNodes[h->index].name, OS64_DIRENT_NAME_MAX);
	entry->name[OS64_DIRENT_NAME_MAX] = '\0';
	entry->size  = 0;
	entry->flags = 0;   // a regular file, as far as anything can tell
	entry->mtime = 0;   // this filesystem has no time to give you
	h->index++;
	return 1;
}

static int dev_close_dir(vfs_directory_t *vfs_dir)
{
	if (vfs_dir == NULL)
		return 0;

	if (vfs_dir->handle != NULL)
		kfree(vfs_dir->handle);
	kfree(vfs_dir);
	return 0;
}

// stat is readdir for exactly one name (vfs.h) — file OR directory.
static int dev_stat(const char *path, os64_dirent_t *entry, vfs_filesystem_t *vfs_fs)
{
	dev_node_t node = dev_parse_path(path);
	(void)vfs_fs;

	memset(entry, 0, sizeof(*entry));

	if (node == DEV_NODE_NONE)
		return -1;

	if (node == DEV_NODE_ROOT)
	{
		entry->flags = OS64_DE_DIR;
		strncpy(entry->name, "dev", OS64_DIRENT_NAME_MAX);
		return 0;
	}

	for (int i = 0; i < kDevNodeCount; i++)
		if (kDevNodes[i].node == node)
		{
			strncpy(entry->name, kDevNodes[i].name, OS64_DIRENT_NAME_MAX);
			entry->name[OS64_DIRENT_NAME_MAX] = '\0';
			return 0;
		}

	return -1;
}

// ── The alias hook ──────────────────────────────────────────────────────────
// syscall_open's one question for devfs. The doctrine — why /dev/tty cannot
// answer through fops, and why this is a direct call rather than an op-table
// slot — is in devfs.h.

static vfs_filesystem_t *kDevFilesystem = NULL;   // set at mount; the identity
                                                  // check the alias hook needs

bool devfs_handle_alias(vfs_filesystem_t *fs, const char *path,
                        const char *mode, handle_type_t *type)
{
	if (fs == NULL || fs != kDevFilesystem || type == NULL)
		return false;

	if (dev_parse_path(path) != DEV_NODE_TTY)
		return false;

	// "d" is the directory mode, and a terminal is not a directory. Refusing
	// here rather than aliasing sends `open("/dev/tty", "d")` down the
	// ordinary path, where dops answers the honest -1 — a readdir handle on a
	// keyboard would be a much stranger thing to hand back than a failure.
	if (mode == NULL || mode[1] != '\0' ||
	    (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a' && mode[0] != 'c'))
		return false;

	// Reading my terminal is the keyboard; writing it is the glass. Both are
	// late-bound to task_tty(caller) at every access, which is what lets one
	// tag serve a VT and a pty slave alike (PTY.md).
	if (mode != NULL && mode[0] == 'r')
		*type = HANDLE_CONSOLE_IN;
	else
		*type = HANDLE_CONSOLE_OUT;

	return true;
}

// ── Mounting ────────────────────────────────────────────────────────────────

vfs_file_operations_t dev_fops = {
	.open  = dev_open,
	.read  = dev_read,
	.write = dev_write,
	.seek  = dev_seek,
	.tell  = dev_tell,
	.close = dev_close,
};

vfs_directory_operations_t dev_dops = {
	.open  = dev_open_dir,
	.read  = dev_read_dir,
	.close = dev_close_dir,
	.stat  = dev_stat,
};

void devfs_mount(void)
{
	if (!synthfs_mount("/dev", &dev_fops, &dev_dops, "the kernel's objects as files"))
		return;

	// Remember WHICH filesystem became /dev, so the alias hook can recognize
	// its own mount rather than trusting a path string that has already been
	// stripped of its prefix by the time syscall_open asks.
	const char *tail = NULL;
	kDevFilesystem = vfs_resolve_mount("/dev", &tail);
}
