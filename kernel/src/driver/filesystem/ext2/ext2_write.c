// ext2_write.c — the os64 ext2 driver's WRITE half, born 2026-08-04: the day
// os64 wrote its first ext2 byte. The read half (ext2.c) was verified against
// a filesystem os64 never touched; this half is judged by the same outside
// authority from the other side — after os64 writes, the host's e2fsck -fn
// must stay green (make fsck-ext2). Every structure this file maintains
// (bitmaps, free counts, i_blocks, links_count, bg_used_dirs_count) is
// something e2fsck independently recomputes and compares.
//
// ── THE WRITE-ORDERING DOCTRINE (stated once, applied everywhere) ──────────
// ext2 has no journal; what it has instead is careful ordering, the same
// discipline FFS shipped in 1983. The rule pair below guarantees that a
// crash between any two disk writes leaves at worst a LEAK (a block or inode
// marked used but unreachable — e2fsck reclaims it), never a DANGLING
// REFERENCE (a pointer to something freed or never initialized):
//
//   ALLOCATE-THEN-REFERENCE. Bringing a block/inode into use: (1) write the
//   new thing's CONTENT (zeroed indirect block, dir block with a valid
//   rec_len cover, file data, initialized inode); (2) write the bitmap +
//   free counts that mark it used; (3) LAST, write the reference that makes
//   it reachable — indirect-block entry then inode, or inode then parent
//   dirent.
//
//   DEREFERENCE-THEN-FREE. Retiring: (1) remove the reference (dirent gone /
//   inode written with a zeroed map); (2) THEN clear bitmaps and bump free
//   counts.
//
// Two standing invariants ride along: i_blocks counts 512-byte sectors and
// INCLUDES indirect blocks (e2fsck checks the sum); i_block[] and i_size
// land in the SAME single inode write-back, so a reader never sees a size
// that outruns the map.
//
// ── DURABILITY: FULL WRITE-THROUGH (Chris's ruling, 2026-08-04) ────────────
// Every write-shaped op commits everything it changed — data, bitmaps,
// group descriptors, superblock counts, the inode — before it returns.
// There is no dirty cache, so fops->sync has nothing left to do and returns
// success immediately. This makes ext2 honestly BETTER than FAT here: a file
// being appended reads at its true length to every other program the moment
// the write returns, where FAT holds the length in memory until sync/close
// (the looks-empty-until-sync behavior that forced the sync syscall into
// existence). DIVERGENCES.md records it.
//
// Locking: every op in this file runs whole-body under e->write_lock (the
// doctrine atop ext2.c). Under that lock the CACHED sb/GDT are the source of
// truth and the disk is written back FROM them.

#include "CONFIG.h"
#include "kmalloc.h"
#include "ext2_vfs.h"
#include "ext2_internal.h"
#include "serial_logging.h"
#include "memset.h"
#include "memcpy.h"
#include "memcmp.h"
#include "strings.h"
#include "strings/sprintf.h"   // vsnprintf — ext2_fprintf's formatter
#include "kernel.h"    // kSystemCurrentTime — epoch seconds for timestamps

// ── The scratch pool ────────────────────────────────────────────────────────
// LIFO get/put over the mount's preallocated buffers (rationale + sizing in
// ext2_internal.h). Callers here all hold write_lock, so the pool needs no
// lock of its own. Buffers arrive DIRTY — zero explicitly where it matters.
static uint8_t *wr_scratch_get(ext2_fs_t *e)
{
	int slots = (int)(sizeof(e->wr_scratch) / sizeof(e->wr_scratch[0]));
	if (e->wr_scratch_used < slots)
		return e->wr_scratch[e->wr_scratch_used++];
	// Deeper nesting than the pool funds: fall back to kmalloc, loudly —
	// correctness is preserved, and the printd is the demand signal to grow
	// the pool.
	printd(DEBUG_VFS, "ext2: scratch pool exhausted (%d slots) — kmalloc fallback\n", slots);
	return kmalloc(e->block_size);
}

static void wr_scratch_put(ext2_fs_t *e, uint8_t *buf)
{
	if (buf == NULL)
		return;
	// LIFO discipline: a pool buffer being returned is always the most
	// recently issued one. Anything else is a fallback allocation.
	if (e->wr_scratch_used > 0 && buf == e->wr_scratch[e->wr_scratch_used - 1])
	{
		e->wr_scratch_used--;
		return;
	}
	kfree(buf);
}

// ── Block-level write plumbing ──────────────────────────────────────────────

// The mirror of ext2_read_fs_block: one whole fs block onto the disk.
// Block-ops convention (vfs.h): 0 = success. The stray-write tripwire fires
// inside bops->write if this mount has no business writing — by the time
// execution is HERE, the mount carries a write path, so the tripwire's
// mounted-writable rule passes by construction.
int ext2_write_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                        uint32_t block, const void *buffer)
{
	uint64_t sector = fs->block_device_info->block_device->partition_table
	                      ->parts[fs->partNumber]->partStartSector
	                  + (uint64_t)block * e->sectors_per_block;
	return (int)fs->bops->write(fs->block_device_info, sector, buffer,
	                            e->sectors_per_block);
}

// The universal sub-block primitive: read the block, lay `len` bytes of
// `src` at `offset`, write it back. Every metadata update in this driver is
// a whole-block RMW through here (or write_fs_block for full blocks) — which
// is exactly what makes the lock-free read paths sound: a concurrent reader
// sees the old block or the new block, never a torn one (the NVMe ioLock
// serializes at block granularity).
int ext2_rmw_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                      uint32_t block, uint32_t offset,
                      const void *src, uint32_t len)
{
	if (offset + len > e->block_size)
		return -1;   // caller math error — refuse rather than scribble

	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
		return -1;

	int rc = -1;
	if (ext2_read_fs_block(fs, e, block, buf) == 0)
	{
		memcpy(buf + offset, src, len);
		rc = ext2_write_fs_block(fs, e, block, buf) == 0 ? 0 : -1;
	}
	wr_scratch_put(e, buf);
	return rc;
}

// Write inode `ino` back to its slot in the inode table. Same group/index
// math as ext2_read_inode, and the same 128-byte discipline from the other
// direction: our on-disk inodes are 256 bytes (mkfs's modern default —
// verified on the test image), and ONLY the classic first 128 bytes are
// ours to speak. The RMW touches exactly sizeof(ext2_inode_t) bytes, so the
// extended tail (i_extra_isize, nanosecond timestamps) survives untouched.
int ext2_write_inode_disk(vfs_filesystem_t *fs, ext2_fs_t *e,
                          uint32_t ino, const ext2_inode_t *in)
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

	return ext2_rmw_fs_block(fs, e, block, in_block, in, sizeof(ext2_inode_t));
}

// Write the PRIMARY superblock back from the RAM cache. Backups (the
// sparse_super copies in select groups) are deliberately never touched —
// that's Linux's own runtime behavior; backups belong to mkfs/fsck/resize.
// The superblock lives at byte 1024 of the partition regardless of block
// size (the same special case initialize reads it through), and the write is
// an RMW of its two sectors so any bytes beyond our struct survive.
int ext2_sb_writeback(vfs_filesystem_t *fs, ext2_fs_t *e)
{
	uint8_t *raw = wr_scratch_get(e);
	if (raw == NULL)
		return -1;

	uint64_t part_start = fs->block_device_info->block_device->partition_table
	                          ->parts[fs->partNumber]->partStartSector;

	e->sb.s_wtime = (uint32_t)kSystemCurrentTime;   // best-effort write stamp

	int rc = -1;
	if (fs->bops->read(fs->block_device_info, part_start + 2, raw, 2) == 0)
	{
		memcpy(raw, &e->sb,
		       sizeof(ext2_super_block_t) < 1024 ? sizeof(ext2_super_block_t) : 1024);
		rc = fs->bops->write(fs->block_device_info, part_start + 2, raw, 2) == 0 ? 0 : -1;
	}
	wr_scratch_put(e, raw);
	return rc;
}

// Write back the GDT block holding `group`'s descriptor, sourced from the
// RAM cache (under write_lock the cache IS the truth). Whole-block write:
// the cache was read block-for-block at mount, so neighboring descriptors —
// and any tail bytes past groups_count — go back exactly as they came,
// updated only where this driver changed them.
int ext2_gd_writeback(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t group)
{
	if (group >= e->groups_count)
		return -1;

	uint32_t desc_per_block = e->block_size / sizeof(ext2_group_desc_t);
	uint32_t table_block    = group / desc_per_block;

	// GDT starts in the block after the superblock's block (see the
	// s_first_data_block note in ext2_initialize_filesystem).
	uint32_t disk_block = e->sb.s_first_data_block + 1 + table_block;
	const uint8_t *src  = (const uint8_t *)e->groups + table_block * e->block_size;

	return ext2_write_fs_block(fs, e, disk_block, src) == 0 ? 0 : -1;
}

// ── The allocators ──────────────────────────────────────────────────────────
// Goal-directed, FFS-style: data blocks want the group their inode lives in,
// new inodes want their parent directory's group — locality was the entire
// point of block groups in 1983 and it costs us three lines to honor.
// Bitmap bit b of group g is block s_first_data_block + g*bpg + b (the
// s_first_data_block offset is the classic 1KB-block-size quirk: block 0 is
// the boot record, so data numbering starts at 1).

// Scan one group's block bitmap for a free bit. Returns the absolute block
// number (0 = group full / I/O error) and leaves the bitmap updated on disk,
// counts updated in cache AND on disk. If zero_fill, the block's CONTENT is
// zeroed on disk BEFORE the bitmap marks it used (allocate-then-reference:
// an indirect or directory block must never be reachable in a garbage
// state; a plain data block skips this because its caller writes the real
// content before any reference exists).
static uint32_t ext2_alloc_block_in_group(vfs_filesystem_t *fs, ext2_fs_t *e,
                                          uint32_t g, bool zero_fill)
{
	if (e->groups[g].bg_free_blocks_count == 0)
		return 0;

	// Blocks this group actually covers (the last group is usually partial).
	uint32_t bpg   = e->sb.s_blocks_per_group;
	uint32_t base  = e->sb.s_first_data_block + g * bpg;
	uint32_t span  = e->sb.s_blocks_count - base;
	if (span > bpg)
		span = bpg;

	uint8_t *bm = wr_scratch_get(e);
	if (bm == NULL)
		return 0;
	if (ext2_read_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) != 0)
	{
		wr_scratch_put(e, bm);
		return 0;
	}

	uint32_t bit = (uint32_t)~0u;
	for (uint32_t byte = 0; byte * 8 < span && bit == (uint32_t)~0u; byte++)
	{
		if (bm[byte] == 0xFF)
			continue;
		for (uint32_t b = 0; b < 8; b++)
		{
			uint32_t candidate = byte * 8 + b;
			if (candidate >= span)
				break;
			if (!(bm[byte] & (1u << b)))
			{
				bit = candidate;
				break;
			}
		}
	}
	if (bit == (uint32_t)~0u)
	{
		// bg_free_blocks_count said yes but the bitmap says no — believe the
		// bitmap (it's the on-disk truth) and say so; fsck would flag this.
		wr_scratch_put(e, bm);
		printd(DEBUG_VFS, "ext2: group %u free count/bitmap disagree — treating as full\n", g);
		return 0;
	}

	uint32_t block = base + bit;

	if (zero_fill)
	{
		uint8_t *z = wr_scratch_get(e);
		if (z == NULL)
		{
			wr_scratch_put(e, bm);
			return 0;
		}
		// Scratch arrives DIRTY (pool reuse) — make it the zero block by hand.
		memset(z, 0, e->block_size);
		if (ext2_write_fs_block(fs, e, block, z) != 0)
		{
			wr_scratch_put(e, z); wr_scratch_put(e, bm);
			return 0;
		}
		wr_scratch_put(e, z);
	}

	bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
	if (ext2_write_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) != 0)
	{
		wr_scratch_put(e, bm);
		return 0;
	}
	wr_scratch_put(e, bm);

	e->groups[g].bg_free_blocks_count--;
	e->sb.s_free_blocks_count--;
	ext2_gd_writeback(fs, e, g);
	ext2_sb_writeback(fs, e);
	return block;
}

// Allocate one block, goal group first, then the rest in order.
uint32_t ext2_alloc_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                          uint32_t goal_group, bool zero_fill)
{
	for (uint32_t i = 0; i < e->groups_count; i++)
	{
		uint32_t g = (goal_group + i) % e->groups_count;
		uint32_t block = ext2_alloc_block_in_group(fs, e, g, zero_fill);
		if (block != 0)
			return block;
	}
	printd(DEBUG_VFS, "ext2: out of blocks (%u groups scanned)\n", e->groups_count);
	return 0;   // filesystem full — the caller turns this into a short write
}

// Free one block: bitmap bit off, counts up. The caller has already removed
// every reference (dereference-then-free) — this is the last step, so a
// crash before it leaks the block and a crash after it is a completed free.
int ext2_free_block(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t block)
{
	if (block < e->sb.s_first_data_block || block >= e->sb.s_blocks_count)
		return -1;

	uint32_t rel = block - e->sb.s_first_data_block;
	uint32_t g   = rel / e->sb.s_blocks_per_group;
	uint32_t bit = rel % e->sb.s_blocks_per_group;
	if (g >= e->groups_count)
		return -1;

	uint8_t *bm = wr_scratch_get(e);
	if (bm == NULL)
		return -1;
	int rc = -1;
	if (ext2_read_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) == 0)
	{
		bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
		if (ext2_write_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) == 0)
		{
			e->groups[g].bg_free_blocks_count++;
			e->sb.s_free_blocks_count++;
			ext2_gd_writeback(fs, e, g);
			ext2_sb_writeback(fs, e);
			rc = 0;
		}
	}
	wr_scratch_put(e, bm);
	return rc;
}

// Allocate an inode. Same shape as the block allocator, with the inode
// numbering quirks: inodes are 1-BASED (bit b of group g is inode
// g*inodes_per_group + b + 1), and numbers below s_first_ino (11 on every
// image mkfs makes — the reserved inodes: bad-blocks, root, resize, ...)
// are never handed out even where the bitmap shows them free.
uint32_t ext2_alloc_inode(vfs_filesystem_t *fs, ext2_fs_t *e,
                          uint32_t goal_group, bool is_dir)
{
	uint32_t first_ino = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                         ? e->sb.s_first_ino : EXT2_GOOD_OLD_FIRST_INO;
	uint32_t ipg = e->sb.s_inodes_per_group;

	for (uint32_t i = 0; i < e->groups_count; i++)
	{
		uint32_t g = (goal_group + i) % e->groups_count;
		if (e->groups[g].bg_free_inodes_count == 0)
			continue;

		uint8_t *bm = wr_scratch_get(e);
		if (bm == NULL)
			return 0;
		if (ext2_read_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) != 0)
		{
			wr_scratch_put(e, bm);
			continue;
		}

		for (uint32_t bit = 0; bit < ipg; bit++)
		{
			uint32_t ino = g * ipg + bit + 1;
			if (ino < first_ino || ino > e->sb.s_inodes_count)
				continue;
			if (bm[bit / 8] & (1u << (bit % 8)))
				continue;

			bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
			if (ext2_write_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) != 0)
			{
				wr_scratch_put(e, bm);
				return 0;
			}
			wr_scratch_put(e, bm);

			e->groups[g].bg_free_inodes_count--;
			e->sb.s_free_inodes_count--;
			if (is_dir)
				e->groups[g].bg_used_dirs_count++;
			ext2_gd_writeback(fs, e, g);
			ext2_sb_writeback(fs, e);
			return ino;
		}
		wr_scratch_put(e, bm);
	}
	printd(DEBUG_VFS, "ext2: out of inodes\n");
	return 0;
}

int ext2_free_inode(vfs_filesystem_t *fs, ext2_fs_t *e,
                    uint32_t ino, bool is_dir)
{
	if (ino == 0 || ino > e->sb.s_inodes_count)
		return -1;

	uint32_t ipg = e->sb.s_inodes_per_group;
	uint32_t g   = (ino - 1) / ipg;
	uint32_t bit = (ino - 1) % ipg;
	if (g >= e->groups_count)
		return -1;

	uint8_t *bm = wr_scratch_get(e);
	if (bm == NULL)
		return -1;
	int rc = -1;
	if (ext2_read_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) == 0)
	{
		bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
		if (ext2_write_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) == 0)
		{
			e->groups[g].bg_free_inodes_count++;
			e->sb.s_free_inodes_count++;
			if (is_dir && e->groups[g].bg_used_dirs_count > 0)
				e->groups[g].bg_used_dirs_count--;
			ext2_gd_writeback(fs, e, g);
			ext2_sb_writeback(fs, e);
			rc = 0;
		}
	}
	wr_scratch_put(e, bm);
	return rc;
}

// ── The allocating block map ────────────────────────────────────────────────

// One entry of an indirect block, allocating the child if absent.
// `child_is_indirect` decides zero_fill: a chain block must be a valid
// (all-zero) pointer block on disk BEFORE its parent entry points at it —
// allocate-then-reference at the indirection layer. A data leaf skips the
// zero-fill; its caller (ext2_write) writes real content next, and if the
// content lands as a partial block the fresh flag tells the caller to zero
// the remainder in the same write.
static uint32_t ext2_ind_get_or_alloc(vfs_filesystem_t *fs, ext2_fs_t *e,
                                      uint32_t ind_block, uint32_t idx,
                                      uint32_t goal_group, bool child_is_indirect,
                                      ext2_inode_t *ino, bool *fresh)
{
	if (ind_block == 0 || idx >= e->ptrs_per_block)
		return 0;

	uint32_t *buf = (uint32_t *)wr_scratch_get(e);
	if (buf == NULL)
		return 0;
	if (ext2_read_fs_block(fs, e, ind_block, buf) != 0)
	{
		wr_scratch_put(e, (uint8_t *)buf);
		return 0;
	}

	uint32_t child = buf[idx];
	if (child == 0)
	{
		child = ext2_alloc_block(fs, e, goal_group, child_is_indirect);
		if (child != 0)
		{
			uint32_t entry = child;
			// 4-byte RMW of the parent entry — after the child's content
			// (if indirect) is already valid on disk.
			if (ext2_rmw_fs_block(fs, e, ind_block,
			                      idx * (uint32_t)sizeof(uint32_t),
			                      &entry, sizeof(uint32_t)) != 0)
			{
				// Parent entry didn't land: the child is a leak (bitmap set,
				// nothing references it) — exactly the failure class the
				// ordering doctrine promises. fsck reclaims it.
				child = 0;
			}
			else
			{
				ino->i_blocks += e->sectors_per_block;
				if (fresh != NULL)
					*fresh = true;
			}
		}
	}
	wr_scratch_put(e, (uint8_t *)buf);
	return child;
}

// ext2_bmap's allocating twin: file-relative block index -> on-disk block,
// allocating the data block AND any missing indirect-chain blocks on the
// way. Mutates the IN-MEMORY inode (i_block[], i_blocks); the caller owns
// writing it back — which is what keeps i_block[] and i_size in one atomic
// inode write. `*fresh` reports a data block that has no content yet (the
// partial-write-must-zero-the-rest signal). Returns 0 = allocation failed
// (disk full) or I/O error.
uint32_t ext2_bmap_alloc(vfs_filesystem_t *fs, ext2_fs_t *e,
                         ext2_inode_t *ino, uint32_t fblock,
                         uint32_t goal_group, bool *fresh)
{
	uint32_t ppb = e->ptrs_per_block;
	if (fresh != NULL)
		*fresh = false;

	// Direct blocks: the entry lives in the inode itself, so "write the
	// reference" is deferred to the caller's single inode write-back.
	if (fblock < EXT2_NDIR_BLOCKS)
	{
		if (ino->i_block[fblock] == 0)
		{
			uint32_t b = ext2_alloc_block(fs, e, goal_group, false);
			if (b == 0)
				return 0;
			ino->i_block[fblock] = b;
			ino->i_blocks += e->sectors_per_block;
			if (fresh != NULL)
				*fresh = true;
		}
		return ino->i_block[fblock];
	}
	fblock -= EXT2_NDIR_BLOCKS;

	// Single indirect. The chain root also lives in the inode.
	if (fblock < ppb)
	{
		if (ino->i_block[EXT2_IND_BLOCK] == 0)
		{
			uint32_t ind = ext2_alloc_block(fs, e, goal_group, true);
			if (ind == 0)
				return 0;
			ino->i_block[EXT2_IND_BLOCK] = ind;
			ino->i_blocks += e->sectors_per_block;
		}
		return ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_IND_BLOCK],
		                             fblock, goal_group, false, ino, fresh);
	}
	fblock -= ppb;

	// Double indirect.
	if (fblock < (uint64_t)ppb * ppb)
	{
		if (ino->i_block[EXT2_DIND_BLOCK] == 0)
		{
			uint32_t dind = ext2_alloc_block(fs, e, goal_group, true);
			if (dind == 0)
				return 0;
			ino->i_block[EXT2_DIND_BLOCK] = dind;
			ino->i_blocks += e->sectors_per_block;
		}
		uint32_t l1 = ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_DIND_BLOCK],
		                                    fblock / ppb, goal_group, true, ino, NULL);
		if (l1 == 0)
			return 0;
		return ext2_ind_get_or_alloc(fs, e, l1, fblock % ppb,
		                             goal_group, false, ino, fresh);
	}
	fblock -= ppb * ppb;

	// Triple indirect — same ten lines that bought the read side ~16GB.
	if (ino->i_block[EXT2_TIND_BLOCK] == 0)
	{
		uint32_t tind = ext2_alloc_block(fs, e, goal_group, true);
		if (tind == 0)
			return 0;
		ino->i_block[EXT2_TIND_BLOCK] = tind;
		ino->i_blocks += e->sectors_per_block;
	}
	uint32_t l1 = ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_TIND_BLOCK],
	                                    fblock / (ppb * ppb), goal_group, true, ino, NULL);
	if (l1 == 0)
		return 0;
	uint32_t l2 = ext2_ind_get_or_alloc(fs, e, l1, (fblock / ppb) % ppb,
	                                    goal_group, true, ino, NULL);
	if (l2 == 0)
		return 0;
	return ext2_ind_get_or_alloc(fs, e, l2, fblock % ppb,
	                             goal_group, false, ino, fresh);
}

// ── Truncation ──────────────────────────────────────────────────────────────

// Free every block an (already-dereferenced) block map names: the indirect
// chains bottom-up, then the chain roots, then nothing — the map came to us
// as a COPY made before the inode was rewritten with a zeroed map, so by the
// time these frees run, nothing on disk references any of it
// (dereference-then-free, the doctrine's second rule). depth counts pointer
// levels: 0 = data block, 1..3 = indirect. Recursion is bounded by ext2's
// own geometry — three levels, ever.
static void ext2_free_chain(vfs_filesystem_t *fs, ext2_fs_t *e,
                            uint32_t block, uint32_t depth)
{
	if (block == 0)
		return;

	if (depth > 0)
	{
		uint32_t *buf = (uint32_t *)wr_scratch_get(e);
		if (buf != NULL)
		{
			if (ext2_read_fs_block(fs, e, block, buf) == 0)
				for (uint32_t i = 0; i < e->ptrs_per_block; i++)
					ext2_free_chain(fs, e, buf[i], depth - 1);
			wr_scratch_put(e, (uint8_t *)buf);
		}
		// buf == NULL: the children leak rather than dangle — the map is
		// already severed, so a leak is the worst case by construction.
	}
	ext2_free_block(fs, e, block);
}

// Free all blocks named by `old` (a pre-truncate copy of the inode).
static void ext2_free_inode_blocks(vfs_filesystem_t *fs, ext2_fs_t *e,
                                   const ext2_inode_t *old)
{
	for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++)
		ext2_free_chain(fs, e, old->i_block[i], 0);
	ext2_free_chain(fs, e, old->i_block[EXT2_IND_BLOCK],  1);
	ext2_free_chain(fs, e, old->i_block[EXT2_DIND_BLOCK], 2);
	ext2_free_chain(fs, e, old->i_block[EXT2_TIND_BLOCK], 3);
}

// ── Path splitting and directory surgery ────────────────────────────────────

// Dirent file_type codes (the FILETYPE feature's byte — the namespace the
// header's enum actually belongs to, per the trap note in ext2_internal.h).
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

// Split a canonical fs-local path into (parent directory inode, leaf name).
// Returns the PARENT's inode number (0 = no such parent / not a directory /
// bad leaf name) and fills *parent_out, *name_out (pointing into `path`),
// *len_out. "/x" splits to (root, "x"); refuses "", "/", "." and ".." leaves.
static uint32_t ext2_split_path(vfs_filesystem_t *fs, ext2_fs_t *e,
                                const char *path, ext2_inode_t *parent_out,
                                const char **name_out, uint32_t *len_out)
{
	// Find the last component. Paths arrive canonical (vfs_canonicalize_path
	// ran upstream): absolute, no duplicate or trailing slashes.
	const char *leaf = NULL;
	for (const char *p = path; *p != '\0'; p++)
		if (*p == '/')
			leaf = p + 1;
	if (leaf == NULL || *leaf == '\0')
		return 0;   // "" or "/" — no leaf to speak of

	uint32_t len = 0;
	while (leaf[len] != '\0')
		len++;
	if (len > EXT2_NAME_LEN)
		return 0;
	if ((len == 1 && leaf[0] == '.') ||
	    (len == 2 && leaf[0] == '.' && leaf[1] == '.'))
		return 0;   // navigation artifacts are not creatable/removable names

	// Resolve the parent: the path up to (not including) the leaf, which for
	// a top-level name is just "/".
	size_t parent_len = (size_t)(leaf - path);   // includes the trailing '/'
	char *parent_path = kmalloc(parent_len + 2);
	if (parent_path == NULL)
		return 0;
	if (parent_len <= 1)
	{
		parent_path[0] = '/';
		parent_path[1] = '\0';
	}
	else
	{
		memcpy(parent_path, path, parent_len - 1);   // drop the trailing '/'
		parent_path[parent_len - 1] = '\0';
	}

	uint32_t parent_ino = ext2_resolve_path(fs, e, parent_path, parent_out);
	kfree(parent_path);

	if (parent_ino == 0 ||
	    (parent_out->i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
		return 0;

	*name_out = leaf;
	*len_out = len;
	return parent_ino;
}

// Insert (name -> child_ino) into the directory `dir` (inode number
// dir_ino). The classic rec_len dance: every dirent's rec_len reaches to
// the next entry (the last one reaches the block end), so free space lives
// as SLACK inside entries. A new entry either claims a dead entry
// (inode == 0) whose rec_len fits, or splits a live entry's slack. If no
// block has room, the directory grows by one block whose single entry
// covers it entirely. Updates and WRITES BACK the dir inode when it grows
// (mtime rides along either way — caller need not re-write it).
//
// Ordering: an in-place insert is one whole-block RMW (a reader sees the
// entry absent or complete, never partial). Growth writes the new block's
// content first, then the dir inode that references it.
static int ext2_dir_insert(vfs_filesystem_t *fs, ext2_fs_t *e,
                           uint32_t dir_ino, ext2_inode_t *dir,
                           const char *name, uint32_t len,
                           uint32_t child_ino, uint8_t file_type)
{
	// The file_type byte is only meaningful under the FILETYPE feature;
	// without it the byte is reserved-zero. (Our incompat gate admits only
	// FILETYPE, so this is the entire decision space.)
	uint32_t incompat = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                        ? e->sb.s_feature_incompat : 0;
	if (!(incompat & EXT2_FEATURE_INCOMPAT_FILETYPE))
		file_type = 0;

	uint16_t needed = (uint16_t)EXT2_DIR_REC_LEN(len);

	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
		return -1;

	uint32_t dir_blocks = dir->i_size / e->block_size;
	for (uint32_t fb = 0; fb < dir_blocks; fb++)
	{
		uint32_t disk_block = ext2_bmap(fs, e, dir, fb);
		if (disk_block == 0)
			continue;   // hole in a directory — nothing to split there
		if (ext2_read_fs_block(fs, e, disk_block, buf) != 0)
			break;

		uint32_t pos = 0;
		while (pos < e->block_size)
		{
			ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(buf + pos);
			if (de->rec_len == 0)
				break;   // corrupt block — try the next one

			if (de->inode == 0 && de->rec_len >= needed)
			{
				// A dead entry big enough: claim it in place, keeping its
				// rec_len (it already covers this span of the block).
				de->inode = child_ino;
				de->name_len = (uint8_t)len;
				de->file_type = file_type;
				memcpy(de->name, name, len);
				int rc = ext2_write_fs_block(fs, e, disk_block, buf) == 0 ? 0 : -1;
				if (rc == 0)
				{
					dir->i_mtime = (uint32_t)kSystemCurrentTime;
					rc = ext2_write_inode_disk(fs, e, dir_ino, dir);
				}
				wr_scratch_put(e, buf);
				return rc;
			}

			// A live entry's slack: the gap between what it needs and what
			// its rec_len covers.
			uint16_t ideal = (uint16_t)EXT2_DIR_REC_LEN(de->name_len);
			if (de->inode != 0 && de->rec_len >= ideal + needed)
			{
				uint16_t old_rec = de->rec_len;
				de->rec_len = ideal;
				ext2_dir_entry_2_t *ne = (ext2_dir_entry_2_t *)(buf + pos + ideal);
				ne->inode = child_ino;
				ne->rec_len = old_rec - ideal;   // inherit the reach
				ne->name_len = (uint8_t)len;
				ne->file_type = file_type;
				memcpy(ne->name, name, len);
				int rc = ext2_write_fs_block(fs, e, disk_block, buf) == 0 ? 0 : -1;
				if (rc == 0)
				{
					dir->i_mtime = (uint32_t)kSystemCurrentTime;
					rc = ext2_write_inode_disk(fs, e, dir_ino, dir);
				}
				wr_scratch_put(e, buf);
				return rc;
			}

			pos += de->rec_len;
		}
	}

	// No room anywhere: grow the directory by one block. Its single entry
	// covers the whole block (the empty-slack invariant, degenerate case).
	uint32_t goal_group = (dir_ino - 1) / e->sb.s_inodes_per_group;
	bool fresh = false;
	uint32_t nb = ext2_bmap_alloc(fs, e, dir, dir_blocks, goal_group, &fresh);
	if (nb == 0)
	{
		wr_scratch_put(e, buf);
		return -1;
	}
	memset(buf, 0, e->block_size);
	ext2_dir_entry_2_t *ne = (ext2_dir_entry_2_t *)buf;
	ne->inode = child_ino;
	ne->rec_len = (uint16_t)e->block_size;
	ne->name_len = (uint8_t)len;
	ne->file_type = file_type;
	memcpy(ne->name, name, len);
	if (ext2_write_fs_block(fs, e, nb, buf) != 0)
	{
		wr_scratch_put(e, buf);
		return -1;
	}
	wr_scratch_put(e, buf);

	// Content is down; NOW the reference — the dir inode with its bigger
	// size and (via bmap_alloc's mutation) the new block in its map.
	dir->i_size += e->block_size;
	dir->i_mtime = (uint32_t)kSystemCurrentTime;
	return ext2_write_inode_disk(fs, e, dir_ino, dir);
}

// ── File creation ───────────────────────────────────────────────────────────

// Create an empty regular file at `path`. Caller holds write_lock.
// Returns the new inode number, or 0. Ordering: fully-initialized child
// inode first (its bitmap bit was set by the allocator), parent dirent LAST
// — a crash in between leaks an inode e2fsck reclaims, and no reader ever
// finds a name pointing at an uninitialized inode.
static uint32_t ext2_create_file(vfs_filesystem_t *fs, ext2_fs_t *e,
                                 const char *path)
{
	ext2_inode_t parent;
	const char *name;
	uint32_t len;
	uint32_t parent_ino = ext2_split_path(fs, e, path, &parent, &name, &len);
	if (parent_ino == 0)
		return 0;

	// Name already taken? (The open path checks existence first, but mkdir
	// and future callers come through here too — cheap and load-bearing.)
	if (ext2_dir_find(fs, e, &parent, name, len) != 0)
		return 0;

	uint32_t goal_group = (parent_ino - 1) / e->sb.s_inodes_per_group;
	uint32_t ino = ext2_alloc_inode(fs, e, goal_group, false);
	if (ino == 0)
		return 0;

	ext2_inode_t node;
	memset(&node, 0, sizeof(node));
	node.i_mode = EXT2_S_IFREG | 0644;
	node.i_links_count = 1;
	node.i_atime = node.i_ctime = node.i_mtime = (uint32_t)kSystemCurrentTime;

	if (ext2_write_inode_disk(fs, e, ino, &node) != 0)
	{
		// The inode never became real: give the number back.
		ext2_free_inode(fs, e, ino, false);
		return 0;
	}

	if (ext2_dir_insert(fs, e, parent_ino, &parent, name, len,
	                    ino, EXT2_FT_REG_FILE) != 0)
	{
		ext2_free_inode(fs, e, ino, false);
		return 0;
	}
	return ino;
}

// ── write / sync / fputs / fprintf ──────────────────────────────────────────

// The 2GB-1 growth cap: classic ext2 keeps a regular file's size in a
// 32-bit field read as signed by half the world's tools; past it lies
// RO_COMPAT_LARGE_FILE bookkeeping we deliberately don't maintain (see
// ext2_internal.h). The day os64 wants a >2GB file, this constant and the
// i_dir_acl handling around it are the whole shopping list.
#define EXT2_MAX_FILE_SIZE 0x7FFFFFFFu

static int ext2_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_file->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_file->owner;
	ext2_fs_t *e = (ext2_fs_t *)fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);

	// FRESH-INODE DISCIPLINE: re-read the inode from disk before mutating.
	// Two handles appending to one file serialize here — each sees the
	// other's committed size and block map instead of clobbering it with a
	// stale copy (last-writer-wins would leak the loser's blocks and hand
	// e2fsck a bitmap/i_blocks disagreement). Through mount scratch — we
	// hold the lock, no need to pay the read paths' per-call kmalloc.
	{
		uint8_t *ibuf = wr_scratch_get(e);
		int irc = (ibuf != NULL) ? ext2_read_inode_buf(fs, e, h->ino, &h->inode, ibuf) : -1;
		wr_scratch_put(e, ibuf);
		if (irc != 0)
		{
			spinlock_release_irqrestore(&e->write_lock, lock_flags);
			return -1;
		}
	}
	h->size = h->inode.i_size;

	// A LARGE_FILE file (size high bits in i_dir_acl) is refused whole — we
	// don't maintain that bookkeeping, so we don't touch its inode.
	if (h->inode.i_dir_acl != 0)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		printd(DEBUG_VFS, "ext2: inode %u is a >2GB (large_file) file — refusing write\n", h->ino);
		return -1;
	}

	// Clamp to the growth cap; writing SOME bytes and reporting a short
	// count is the "device full" shape the syscall layer already speaks.
	if (h->pos >= EXT2_MAX_FILE_SIZE)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}
	if (size > EXT2_MAX_FILE_SIZE - h->pos)
		size = EXT2_MAX_FILE_SIZE - h->pos;

	// Data blocks want the inode's own group — FFS locality.
	uint32_t goal_group = (h->ino - 1) / e->sb.s_inodes_per_group;

	uint8_t *scratch = wr_scratch_get(e);
	if (scratch == NULL)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

	size_t done = 0;
	bool io_error = false;
	while (done < size)
	{
		uint32_t fblock   = (uint32_t)(h->pos / e->block_size);
		uint32_t in_block = (uint32_t)(h->pos % e->block_size);
		size_t   chunk    = e->block_size - in_block;
		if (chunk > size - done)
			chunk = size - done;

		bool fresh = false;
		uint32_t disk_block = ext2_bmap_alloc(fs, e, &h->inode, fblock,
		                                      goal_group, &fresh);
		if (disk_block == 0)
			break;   // disk full (or chain I/O error): report the short count

		int rc;
		if (chunk == e->block_size)
		{
			// Whole block: no read needed, old content irrelevant.
			rc = ext2_write_fs_block(fs, e, disk_block, (const uint8_t *)buffer + done);
		}
		else if (fresh)
		{
			// Partial write into a block that has NO content yet: the rest
			// of the block must be zeros, not whatever the previous owner
			// left there. (This is both the hole-fill contract — holes read
			// as zeros — and a don't-leak-freed-data rule.) kmalloc's
			// zeroing gives us the zero canvas.
			memset(scratch, 0, e->block_size);
			memcpy(scratch + in_block, (const uint8_t *)buffer + done, chunk);
			rc = ext2_write_fs_block(fs, e, disk_block, scratch);
		}
		else
		{
			rc = ext2_rmw_fs_block(fs, e, disk_block, in_block,
			                       (const uint8_t *)buffer + done, chunk);
		}
		if (rc != 0)
		{
			io_error = true;
			break;
		}

		done += chunk;
		h->pos += chunk;
	}
	wr_scratch_put(e, scratch);

	// WRITE-THROUGH: commit the inode now — size, mtime, and every block
	// pointer bmap_alloc added, in ONE 128-byte write (i_size never outruns
	// i_block[]). Committed even on the short-write path so the bytes that
	// DID land are durable and accounted.
	if (done > 0 || h->inode.i_blocks != 0)
	{
		if (h->pos > h->inode.i_size)
			h->inode.i_size = (uint32_t)h->pos;
		h->inode.i_mtime = (uint32_t)kSystemCurrentTime;
		if (ext2_write_inode_disk(fs, e, h->ino, &h->inode) != 0)
			io_error = true;
		h->size = h->inode.i_size;
	}

	spinlock_release_irqrestore(&e->write_lock, lock_flags);

	if (done == 0)
		return io_error ? -1 : 0;
	return (int)done;
}

// WRITE-THROUGH makes sync a completed promise, not a pending one: by the
// time any write returns, its data, metadata, and inode are already on the
// device. Contrast fat_sync, which exists precisely because FatFs defers
// the length to sync/close. (DIVERGENCES.md records the difference.)
static int ext2_sync(vfs_file_t *vfs_file)
{
	(void)vfs_file;
	return 0;
}

static int ext2_fputs(vfs_file_t *vfs_file, char *buffer)
{
	return ext2_write(vfs_file, buffer, strlen(buffer));
}

// Format-then-write, mirroring fat_fprintf's division of labor: the
// formatting happens HERE into a bounded buffer, and the disk only ever
// sees a plain write.
#define EXT2_FPRINTF_MAX 512

static int ext2_fprintf(vfs_file_t *vfs_file, const char *fmt, ...)
{
	char buffer[EXT2_FPRINTF_MAX];
	va_list args;

	va_start(args, fmt);
	int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (len < 0)
		return -1;
	return ext2_write(vfs_file, buffer, (size_t)len);
}

// ── The writable open ───────────────────────────────────────────────────────
// The full mode vocabulary, matching the FAT glue's semantics exactly:
//   "r"      — existing file, read (delegates to the shared open core)
//   "a"      — append: open at end, CREATING the file if absent
//   "w"/"c"  — create-or-truncate ("c" predates "w" and is byte-identical;
//              kept for existing callers, same as FAT)
// Creation and truncation mutate the disk, so those arms run under
// write_lock; the handle itself is built by the same shared core every open
// uses, AFTER the lock drops (the core takes no locks — it's the read path).

// The truncate arm, factored for clarity. Caller holds write_lock.
static int ext2_truncate_locked(vfs_filesystem_t *fs, ext2_fs_t *e,
                                uint32_t ino, ext2_inode_t *node)
{
	// Refuse to truncate what another handle holds open (ruling 5) — the
	// reader would watch its file's blocks recycle under it. And refuse
	// LARGE_FILE files whole, same as write.
	if (ext2_openref_count(e, ino) > 0)
	{
		printd(DEBUG_VFS, "ext2: refusing truncate of inode %u — open elsewhere (busy)\n", ino);
		return -1;
	}
	if (node->i_dir_acl != 0)
		return -1;

	// Dereference-then-free: sever the map on disk FIRST (zeroed i_block[],
	// size 0, i_blocks 0 in one inode write), THEN free the blocks from the
	// pre-severance copy. A crash between the two leaks blocks; it can
	// never leave the inode pointing at freed ones.
	ext2_inode_t old = *node;
	memset(node->i_block, 0, sizeof(node->i_block));
	node->i_size = 0;
	node->i_blocks = 0;
	node->i_mtime = (uint32_t)kSystemCurrentTime;
	if (ext2_write_inode_disk(fs, e, ino, node) != 0)
		return -1;
	ext2_free_inode_blocks(fs, e, &old);
	return 0;
}

static int ext2_open_rw(vfs_file_t **vfs_file, const char *path, const char *mode,
                        vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	if (mode == NULL || mode[0] == '\0' || mode[1] != '\0')
		return -1;

	switch (mode[0])
	{
		case 'r':
			return ext2_open_existing(vfs_file, path, vfs_fs);

		case 'a':
		{
			if (ext2_open_existing(vfs_file, path, vfs_fs) != 0)
			{
				// Absent: create it, then open the now-existing file. (A
				// concurrent rm between the unlock and the reopen makes the
				// reopen fail — correct, just unlucky.)
				uint64_t flags = spinlock_acquire_irqsave(&e->write_lock);
				uint32_t ino = ext2_create_file(vfs_fs, e, path);
				spinlock_release_irqrestore(&e->write_lock, flags);
				if (ino == 0)
					return -1;
				if (ext2_open_existing(vfs_file, path, vfs_fs) != 0)
					return -1;
			}
			ext2_handle_t *h = (ext2_handle_t *)(*vfs_file)->handle;
			h->pos = h->size;
			return 0;
		}

		case 'w':
		case 'c':
		{
			uint64_t flags = spinlock_acquire_irqsave(&e->write_lock);

			ext2_inode_t node;
			uint32_t ino = ext2_resolve_path(vfs_fs, e, path, &node);
			if (ino != 0)
			{
				// Exists: must be a regular file, and truncation must be
				// permitted (not open elsewhere, not a large file).
				if ((node.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
				    ext2_truncate_locked(vfs_fs, e, ino, &node) != 0)
				{
					spinlock_release_irqrestore(&e->write_lock, flags);
					return -1;
				}
			}
			else
			{
				ino = ext2_create_file(vfs_fs, e, path);
				if (ino == 0)
				{
					spinlock_release_irqrestore(&e->write_lock, flags);
					return -1;
				}
			}
			spinlock_release_irqrestore(&e->write_lock, flags);

			// Between the unlock and this open, a concurrent "r" open can
			// slip in and register first — benign: it opens the same
			// (empty) file we just made. The refusal above was about
			// truncating a file somebody ALREADY held.
			return ext2_open_existing(vfs_file, path, vfs_fs);
		}

		default:
			return -1;
	}
}

// ── Removal: the one verb ───────────────────────────────────────────────────

// Remove `name` from directory `dir` (inode dir_ino): the rec_len merge.
// An entry is erased by folding its span into the SAME-BLOCK predecessor's
// rec_len; the first entry of a block has no predecessor, so it dies by
// inode = 0 instead (its rec_len keeps covering the span — the dead-entry
// shape ext2_dir_insert knows how to reclaim). One whole-block RMW either
// way: a concurrent reader sees the entry present or absent, never half.
// Returns the removed entry's inode number, or 0.
static uint32_t ext2_dir_remove(vfs_filesystem_t *fs, ext2_fs_t *e,
                                uint32_t dir_ino, ext2_inode_t *dir,
                                const char *name, uint32_t len)
{
	(void)dir_ino;
	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
		return 0;

	uint32_t removed = 0;
	uint32_t dir_blocks = dir->i_size / e->block_size;
	for (uint32_t fb = 0; fb < dir_blocks && removed == 0; fb++)
	{
		uint32_t disk_block = ext2_bmap(fs, e, dir, fb);
		if (disk_block == 0)
			continue;
		if (ext2_read_fs_block(fs, e, disk_block, buf) != 0)
			break;

		ext2_dir_entry_2_t *prev = NULL;
		uint32_t pos = 0;
		while (pos < e->block_size)
		{
			ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(buf + pos);
			if (de->rec_len == 0)
				break;
			if (de->inode != 0 && de->name_len == len &&
			    memcmp(de->name, name, len) == 0)
			{
				removed = de->inode;
				if (prev != NULL)
					prev->rec_len += de->rec_len;   // fold into predecessor
				else
					de->inode = 0;                  // first-in-block: tombstone
				if (ext2_write_fs_block(fs, e, disk_block, buf) != 0)
					removed = 0;
				break;
			}
			prev = de;
			pos += de->rec_len;
		}
	}
	wr_scratch_put(e, buf);
	return removed;
}

// Is this directory empty? Only "." and ".." may hold live inodes.
static bool ext2_dir_is_empty(vfs_filesystem_t *fs, ext2_fs_t *e,
                              const ext2_inode_t *dir)
{
	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
		return false;   // can't prove it empty — refuse the removal

	bool empty = true;
	uint32_t dir_blocks = dir->i_size / e->block_size;
	for (uint32_t fb = 0; fb < dir_blocks && empty; fb++)
	{
		uint32_t disk_block = ext2_bmap(fs, e, (ext2_inode_t *)dir, fb);
		if (disk_block == 0)
			continue;
		if (ext2_read_fs_block(fs, e, disk_block, buf) != 0)
		{
			empty = false;
			break;
		}
		uint32_t pos = 0;
		while (pos < e->block_size)
		{
			ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(buf + pos);
			if (de->rec_len == 0)
				break;
			if (de->inode != 0 &&
			    !(de->name_len == 1 && de->name[0] == '.') &&
			    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))
			{
				empty = false;
				break;
			}
			pos += de->rec_len;
		}
	}
	wr_scratch_put(e, buf);
	return empty;
}

// os64's ONE removal verb (fops->rm): a file OR an empty directory. The
// ABI comment over SYSCALL_UNLINK promised this driver would be built to
// that contract the morning it was ratified; here it is, same afternoon.
// (FatFs's f_unlink grants the same shape natively — see fat_rm.)
static int ext2_rm(const char *filename, vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);

	// Resolve parent + leaf, then the child through the parent — we need
	// all three for the dirent surgery.
	ext2_inode_t parent;
	const char *name;
	uint32_t len;
	uint32_t parent_ino = ext2_split_path(vfs_fs, e, filename, &parent, &name, &len);
	if (parent_ino == 0)
		goto refuse;

	uint32_t child_ino = ext2_dir_find(vfs_fs, e, &parent, name, len);
	if (child_ino == 0)
		goto refuse;   // no such entry

	ext2_inode_t child;
	if (ext2_read_inode(vfs_fs, e, child_ino, &child) != 0)
		goto refuse;

	// Ruling 5: refuse while open — a file being read, a directory being
	// listed. The refusal is the whole design; there is no unlink-while-open
	// deferral until a consumer demands one.
	if (ext2_openref_count(e, child_ino) > 0)
	{
		printd(DEBUG_VFS, "ext2: refusing rm of '%s' (inode %u) — open elsewhere (busy)\n",
		       name, child_ino);
		goto refuse;
	}

	uint32_t kind = child.i_mode & EXT2_S_IFMT;
	if (kind == EXT2_S_IFDIR)
	{
		if (!ext2_dir_is_empty(vfs_fs, e, &child))
			goto refuse;   // one verb, but only for EMPTY directories

		// Dereference-then-free, directory edition: name out of the parent,
		// parent's ".."-backlink count down, THEN the child's teardown.
		if (ext2_dir_remove(vfs_fs, e, parent_ino, &parent, name, len) == 0)
			goto refuse;
		if (parent.i_links_count > 0)
			parent.i_links_count--;   // the child's ".." no longer points here
		parent.i_mtime = (uint32_t)kSystemCurrentTime;
		ext2_write_inode_disk(vfs_fs, e, parent_ino, &parent);

		ext2_inode_t old = child;
		memset(child.i_block, 0, sizeof(child.i_block));
		child.i_links_count = 0;
		child.i_size = 0;
		child.i_blocks = 0;
		child.i_dtime = (uint32_t)kSystemCurrentTime;
		ext2_write_inode_disk(vfs_fs, e, child_ino, &child);
		ext2_free_inode_blocks(vfs_fs, e, &old);
		ext2_free_inode(vfs_fs, e, child_ino, true);
	}
	else if (kind == EXT2_S_IFREG)
	{
		if (ext2_dir_remove(vfs_fs, e, parent_ino, &parent, name, len) == 0)
			goto refuse;
		parent.i_mtime = (uint32_t)kSystemCurrentTime;
		ext2_write_inode_disk(vfs_fs, e, parent_ino, &parent);

		// Respect a link count > 1 even though os64 can't create hard links
		// yet — the DISK can carry them (debugfs, another OS), and freeing
		// a still-linked inode is corruption by another name.
		if (child.i_links_count > 1)
		{
			child.i_links_count--;
			child.i_ctime = (uint32_t)kSystemCurrentTime;
			ext2_write_inode_disk(vfs_fs, e, child_ino, &child);
		}
		else
		{
			ext2_inode_t old = child;
			memset(child.i_block, 0, sizeof(child.i_block));
			child.i_links_count = 0;
			child.i_size = 0;
			child.i_blocks = 0;
			child.i_dtime = (uint32_t)kSystemCurrentTime;
			ext2_write_inode_disk(vfs_fs, e, child_ino, &child);
			ext2_free_inode_blocks(vfs_fs, e, &old);
			ext2_free_inode(vfs_fs, e, child_ino, false);
		}
	}
	else
	{
		// Symlinks, devices, sockets: NOT ours to tear down. A fast symlink
		// keeps its target STRING inside i_block[] — "freeing" those words
		// as block numbers would shred the allocator. Refuse until a slice
		// deliberately learns their storage rules.
		printd(DEBUG_VFS, "ext2: refusing rm of '%s' — inode %u is neither file nor directory (mode 0x%04x)\n",
		       name, child_ino, child.i_mode);
		goto refuse;
	}

	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return 0;

refuse:
	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return -1;
}

// ── mkdir ───────────────────────────────────────────────────────────────────

// One atomic call from the caller's view (what took Unix until 4.2BSD).
// Ordering inside: the "."/".." block's content first, then the child inode
// (its bitmap bit is already set by the allocator), parent dirent LAST —
// with the parent's links_count bump riding the same parent-inode write
// dir_insert already does.
static int ext2_mkdir(char *path, vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);

	ext2_inode_t parent;
	const char *name;
	uint32_t len;
	uint32_t parent_ino = ext2_split_path(vfs_fs, e, path, &parent, &name, &len);
	if (parent_ino == 0)
		goto refuse;
	if (ext2_dir_find(vfs_fs, e, &parent, name, len) != 0)
		goto refuse;   // name taken (file or dir — either way, no)

	uint32_t goal_group = (parent_ino - 1) / e->sb.s_inodes_per_group;
	uint32_t ino = ext2_alloc_inode(vfs_fs, e, goal_group, true);
	if (ino == 0)
		goto refuse;

	// The directory's one data block: "." (rec_len 12 — exactly
	// EXT2_DIR_REC_LEN(1)) then ".." reaching to the block end.
	uint32_t block = ext2_alloc_block(vfs_fs, e, goal_group, false);
	if (block == 0)
	{
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}
	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
	{
		ext2_free_block(vfs_fs, e, block);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}
	// Scratch arrives DIRTY (pool reuse) — zero the canvas before building
	// the two entries on it.
	memset(buf, 0, e->block_size);
	ext2_dir_entry_2_t *dot = (ext2_dir_entry_2_t *)buf;
	dot->inode = ino;
	dot->rec_len = (uint16_t)EXT2_DIR_REC_LEN(1);
	dot->name_len = 1;
	dot->file_type = EXT2_FT_DIR;
	dot->name[0] = '.';
	ext2_dir_entry_2_t *dotdot = (ext2_dir_entry_2_t *)(buf + dot->rec_len);
	dotdot->inode = parent_ino;
	dotdot->rec_len = (uint16_t)(e->block_size - dot->rec_len);
	dotdot->name_len = 2;
	dotdot->file_type = EXT2_FT_DIR;
	dotdot->name[0] = '.';
	dotdot->name[1] = '.';

	int rc = ext2_write_fs_block(vfs_fs, e, block, buf);
	wr_scratch_put(e, buf);
	if (rc != 0)
	{
		ext2_free_block(vfs_fs, e, block);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}

	// The child inode: links_count 2 is "." plus the parent's entry — the
	// classic identity every fsck recomputes (a dir's links = 2 + number of
	// subdirectories, seen from the other side).
	ext2_inode_t node;
	memset(&node, 0, sizeof(node));
	node.i_mode = EXT2_S_IFDIR | 0755;
	node.i_links_count = 2;
	node.i_size = e->block_size;
	node.i_blocks = e->sectors_per_block;
	node.i_block[0] = block;
	node.i_atime = node.i_ctime = node.i_mtime = (uint32_t)kSystemCurrentTime;
	if (ext2_write_inode_disk(vfs_fs, e, ino, &node) != 0)
	{
		ext2_free_block(vfs_fs, e, block);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}

	// Parent last: its links bump (the new "..") rides dir_insert's
	// parent-inode write.
	parent.i_links_count++;
	if (ext2_dir_insert(vfs_fs, e, parent_ino, &parent, name, len,
	                    ino, EXT2_FT_DIR) != 0)
	{
		ext2_free_block(vfs_fs, e, block);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}

	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return 0;

refuse:
	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return -1;
}

// ── The write-capable op tables ─────────────────────────────────────────────
// The RW pair: every read slot aliases the read half's functions (via the
// RO tables — same function pointers, one implementation); the write slots
// are this file's. A mount registered with this pair answers the tripwire's
// mounted-writable question YES — which is the entire difference between
// the root (RO pair, still guarded) and /ext2 (this pair, allowed the pen).
vfs_file_operations_t ext2_rw_fops;
vfs_directory_operations_t ext2_rw_dops;

// Populated from the RO tables + this file's write slots. Called by the
// mount wiring in vfs.c (vfs_mount_secondary_partitions) before
// kRegisterFilesystem memcpy's the tables into the mount — C static
// initialization can't alias another table's members, so a tiny constructor
// does it. Idempotent; call it any number of times.
void ext2_rw_tables_init(void)
{
	ext2_rw_fops = ext2_fops;            // read slots: one implementation
	ext2_rw_fops.open    = ext2_open_rw; // the full mode vocabulary
	ext2_rw_fops.write   = ext2_write;
	ext2_rw_fops.sync    = ext2_sync;
	ext2_rw_fops.fputs   = ext2_fputs;
	ext2_rw_fops.fprintf = ext2_fprintf;
	ext2_rw_fops.rm      = ext2_rm;      // the one removal verb

	ext2_rw_dops = ext2_dops;
	ext2_rw_dops.mkdir   = ext2_mkdir;
}
