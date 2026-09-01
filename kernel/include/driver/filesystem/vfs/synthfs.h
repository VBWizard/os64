#ifndef SYNTHFS_H
#define SYNTHFS_H

// synthfs — the shared machinery of SYNTHETIC filesystems (the ones with no
// disk underneath: /proc renders the scheduler, /sys renders the hardware).
//
// Extracted from procfs (2026-08-08) when sysfs became its second customer.
// procfs was accidentally written as a library with one caller: the growable
// text buffer, the path-component parsers, the whole snapshot-file-handle
// life cycle, and the mount dance were all generic from birth — only the
// generators and the ctl vocabulary ever knew what a task was. This header
// is that seam made explicit. A synthetic filesystem keeps for itself:
//
//   - its path grammar (what the components MEAN),
//   - its generators (what text a file holds),
//   - its directory listing (what names exist),
//
// and delegates here: buffer growth, the read/seek/tell/close of a rendered
// snapshot, and claiming a prefix in kMountTable.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/filesystem/vfs/vfs.h"

// ── The growable text buffer ────────────────────────────────────────────────
// Every synthetic file is built by appending formatted lines into one of
// these, then handed to the file handle as a fixed snapshot. Growth is
// allocate-copy-free (kmalloc has no realloc), doubling each time — a status
// file is a few hundred bytes and never reallocates; a big listing might do
// it once.

typedef struct
{
	char  *buf;
	size_t len;   // bytes used, not counting the NUL
	size_t cap;   // bytes allocated
	bool   oom;   // a growth failed — the text is truncated, and says so
} synth_text_t;

bool synth_text_init(synth_text_t *t, size_t cap);
void synth_text_addf(synth_text_t *t, const char *fmt, ...);

// ── Whitespace-columned reports: escaping a field that is DATA ──────────────
// A row of whitespace-separated columns only parses if no column contains
// whitespace, and some of them are not ours to promise that about. A GPT
// partition name is arbitrary bytes off somebody's disk — "Basic data
// partition" is what Windows writes by default — and a mount prefix is a
// path, which os64 lets contain anything a filename may. One such value
// turned a 12-column /sys/mounts row into 15 and df rejected the whole
// report as malformed.
//
// So a DATA field is escaped and a reader reverses it (os64_unescape_field
// in libos64): backslash becomes `\\`, and any byte at or below 0x20 or
// equal to 0x7F becomes `\xHH`. Nothing else changes, which is why every row
// this kernel has ever printed for a well-behaved disk is byte-identical
// before and after — the escape shows up exactly where the format was
// already broken. Worst-case growth is 4x plus the NUL; the caller sizes
// `out` for it.
//
// SANITIZING was the alternative and it is a trap: turning "Basic data
// partition" into "Basic_data_partition" prints something that is not the
// partition's name, and `mount` matches GPT names verbatim — so the reader
// would be shown a name the machine refuses to accept back.
void synth_text_escape(const char *in, char *out, size_t cap);

// ── Path parsing helpers ────────────────────────────────────────────────────
// Paths arrive fs-local (the mount prefix is already stripped by
// vfs_resolve_mount), always absolute. These are the neutral tools every
// synthetic path grammar is built from.

// Pull one '/'-delimited component out of `path` starting at *pos. Returns
// false at end of string. The component is copied (bounded) into `out`.
bool synth_next_component(const char *path, size_t *pos, char *out, size_t outlen);

// Strict decimal parse — the whole component must be digits. "7x" is not
// entry 7, it is a name that does not exist, and saying so is cheaper than
// the confusion of a permissive parse.
bool synth_parse_u64(const char *s, uint64_t *out);

// Is `name` one of the `count` strings in `table`?
bool synth_name_in(const char *name, const char **table, size_t count);

// ── The snapshot file handle ────────────────────────────────────────────────
// A synthetic file is generated WHOLE at open time and served as an immutable
// snapshot (an internally consistent file beats a fresh one — you can never
// read the first half of one state and the second half of its successor's).
// A filesystem that needs private per-handle state (procfs's ctl target)
// embeds this as the FIRST member of its own handle struct and passes the
// outer size to synth_snapshot_publish — the generic fops only ever touch
// the embedded head, and close frees the outer allocation by the same
// pointer.

typedef struct
{
	char  *data;   // the snapshot (NULL only if generation failed outright)
	size_t size;
	size_t pos;
} synth_snapshot_t;

// Wire a rendered text into a fresh handle + vfs_file_t. Consumes `text` on
// success (the buffer becomes the snapshot) AND on failure (it is freed).
// Appends the truncation notice first if the text ran out of memory.
// Returns the handle (handle_size >= sizeof(synth_snapshot_t), extra bytes
// zeroed for the caller's own fields), or NULL with *vfs_file NULL.
void *synth_snapshot_publish(vfs_file_t **vfs_file, synth_text_t *text,
                             const char *path, vfs_filesystem_t *vfs_fs,
                             size_t handle_size, int filetype);

int synth_snapshot_read(vfs_file_t *vfs_file, void *buffer, size_t size);
int synth_snapshot_seek(vfs_file_t *vfs_file, long offset, int whence);
int64_t synth_snapshot_tell(vfs_file_t *vfs_file);
int synth_snapshot_close(vfs_file_t *vfs_file);

// ── Mounting ────────────────────────────────────────────────────────────────
// Claim `prefix` in kMountTable with copies of the given op tables.
// kRegisterFilesystem cannot be used: it reaches through
// device->block_device->ops to copy block operations, and there is no block
// device here — bops stays NULL, the honest statement that nothing under a
// synthetic filesystem ever reads a sector. `what` is the human phrase for
// the serial boot line ("processes as files"); the glass line is derived
// from the prefix itself. Returns false (and mounts nothing) if the table
// is full or memory is short.
bool synthfs_mount(const char *prefix,
                   const vfs_file_operations_t *fops,
                   const vfs_directory_operations_t *dops,
                   const char *what);

#endif
