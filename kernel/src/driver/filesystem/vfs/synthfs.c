// synthfs.c — the shared machinery of synthetic filesystems. See synthfs.h
// for the seam's doctrine; this code was born in procfs.c (where its comments
// earned their scars) and moved here verbatim when sysfs became the second
// customer (2026-08-08).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/filesystem/vfs/synthfs.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "strings/strings.h"
#include "sprintf.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — the mount line belongs on the glass too
#include "CONFIG.h"

// ── The growable text buffer ────────────────────────────────────────────────

bool synth_text_init(synth_text_t *t, size_t cap)
{
	t->buf = kmalloc(cap);
	t->len = 0;
	t->cap = cap;
	t->oom = false;
	return t->buf != NULL;
}

static bool synth_text_grow(synth_text_t *t, size_t needed)
{
	size_t newcap = t->cap ? t->cap : 256;
	while (newcap < needed)
		newcap *= 2;

	char *nb = kmalloc(newcap);
	if (nb == NULL)
	{
		t->oom = true;
		return false;
	}
	memcpy(nb, t->buf, t->len);
	kfree(t->buf);
	t->buf = nb;
	t->cap = newcap;
	return true;
}

// Append one formatted line. Deliberately renders into a bounded stack line
// buffer first: snprintf tells us the true length, so the grow decision is
// made with the real number rather than a guess.
void synth_text_addf(synth_text_t *t, const char *fmt, ...)
{
	char line[512];
	va_list args;

	if (t->buf == NULL)
		return;

	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (n < 0)
		return;
	size_t want = (size_t)n;
	if (want >= sizeof(line))
		want = sizeof(line) - 1;   // truncated at the line buffer; still valid text

	if (t->len + want + 1 > t->cap && !synth_text_grow(t, t->len + want + 1))
		return;

	memcpy(t->buf + t->len, line, want);
	t->len += want;
	t->buf[t->len] = '\0';
}

// ── Path parsing helpers ────────────────────────────────────────────────────

bool synth_next_component(const char *path, size_t *pos, char *out, size_t outlen)
{
	while (path[*pos] == '/')
		(*pos)++;
	if (path[*pos] == '\0')
		return false;

	size_t n = 0;
	while (path[*pos] != '\0' && path[*pos] != '/')
	{
		if (n + 1 < outlen)
			out[n++] = path[*pos];
		(*pos)++;
	}
	out[n] = '\0';
	return true;
}

bool synth_parse_u64(const char *s, uint64_t *out)
{
	if (s[0] == '\0')
		return false;

	uint64_t v = 0;
	for (size_t i = 0; s[i] != '\0'; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return true;
}

bool synth_name_in(const char *name, const char **table, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (strcmp(name, table[i]) == 0)
			return true;
	return false;
}

// ── The snapshot file handle ────────────────────────────────────────────────

void *synth_snapshot_publish(vfs_file_t **vfs_file, synth_text_t *text,
                             const char *path, vfs_filesystem_t *vfs_fs,
                             size_t handle_size, int filetype)
{
	if (text->oom)
		synth_text_addf(text, "(truncated: out of memory)\n");

	synth_snapshot_t *h = kmalloc(handle_size);
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (h == NULL || *vfs_file == NULL)
	{
		if (h) kfree(h);
		if (*vfs_file) kfree(*vfs_file);
		if (text->buf) kfree(text->buf);
		*vfs_file = NULL;
		return NULL;
	}

	// The allocator zeroes everything, so the caller's fields past the
	// snapshot head start at their natural defaults.
	h->data = text->buf;
	h->size = text->len;
	h->pos  = 0;

	(*vfs_file)->filetype = filetype;
	(*vfs_file)->handle   = h;
	(*vfs_file)->f_path   = (char *)path;   // caller's pointer, caller's lifetime
	                                        // (the handle closer frees it — vfs.h)
	(*vfs_file)->fops     = vfs_fs->fops;
	(*vfs_file)->owner    = vfs_fs;
	return h;
}

int synth_snapshot_read(vfs_file_t *vfs_file, void *buffer, size_t size)
{
	synth_snapshot_t *h = (synth_snapshot_t *)vfs_file->handle;

	if (h->data == NULL || h->pos >= h->size)
		return 0;   // end of file — 0, like every other read in os64
	if (size > h->size - h->pos)
		size = h->size - h->pos;

	memcpy(buffer, h->data + h->pos, size);
	h->pos += size;
	return (int)size;
}

int synth_snapshot_seek(vfs_file_t *vfs_file, long offset, int whence)
{
	synth_snapshot_t *h = (synth_snapshot_t *)vfs_file->handle;
	int64_t base;

	switch (whence)
	{
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = (int64_t)h->pos; break;
		case SEEK_END: base = (int64_t)h->size; break;
		default: return -1;
	}
	int64_t target = base + offset;
	if (target < 0)
		return -1;
	h->pos = (size_t)target;   // past-end is legal; reads there return 0
	return 0;
}

int synth_snapshot_tell(vfs_file_t *vfs_file)
{
	return (int)((synth_snapshot_t *)vfs_file->handle)->pos;
}

int synth_snapshot_close(vfs_file_t *vfs_file)
{
	synth_snapshot_t *h = (synth_snapshot_t *)vfs_file->handle;

	if (h != NULL)
	{
		if (h->data != NULL)
			kfree(h->data);
		kfree(h);   // frees the whole embedding handle — synthfs.h's contract
	}
	kfree(vfs_file);
	return 0;
}

// ── Mounting ────────────────────────────────────────────────────────────────

bool synthfs_mount(const char *prefix,
                   const vfs_file_operations_t *fops,
                   const vfs_directory_operations_t *dops,
                   const char *what)
{
	if (kMountCount >= VFS_MAX_MOUNTS)
	{
		// ON THE GLASS, not just the log. A synthetic filesystem that fails to
		// mount leaves no wreckage to find later — /proc simply is not there,
		// and every symptom points somewhere else. This is the one moment
		// anybody can be told (vfs.h, THE CEILING).
		printd(DEBUG_BOOT, "BOOT: mount table full (%u) — %s not mounted\n",
		       VFS_MAX_MOUNTS, prefix);
		printf("MOUNT TABLE FULL (%u) — %s NOT mounted\n", VFS_MAX_MOUNTS, prefix);
		return false;
	}

	vfs_filesystem_t *fs = kmalloc(sizeof(vfs_filesystem_t));
	if (fs == NULL)
	{
		printd(DEBUG_BOOT, "BOOT: out of memory — %s not mounted\n", prefix);
		return false;
	}

	fs->fops = kmalloc(sizeof(vfs_file_operations_t));
	fs->dops = kmalloc(sizeof(vfs_directory_operations_t));
	if (fs->fops == NULL || fs->dops == NULL)
	{
		if (fs->fops) kfree(fs->fops);
		if (fs->dops) kfree(fs->dops);
		kfree(fs);
		printd(DEBUG_BOOT, "BOOT: out of memory — %s not mounted\n", prefix);
		return false;
	}
	memcpy(fs->fops, fops, sizeof(vfs_file_operations_t));
	memcpy(fs->dops, dops, sizeof(vfs_directory_operations_t));

	// bops stays NULL — nothing under a synthetic filesystem ever reads a
	// sector, and a NULL there is the honest statement of that. (Everything
	// else in the struct is left zeroed by the allocator: no superblock, no
	// block device, no partition number, because none of those things exist
	// here.)

	vfs_mount_entry_t *m = &kMountTable[kMountCount];
	strncpy(m->prefix, prefix, VFS_MOUNT_PREFIX_MAX - 1);
	m->prefix[VFS_MOUNT_PREFIX_MAX - 1] = '\0';
	m->prefix_len = strlen(m->prefix);
	// part_guid stays all-zero: there is no partition. Mounting AFTER the
	// auto-mount sweep keeps that zero out of the sweep's dedupe comparisons,
	// where it could otherwise match a partition whose GUID failed to read.
	memset(m->part_guid, 0, sizeof(m->part_guid));
	m->fs = fs;
	kMountCount++;

	printd(DEBUG_BOOT, "BOOT: mounted %s (synthetic — %s)\n", prefix, what);
	// The glass line: "mounted proc at /proc". Synthetic prefixes are
	// canonical "/name" (vfs.h), so the name is the prefix minus its slash.
	printf("mounted %s at %s\n", prefix + 1, prefix);
	return true;
}
