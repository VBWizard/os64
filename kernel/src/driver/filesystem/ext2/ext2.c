// ext2.c — the os64 ext2 driver's READ half: mount, path resolution, the
// block map, file/dir reads, stat. The WRITE half (allocation, bitmap
// bookkeeping, dirent surgery — everything that mutates the disk) lives next
// door in ext2_write.c, added 2026-08-04, the day os64 wrote its first ext2
// byte. The two halves share the private seam ext2_internal.h and map onto
// the two op-table pairs: ext2_fops/ext2_dops (read-only — what the ROOT
// mounts until writable-root is ratified) and ext2_rw_fops/ext2_rw_dops
// (the superset secondary mounts get).
//
// The read half was verified against a filesystem os64 NEVER TOUCHED:
// partition 2 of the build image is formatted by the host's mkfs.ext2 and
// populated by debugfs (see the GNUmakefile disk rule +
// tools/gen_ext2_testdata.py). If this code reads that, it reads real ext2 —
// not our own private dialect of it. The write half is judged by the same
// outside authority from the other side: after os64 writes, the host's
// e2fsck -fn must stay green (make fsck-ext2).
//
// Lineage, because it matters to how this file is shaped: ext2 (Rémy Card,
// 1993 — his copyright still heads our ext2_fs.h) is BSD FFS re-expressed:
// block groups are cylinder groups, the inode is the classic UNIX inode with
// 12 direct pointers and single/double/triple indirection. Everything ext3/4
// added sits ON this skeleton, which is why a clean ext2 reader is the right
// foundation and not a dead end.
//
// LOCKING (the successor to the read-only era's proud "NO LOCKS by design" —
// which predicted, correctly, that writes would end it):
//   write_lock  — one writer at a time, held across the WHOLE body of every
//                 write-shaped op. Same cost caveat as the FatFs volume
//                 locks (fat_glue.c): the holder keeps interrupts off for
//                 the whole transfer; graduates to a sleeping mutex when the
//                 interruptible-syscall-bodies debt is paid.
//   open_lock   — guards only the open-inode refcount table (the
//                 rm-of-an-open-file refusal, ruled 2026-08-04); held for a
//                 table scan, never across disk I/O.
//   READS STAY LOCK-FREE. The argument: readers touch only geometry that is
//   immutable after mount; writers mutate only the free-count fields of the
//   cached sb/GDT, which no read path consumes; and every on-disk metadata
//   update is a whole-block RMW serialized at the block driver (NVMe
//   ioLock), so a concurrent reader sees the old block or the new block,
//   never a torn one. The write ordering (ext2_write.c's doctrine) makes
//   both states parse. Two benign races remain, commented at their sites:
//   a dir handle's cached blockbuf can serve a just-deleted entry (stale
//   listing), and a reader's cached size lags an appender (short read).

#include "CONFIG.h"
#include "kmalloc.h"
#include "ext2_vfs.h"
#include "ext2_internal.h"
#include "serial_logging.h"
#include "memset.h"
#include "memcpy.h"
#include "memcmp.h"
#include "strings.h"

// ── Block-level plumbing ────────────────────────────────────────────────────
// (Shared with the write half via ext2_internal.h — the de-static'ing of
// 2026-08-04. Contracts unchanged.)

int ext2_read_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                       uint32_t block, void *buffer)
{
	uint64_t sector = fs->block_device_info->block_device->partition_table
	                      ->parts[fs->partNumber]->partStartSector
	                  + (uint64_t)block * e->sectors_per_block;
	// Block-ops convention (vfs.h): 0 = success.
	return (int)fs->bops->read(fs->block_device_info, sector, buffer,
	                           e->sectors_per_block);
}

// Read inode `ino` (1-based, per ext2) into *out, through caller-owned block
// scratch. Locates the group, the inode table, the block within it — three
// array lookups and one disk read.
int ext2_read_inode_buf(vfs_filesystem_t *fs, ext2_fs_t *e,
                        uint32_t ino, ext2_inode_t *out, uint8_t *buf)
{
	if (ino == 0 || ino > e->sb.s_inodes_count)
		return -1;

	uint32_t group = (ino - 1) / e->sb.s_inodes_per_group;
	uint32_t index = (ino - 1) % e->sb.s_inodes_per_group;
	if (group >= e->groups_count)
		return -1;

	uint32_t byte_off = index * e->inode_size;
	uint32_t block    = e->groups[group].bg_inode_table + byte_off / e->block_size;
	uint32_t in_block = byte_off % e->block_size;

	if (ext2_read_fs_block(fs, e, block, buf) != 0)
		return -1;
	// The on-disk record may be 256 bytes (modern mkfs default); the fields we
	// speak are the classic first 128, which is sizeof(ext2_inode_t).
	memcpy(out, buf + in_block, sizeof(ext2_inode_t));
	return 0;
}

// The read paths' wrapper: per-call scratch, because THEY are lock-free by
// doctrine and pay for it in allocations on purpose. The write half calls
// _buf directly with its mount scratch (see the pool note in
// ext2_internal.h).
int ext2_read_inode(vfs_filesystem_t *fs, ext2_fs_t *e,
                    uint32_t ino, ext2_inode_t *out)
{
	uint8_t *buf = kmalloc(e->block_size);
	if (buf == NULL)
		return -1;
	int rc = ext2_read_inode_buf(fs, e, ino, out, buf);
	kfree(buf);
	return rc;
}

// One entry of an indirect block: read the pointer block, pluck entry `idx`.
// Per-call scratch keeps this reentrant; a block cache makes it fast later.
static uint32_t ext2_indirect_entry(vfs_filesystem_t *fs, ext2_fs_t *e,
                                    uint32_t ind_block, uint32_t idx)
{
	if (ind_block == 0)
		return 0;   // a hole in the map is a hole in the file

	uint32_t *buf = kmalloc(e->block_size);
	if (buf == NULL)
		return 0;
	uint32_t entry = 0;
	if (ext2_read_fs_block(fs, e, ind_block, buf) == 0 && idx < e->ptrs_per_block)
		entry = buf[idx];
	kfree(buf);
	return entry;
}

// The block map: file-relative block index -> on-disk block number (0 = hole,
// which reads as zeros). This walk — 12 direct, then one, two, three levels
// of indirection — IS the classic UNIX inode, unchanged since the 70s.
// (Its allocating twin, ext2_bmap_alloc, lives in ext2_write.c.)
uint32_t ext2_bmap(vfs_filesystem_t *fs, ext2_fs_t *e,
                   const ext2_inode_t *ino, uint32_t fblock)
{
	uint32_t ppb = e->ptrs_per_block;

	if (fblock < EXT2_NDIR_BLOCKS)
		return ino->i_block[fblock];
	fblock -= EXT2_NDIR_BLOCKS;

	if (fblock < ppb)
		return ext2_indirect_entry(fs, e, ino->i_block[EXT2_IND_BLOCK], fblock);
	fblock -= ppb;

	if (fblock < (uint64_t)ppb * ppb)
	{
		uint32_t l1 = ext2_indirect_entry(fs, e, ino->i_block[EXT2_DIND_BLOCK],
		                                  fblock / ppb);
		return ext2_indirect_entry(fs, e, l1, fblock % ppb);
	}
	fblock -= ppb * ppb;

	// Triple indirection: 10 more lines to never think about file size again
	// (at 1KB blocks this reaches ~16GB; at 4KB, ~4TB).
	uint32_t l1 = ext2_indirect_entry(fs, e, ino->i_block[EXT2_TIND_BLOCK],
	                                  fblock / (ppb * ppb));
	uint32_t l2 = ext2_indirect_entry(fs, e, l1, (fblock / ppb) % ppb);
	return ext2_indirect_entry(fs, e, l2, fblock % ppb);
}

// ── Path resolution ─────────────────────────────────────────────────────────

// Find `name` (of length `len`) in the directory `dir_ino` describes.
// Returns the child's inode number, or 0. Linear scan of the directory's
// data blocks — dir_index (hash trees) is a COMPAT feature layered over this
// exact format, so linear works on any ext2 (we also pin ^dir_index on the
// test image to keep the layout at its canonical simplest).
uint32_t ext2_dir_find(vfs_filesystem_t *fs, ext2_fs_t *e,
                       const ext2_inode_t *dir_ino,
                       const char *name, uint32_t len)
{
	uint32_t found = 0;
	uint8_t *buf = kmalloc(e->block_size);
	if (buf == NULL)
		return 0;

	uint32_t dir_size = dir_ino->i_size;
	for (uint32_t off = 0; off < dir_size && !found; off += e->block_size)
	{
		uint32_t disk_block = ext2_bmap(fs, e, dir_ino, off / e->block_size);
		if (disk_block == 0)
			continue;   // hole in a directory: legal, just empty
		if (ext2_read_fs_block(fs, e, disk_block, buf) != 0)
			break;

		uint32_t pos = 0;
		while (pos < e->block_size)
		{
			ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(buf + pos);
			if (de->rec_len == 0)
				break;   // corrupt block — stop rather than loop forever
			if (de->inode != 0 && de->name_len == len &&
			    memcmp(de->name, name, len) == 0)
			{
				found = de->inode;
				break;
			}
			pos += de->rec_len;
		}
	}
	kfree(buf);
	return found;
}

// Walk an absolute path from the root inode (2 — inode numbering starts at
// 1 and the root is famously #2, #1 being the bad-blocks inode). Fills *out
// with the final inode; returns its number or 0.
uint32_t ext2_resolve_path(vfs_filesystem_t *fs, ext2_fs_t *e,
                           const char *path, ext2_inode_t *out)
{
	uint32_t ino = EXT2_ROOT_INO;
	if (ext2_read_inode(fs, e, ino, out) != 0)
		return 0;

	const char *p = path;
	while (*p == '/')
		p++;

	while (*p != '\0')
	{
		// Every intermediate step must be a directory to descend into it.
		if ((out->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
			return 0;

		const char *start = p;
		while (*p != '\0' && *p != '/')
			p++;
		uint32_t len = (uint32_t)(p - start);
		while (*p == '/')
			p++;
		if (len == 0)
			continue;

		ino = ext2_dir_find(fs, e, out, start, len);
		if (ino == 0)
			return 0;   // no such component
		if (ext2_read_inode(fs, e, ino, out) != 0)
			return 0;
	}
	return ino;
}

// ── The open-inode refcount table ───────────────────────────────────────────
// The rm-of-an-open-file refusal (ruled 2026-08-04; contract in
// ext2_internal.h). Every open on EITHER op table passes through here, so a
// writable mount's rm sees read-only handles too. The lock is held for a
// 64-slot scan and nothing else.

bool ext2_openref_register(ext2_fs_t *e, uint32_t ino)
{
	uint64_t flags = spinlock_acquire_irqsave(&e->open_lock);
	int free_slot = -1;
	for (int i = 0; i < EXT2_OPEN_TABLE_SLOTS; i++)
	{
		if (e->open_refs[i].ino == ino)
		{
			e->open_refs[i].count++;
			spinlock_release_irqrestore(&e->open_lock, flags);
			return true;
		}
		if (e->open_refs[i].ino == 0 && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
	{
		// Table full: the open FAILS rather than existing invisibly to rm.
		// 64 distinct open inodes on one mount means something is leaking
		// handles anyway — say so where the leak's author will look.
		spinlock_release_irqrestore(&e->open_lock, flags);
		printd(DEBUG_VFS, "ext2: open-inode table full (%u slots) — refusing open of inode %u\n",
		       EXT2_OPEN_TABLE_SLOTS, ino);
		return false;
	}
	e->open_refs[free_slot].ino = ino;
	e->open_refs[free_slot].count = 1;
	spinlock_release_irqrestore(&e->open_lock, flags);
	return true;
}

void ext2_openref_unregister(ext2_fs_t *e, uint32_t ino)
{
	uint64_t flags = spinlock_acquire_irqsave(&e->open_lock);
	for (int i = 0; i < EXT2_OPEN_TABLE_SLOTS; i++)
	{
		if (e->open_refs[i].ino == ino)
		{
			if (--e->open_refs[i].count == 0)
				e->open_refs[i].ino = 0;
			break;
		}
	}
	spinlock_release_irqrestore(&e->open_lock, flags);
}

uint32_t ext2_openref_count(ext2_fs_t *e, uint32_t ino)
{
	uint64_t flags = spinlock_acquire_irqsave(&e->open_lock);
	uint32_t count = 0;
	for (int i = 0; i < EXT2_OPEN_TABLE_SLOTS; i++)
	{
		if (e->open_refs[i].ino == ino)
		{
			count = e->open_refs[i].count;
			break;
		}
	}
	spinlock_release_irqrestore(&e->open_lock, flags);
	return count;
}

// ── File operations ─────────────────────────────────────────────────────────

// The shared open core: resolve an EXISTING regular file, count it open,
// build the handle. Both op tables' opens compose this — the read-only
// table's open below adds nothing but the mode check; the writable table's
// (ext2_open_rw, ext2_write.c) adds create/truncate/append around it.
int ext2_open_existing(vfs_file_t **vfs_file, const char *path,
                       vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	ext2_handle_t probe;
	uint32_t ino = ext2_resolve_path(vfs_fs, e, path, &probe.inode);
	if (ino == 0)
		return -1;
	if ((probe.inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG)
		return -1;   // directories go through dops; devices/symlinks: later

	ext2_handle_t *h = kmalloc(sizeof(ext2_handle_t));
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (h == NULL || *vfs_file == NULL)
	{
		if (h) kfree(h);
		if (*vfs_file) kfree(*vfs_file);
		*vfs_file = NULL;
		return -1;
	}

	// Count the inode as open BEFORE the handle exists — rm must never see a
	// window where the handle lives but the refcount doesn't.
	if (!ext2_openref_register(e, ino))
	{
		kfree(h);
		kfree(*vfs_file);
		*vfs_file = NULL;
		return -1;
	}

	h->inode = probe.inode;
	h->ino = ino;
	h->pos = 0;
	h->size = probe.inode.i_size;
	h->blockbuf = NULL;
	h->blockbuf_index = (uint32_t)~0u;

	(*vfs_file)->handle = h;
	(*vfs_file)->f_path = (char *)path;   // same contract as FAT: caller's
	                                      // pointer, caller's lifetime problem
	(*vfs_file)->fops = vfs_fs->fops;
	(*vfs_file)->owner = vfs_fs;

	// VFS open-file registry (every open path — ro and rw — funnels through
	// here, so this one call covers all four). ext2's sync is nearly free
	// (write-through already committed everything), but a uniform registry
	// is what lsof-shaped futures are built on.
	vfs_openfile_register(*vfs_file);
	return 0;
}

static int ext2_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                     vfs_filesystem_t *vfs_fs)
{
	// THE READ-ONLY TABLE's open: "r" is the entire mode vocabulary. Refusing
	// "w"/"a"/"c" here — loudly, at open — beats writes that silently go
	// nowhere.
	if (mode == NULL || mode[0] != 'r' || mode[1] != '\0')
		return -1;
	return ext2_open_existing(vfs_file, path, vfs_fs);
}

static int ext2_read(vfs_file_t *vfs_file, void *buffer, size_t size)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_file->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_file->owner;
	ext2_fs_t *e = (ext2_fs_t *)fs->fs_specific;

	if (h->pos >= h->size)
		return 0;   // end of file — 0, exactly like every other read in os64
	if (size > h->size - h->pos)
		size = h->size - h->pos;

	uint8_t *scratch = kmalloc(e->block_size);
	if (scratch == NULL)
		return -1;

	size_t done = 0;
	while (done < size)
	{
		uint32_t fblock   = (uint32_t)(h->pos / e->block_size);
		uint32_t in_block = (uint32_t)(h->pos % e->block_size);
		size_t   chunk    = e->block_size - in_block;
		if (chunk > size - done)
			chunk = size - done;

		uint32_t disk_block = ext2_bmap(fs, e, &h->inode, fblock);
		if (disk_block == 0)
		{
			// A hole: unwritten space inside the file's extent reads as
			// zeros. (Sparse files fall out of the block map for free.)
			memset((uint8_t *)buffer + done, 0, chunk);
		}
		else
		{
			if (ext2_read_fs_block(fs, e, disk_block, scratch) != 0)
			{
				kfree(scratch);
				return done ? (int)done : -1;
			}
			memcpy((uint8_t *)buffer + done, scratch + in_block, chunk);
		}
		done += chunk;
		h->pos += chunk;
	}
	kfree(scratch);
	return (int)done;
}

static int ext2_seek(vfs_file_t *vfs_file, long offset, int whence)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_file->handle;
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
	h->pos = (uint64_t)target;   // past-end is legal; reads there return 0
	return 0;
}

static int ext2_tell(vfs_file_t *vfs_file)
{
	return (int)((ext2_handle_t *)vfs_file->handle)->pos;
}

static int ext2_close(vfs_file_t *vfs_file)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_file->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_file->owner;
	vfs_openfile_unregister(vfs_file);   // before any free — sync-all must
	                                     // never walk onto a dying file
	ext2_openref_unregister((ext2_fs_t *)fs->fs_specific, h->ino);
	// Nothing to flush: the write path is WRITE-THROUGH (every write commits
	// data and inode before returning), so a close is pure bookkeeping.
	if (h->blockbuf != NULL)
		kfree(h->blockbuf);
	kfree(h);
	kfree(vfs_file);
	return 0;
}

// ── Directory operations ────────────────────────────────────────────────────

static int ext2_open_dir(vfs_directory_t **vfs_dir, const char *path,
                         vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	ext2_handle_t probe;
	uint32_t ino = ext2_resolve_path(vfs_fs, e, path, &probe.inode);
	if (ino == 0 || (probe.inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
		return -1;

	ext2_handle_t *h = kmalloc(sizeof(ext2_handle_t));
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	uint8_t *bb = kmalloc(e->block_size);
	if (h == NULL || *vfs_dir == NULL || bb == NULL)
	{
		if (h) kfree(h);
		if (*vfs_dir) kfree(*vfs_dir);
		if (bb) kfree(bb);
		*vfs_dir = NULL;
		return -1;
	}

	// Directories count as open too: rm of a directory somebody is listing
	// is refused the same as rm of a file somebody is reading.
	if (!ext2_openref_register(e, ino))
	{
		kfree(h);
		kfree(*vfs_dir);
		kfree(bb);
		*vfs_dir = NULL;
		return -1;
	}

	h->inode = probe.inode;
	h->ino = ino;
	h->pos = 0;
	h->size = probe.inode.i_size;
	h->blockbuf = bb;
	h->blockbuf_index = (uint32_t)~0u;

	(*vfs_dir)->handle = h;
	(*vfs_dir)->f_path = (char *)path;
	(*vfs_dir)->dops = vfs_fs->dops;
	(*vfs_dir)->owner = vfs_fs;
	return 0;
}

// The fs-neutral dirent contract (vfs.h): 1 = entry, 0 = end, <0 = error.
// "." and ".." are deliberately NOT delivered — they're navigation artifacts,
// not directory content, and Plan 9's readdir dropped them thirty years ago
// with no regrets. An os64 ls never needs to special-case them away.
static int ext2_read_dir(vfs_directory_t *vfs_dir, os64_dirent_t *entry)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_dir->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_dir->owner;
	ext2_fs_t *e = (ext2_fs_t *)fs->fs_specific;

	while (h->pos < h->size)
	{
		uint32_t fblock   = (uint32_t)(h->pos / e->block_size);
		uint32_t in_block = (uint32_t)(h->pos % e->block_size);

		if (h->blockbuf_index != fblock)
		{
			uint32_t disk_block = ext2_bmap(fs, e, &h->inode, fblock);
			if (disk_block == 0)
			{
				// Hole in a directory: skip the whole block.
				h->pos = (uint64_t)(fblock + 1) * e->block_size;
				continue;
			}
			if (ext2_read_fs_block(fs, e, disk_block, h->blockbuf) != 0)
				return -1;
			h->blockbuf_index = fblock;
		}

		ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(h->blockbuf + in_block);
		if (de->rec_len == 0)
			return -1;   // corrupt: refuse to spin
		h->pos += de->rec_len;

		if (de->inode == 0)
			continue;   // deleted entry, space not yet reclaimed
		if (de->name_len == 1 && de->name[0] == '.')
			continue;
		if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')
			continue;

		uint32_t nlen = de->name_len;
		if (nlen > OS64_DIRENT_NAME_MAX)
			nlen = OS64_DIRENT_NAME_MAX;
		memcpy(entry->name, de->name, nlen);
		entry->name[nlen] = '\0';

		// The dirent's file_type byte is authoritative when the FILETYPE
		// feature is on, but we read the child inode anyway — the contract
		// promises SIZE, and only the inode knows it. The mode also gives us
		// dir-ness without trusting the feature flag. One extra block read
		// per entry; the block cache will erase it later.
		ext2_inode_t child;
		if (ext2_read_inode(fs, e, de->inode, &child) != 0)
			return -1;
		bool is_dir = (child.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
		entry->flags = is_dir ? OS64_DE_DIR : 0;
		entry->size  = is_dir ? 0 : child.i_size;
		return 1;
	}
	return 0;   // end of directory — and it stays that way
}

static int ext2_close_dir(vfs_directory_t *vfs_dir)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_dir->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_dir->owner;
	ext2_openref_unregister((ext2_fs_t *)fs->fs_specific, h->ino);
	if (h->blockbuf != NULL)
		kfree(h->blockbuf);
	kfree(h);
	kfree(vfs_dir);
	return 0;
}

// stat is readdir for exactly one name (dops->stat in vfs.h): resolve the
// path to its inode and translate to the fs-neutral dirent — same vocabulary
// ext2_read_dir speaks, sourced by path resolution instead of iteration.
static int ext2_stat(const char *path, os64_dirent_t *entry, vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	ext2_handle_t probe;
	uint32_t ino = ext2_resolve_path(vfs_fs, e, path, &probe.inode);
	if (ino == 0)
		return -1;

	// The name is the path's last component ("/dir1/deep.txt" → "deep.txt");
	// the root's own name is "/" (it has no parent entry to be named by —
	// the same truth fat_stat synthesizes for). Paths arrive canonical, so
	// no trailing slashes to fend off.
	if (path[0] == '/' && path[1] == '\0')
	{
		entry->name[0] = '/';
		entry->name[1] = '\0';
	}
	else
	{
		const char *name = path;
		for (const char *p = path; *p != '\0'; p++)
			if (*p == '/' && p[1] != '\0')
				name = p + 1;
		size_t n = 0;
		while (name[n] != '\0' && n < OS64_DIRENT_NAME_MAX)
		{
			entry->name[n] = name[n];
			n++;
		}
		entry->name[n] = '\0';
	}

	bool is_dir = (probe.inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
	entry->size = is_dir ? 0 : probe.inode.i_size;
	entry->flags = is_dir ? OS64_DE_DIR : 0;
	return 0;
}

// ── Mount-time initialization ───────────────────────────────────────────────

int ext2_initialize_filesystem(vfs_filesystem_t *fs)
{
	// The superblock lives at byte 1024 regardless of block size — read its
	// two sectors directly; everything block-sized comes after we know what
	// "block-sized" means.
	uint8_t *raw = kmalloc(1024);
	ext2_fs_t *e = kmalloc(sizeof(ext2_fs_t));
	if (raw == NULL || e == NULL)
	{
		if (raw) kfree(raw);
		if (e) kfree(e);
		return -1;
	}

	uint64_t part_start = fs->block_device_info->block_device->partition_table
	                          ->parts[fs->partNumber]->partStartSector;
	if (fs->bops->read(fs->block_device_info, part_start + 2, raw, 2) != 0)
	{
		kfree(raw); kfree(e);
		return -1;
	}
	memcpy(&e->sb, raw, sizeof(ext2_super_block_t) < 1024 ? sizeof(ext2_super_block_t) : 1024);
	kfree(raw);

	if (e->sb.s_magic != EXT2_SUPER_MAGIC)
	{
		printd(DEBUG_VFS, "ext2: bad magic 0x%04x (want 0x%04x)\n",
		       e->sb.s_magic, EXT2_SUPER_MAGIC);
		kfree(e);
		return -1;
	}

	// Refuse INCOMPAT features we don't implement — the superblock's own
	// documentation orders exactly this. FILETYPE only changes dirent layout
	// we already handle, so it passes; anything else would be misparsed.
	uint32_t incompat = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                        ? e->sb.s_feature_incompat : 0;
	if (incompat & ~(uint32_t)EXT2_FEATURE_INCOMPAT_FILETYPE)
	{
		printd(DEBUG_VFS, "ext2: unsupported incompat features 0x%08x — refusing to mount\n",
		       incompat);
		kfree(e);
		return -1;
	}

	// RO_COMPAT is the gentler tier: safe to READ regardless, but a WRITER
	// that doesn't understand a bit must not write. Reads always proceed;
	// unknown bits strip the write slots from THIS MOUNT's op tables (they
	// are per-mount copies — kRegisterFilesystem clones them — so nulling
	// here is exactly the NULL-slot-means-refusal doctrine, applied at mount
	// time). The two bits we do support, and why they're safe, are documented
	// at EXT2_SUPPORTED_RO_COMPAT in ext2_internal.h.
	uint32_t ro_compat = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                         ? e->sb.s_feature_ro_compat : 0;
	e->forced_ro = (ro_compat & ~(uint32_t)EXT2_SUPPORTED_RO_COMPAT) != 0;
	if (e->forced_ro && fs->fops != NULL && fs->fops->write != NULL)
	{
		printd(DEBUG_VFS, "ext2: unknown ro_compat features 0x%08x — mounting READ-ONLY (writes stripped)\n",
		       ro_compat);
		fs->fops->write   = NULL;
		fs->fops->sync    = NULL;
		fs->fops->rm      = NULL;
		fs->fops->fputs   = NULL;
		fs->fops->fprintf = NULL;
		if (fs->dops != NULL)
			fs->dops->mkdir = NULL;
	}

	e->block_size        = EXT2_MIN_BLOCK_SIZE << e->sb.s_log_block_size;
	e->sectors_per_block = e->block_size / DISK_SECTOR_SIZE;
	e->ptrs_per_block    = e->block_size / sizeof(uint32_t);
	e->inode_size        = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                           ? e->sb.s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
	e->groups_count      = (e->sb.s_blocks_count + e->sb.s_blocks_per_group - 1)
	                           / e->sb.s_blocks_per_group;

	// The group descriptor table starts in the block AFTER the superblock's
	// block: block 2 at 1KB block size (where the superblock owns block 1),
	// block 1 at larger sizes (where it shares block 0). s_first_data_block
	// encodes exactly this (1 or 0), so first_data_block + 1 is always right.
	uint32_t table_blocks = (e->groups_count * sizeof(ext2_group_desc_t)
	                         + e->block_size - 1) / e->block_size;
	e->groups = kmalloc(table_blocks * e->block_size);
	if (e->groups == NULL)
	{
		kfree(e);
		return -1;
	}
	for (uint32_t b = 0; b < table_blocks; b++)
	{
		if (ext2_read_fs_block(fs, e, e->sb.s_first_data_block + 1 + b,
		                       (uint8_t *)e->groups + b * e->block_size) != 0)
		{
			kfree(e->groups); kfree(e);
			return -1;
		}
	}

	// The write-path scratch pool (doctrine in ext2_internal.h): allocated
	// once here, handed out LIFO under write_lock forever after. Allocated
	// even for read-only mounts — 8KB of insurance beats a NULL surprise if
	// a mount is ever flipped writable in place.
	for (size_t i = 0; i < sizeof(e->wr_scratch) / sizeof(e->wr_scratch[0]); i++)
	{
		e->wr_scratch[i] = kmalloc(e->block_size);
		if (e->wr_scratch[i] == NULL)
		{
			for (size_t j = 0; j < i; j++)
				kfree(e->wr_scratch[j]);
			kfree(e->groups);
			kfree(e);
			return -1;
		}
	}
	e->wr_scratch_used = 0;

	fs->fs_specific = e;
	fs->blockSize = (int)e->block_size;

	printd(DEBUG_VFS, "ext2: mounted %s — %u blocks of %u bytes, %u inodes (%u bytes each), %u group(s)\n",
	       (fs->fops != NULL && fs->fops->write != NULL) ? "read-write" : "read-only",
	       e->sb.s_blocks_count, e->block_size, e->sb.s_inodes_count,
	       e->inode_size, e->groups_count);
	return 0;
}

// ── The op tables (the READ-ONLY pair) ──────────────────────────────────────
// Write-shaped slots are deliberately NULL: a NULL here is refused cleanly at
// the syscall layer (the NULL-slot doctrine), so a mount registered with THIS
// pair cannot write no matter what code exists elsewhere. The ROOT mounts
// this pair until writable-root is ratified; secondary ext2 mounts get the
// superset pair ext2_rw_fops/ext2_rw_dops (ext2_write.c). The stray-write
// tripwire (block_device.c) consults the per-mount copy of exactly these
// slots, so the read-only claim is enforced at the disk too.
vfs_file_operations_t ext2_fops = {
	.initialize = ext2_initialize_filesystem,
	.open  = ext2_open,
	.read  = ext2_read,
	.seek  = ext2_seek,
	.tell  = ext2_tell,
	.close = ext2_close,
};

vfs_directory_operations_t ext2_dops = {
	.open  = ext2_open_dir,
	.read  = ext2_read_dir,
	.close = ext2_close_dir,
	.stat  = ext2_stat,
};
