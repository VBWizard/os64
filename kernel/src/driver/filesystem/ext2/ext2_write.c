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
// WITHIN one op the disk traffic is BATCHED (ext2_batch_t, above the
// allocator): a write() call's data blocks go to the device in contiguous
// runs, its bitmap/descriptor/superblock updates land once per group it
// touched, and the indirect leaf collecting its block pointers is written
// once — instead of four commands per 4KB block, which is what put a
// write-through filesystem at 1.7 MB/s on an NVMe drive. A release
// (truncate, an unlinked file's last close) clears its bits the same way:
// each group published once, each parent indirect block written once after
// its subtree. The ordering doctrine above is kept at every flush point;
// only the repetition went.
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

// ── EXT2_ALARM: the operator's line, and who gets to hear it ────────────────
// Every message this file sends to the SCREEN says some version of "this
// filesystem is now ambiguous and I am taking the pens away". That belongs on
// the glass — it is the loudest thing os64 can say about a disk short of a
// panic, and a human needs to see it happen.
//
// It also belongs on the glass only when it is TRUE. The fault-injection
// tests drive these exact paths on a cloned mount every single boot, and five
// read-only demotion warnings under a green 27/0 test run taught exactly the
// wrong lesson: that the scariest line os64 prints is background noise. (It
// worked, too — they were ignored for days before Chris asked what they were.)
//
// So the audience is chosen, never the message: log during a drill, glass for
// real. One macro rather than nine hand-written if/elses, because this is a
// mechanical repeat and a divergent copy is how one site quietly keeps
// shouting. kTestingExpectedNoise lives in vfs.h with the fuller story.
//
// DEBUG_TESTS, and the category is load-bearing — NOT DEBUG_VFS, which was
// the obvious first choice and was WRONG. printd's filter is
// `(kDebugLevel & flags) != flags`: ALL requested bits must be lit, not any.
// DEBUG_VFS (bit 13) is not in the default level, so routing a drill line
// there would have SUPPRESSED it on an ordinary boot — the exact thing this
// macro promises not to do. DEBUG_TESTS (bit 19) IS on by default, and is
// also the truer label: the line exists because a test induced it.
#define EXT2_ALARM(...) do {                       \
        if (kTestingExpectedNoise)                 \
            printd(DEBUG_TESTS, __VA_ARGS__);      \
        else                                       \
            printf(__VA_ARGS__);                   \
    } while (0)

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

// Count zero bits in the meaningful part of an allocation bitmap. Bits past
// the final partial group are filesystem padding, not free objects.
static uint32_t ext2_bitmap_free_count(const uint8_t *bitmap, uint32_t bits)
{
	uint32_t free = 0;
	for (uint32_t bit = 0; bit < bits; bit++)
		if (!(bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))))
			free++;
	return free;
}

// An idempotent replay can find the bitmap bit already clear after a crash
// interrupted the following GDT/superblock writes. Rebuild this group's count
// from the authoritative bitmap, rebuild the global total from the corrected
// group table, and ALWAYS write both ledgers: RAM may already contain the
// right values after a transient I/O failure while disk still does not.
static int ext2_reconcile_free_block_counts(vfs_filesystem_t *fs, ext2_fs_t *e,
                                             uint32_t g, const uint8_t *bitmap)
{
	uint32_t base = e->sb.s_first_data_block + g * e->sb.s_blocks_per_group;
	uint32_t span = e->sb.s_blocks_count - base;
	if (span > e->sb.s_blocks_per_group)
		span = e->sb.s_blocks_per_group;
	e->groups[g].bg_free_blocks_count = (uint16_t)ext2_bitmap_free_count(bitmap, span);

	uint64_t total = 0;
	for (uint32_t i = 0; i < e->groups_count; i++)
		total += e->groups[i].bg_free_blocks_count;
	e->sb.s_free_blocks_count = (uint32_t)total;

	if (ext2_gd_writeback(fs, e, g) != 0)
		return -1;
	return ext2_sb_writeback(fs, e);
}

static int ext2_reconcile_free_inode_counts(vfs_filesystem_t *fs, ext2_fs_t *e,
                                             uint32_t g, const uint8_t *bitmap)
{
	uint32_t base = g * e->sb.s_inodes_per_group;
	uint32_t span = e->sb.s_inodes_count - base;
	if (span > e->sb.s_inodes_per_group)
		span = e->sb.s_inodes_per_group;
	e->groups[g].bg_free_inodes_count = (uint16_t)ext2_bitmap_free_count(bitmap, span);

	uint64_t total = 0;
	for (uint32_t i = 0; i < e->groups_count; i++)
		total += e->groups[i].bg_free_inodes_count;
	e->sb.s_free_inodes_count = (uint32_t)total;

	if (ext2_gd_writeback(fs, e, g) != 0)
		return -1;
	return ext2_sb_writeback(fs, e);
}

// ── The allocators ──────────────────────────────────────────────────────────
// Goal-directed, FFS-style: data blocks want the group their inode lives in,
// new inodes want their parent directory's group — locality was the entire
// point of block groups in 1983 and it costs us three lines to honor.
// Bitmap bit b of group g is block s_first_data_block + g*bpg + b (the
// s_first_data_block offset is the classic 1KB-block-size quirk: block 0 is
// the boot record, so data numbering starts at 1).

// ── The free path's result vocabulary ───────────────────────────────────────
// A failed release is not one fact but two, and the difference decides whether
// this mount may keep allocating:
//
//   EXT2_FREE_FAILED       nothing on disk changed. The retry map names
//                          storage the bitmap still calls USED, so no
//                          allocator can hand it to a live file; replay simply
//                          tries again at the next mount. Recoverable, quiet.
//
//   ..._NAMES_FREE_BLOCK   the dangerous half. A bitmap bit is already CLEAR —
//   ..._NAMES_FREE_INODE   that block or inode is allocatable RIGHT NOW — while
//                          a durable record still names it for release. Let one
//                          allocation land in that window and replay hands a
//                          live file's storage back to the free pool.
//
// The reusable results carry a caller obligation, in this order: persist the
// retry record FIRST (writes are still permitted), THEN demote the mount.
// Demoting first turns the record's own write into a block-tripwire panic and
// costs the very recovery path the record exists to provide.
enum
{
	EXT2_FREE_FAILED = -1,
	// A child release landed but clearing its durable parent pointer did not,
	// or a block's bitmap write landed while a count ledger writeback did not.
	EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK = -2,
	// The inode's bitmap bit is clear while its counts — or the orphan chain
	// entry that still names it — say otherwise.
	EXT2_FREE_RETRY_MAP_NAMES_FREE_INODE = -3
};

// Both reusable results answer the caller's only question the same way, and
// asking it by name beats two comparisons that must never drift apart.
static inline bool ext2_free_left_storage_reusable(int rc)
{
	return rc == EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK ||
	       rc == EXT2_FREE_RETRY_MAP_NAMES_FREE_INODE;
}

// ── The per-call batch ──────────────────────────────────────────────────────
// What one op holds back from the disk until a flush point: the group bitmap
// it is allocating from or releasing into, the indirect leaf collecting a
// write's new block pointers, and a write's run of data blocks not yet
// written. Without it every data block cost four commands (data, bitmap,
// group descriptor, superblock) and a queue-depth-one driver paid each one
// serially — 1.7 MB/s on an NVMe drive, and a 100MB file took a minute to
// delete. With it a 1MB write is a handful of commands, and a release is a
// few per indirect block.
//
// THE FLUSH ORDER IS THE DOCTRINE, restated for the batch. Allocating: data
// run first (content), then bitmap → descriptor → superblock (the map that
// marks it used), then the leaf (the reference); the inode, the last
// reference, is the caller's write after ext2_batch_end. A chain block
// (zero_fill) never waits: its content is zeroed, its bit is marked, and
// the map is flushed before the parent entry that points at it is written
// — allocate-then-reference at the indirection layer, exactly as before the
// batch existed. Releasing: bits clear in the bitmap copy, the map publishes
// them (bitmap, then counts) at a flush point, and a parent's pointers are
// cleared in ONE write only after its subtree's bits are published; a
// failed release publishes nothing pending, so the retry map keeps naming
// storage the disk still calls used. A crash between any two flushes
// therefore still leaves at worst a leak, and replay stays idempotent.
//
// A caller that passes NULL gets one-command-per-step behavior — the right
// shape for allocating or releasing a single block, with nothing to batch.
//
// THE BATCH TAKES ITS TWO SCRATCH BUFFERS AT BEGIN, in a fixed order, and
// gives them back at end in the reverse — the pool is LIFO (ext2_internal.h),
// and a buffer returned out of turn is mistaken for a fallback allocation
// and kfree'd under the pool's feet. Taking them lazily broke exactly that
// way: a write whose first allocation sat in the indirect range loaded the
// leaf before the bitmap.
typedef struct {
	vfs_filesystem_t *fs;
	ext2_fs_t        *e;
	uint8_t  *bm;            // scratch for the group bitmap (taken first)
	bool      bm_loaded;     // ...and whether a group is in it
	uint32_t  bm_group;
	bool      bm_dirty;      // allocations: bits set that the disk has not seen
	uint32_t  bm_alloced;    // allocations: how many, to give the counts back if the bitmap never lands
	uint32_t  bm_next;       // allocations: the bit after the last one handed out
	uint32_t  bm_freed;      // releases: bits cleared, counts not yet credited
	bool      bm_reconcile;  // releases: a bit was already clear — rebuild the counts
	uint32_t  exposed;       // releases: bits free ON DISK whose durable pointer is not yet cleared
	uint32_t *leaf;          // scratch for the indirect leaf (taken second)
	uint32_t  leaf_block;    // ...the block in it, 0 = none
	bool      leaf_dirty;
	const uint8_t *run_src;  // pending data run: source bytes...
	uint32_t  run_block;     // ...their first disk block...
	uint32_t  run_count;     // ...and how many blocks (0 = no run)
	bool      failed;        // a flush lost a write; sticky, reported by _end
	int       free_rc;       // a release's verdict in the vocabulary above, once failed
} ext2_batch_t;

// False when the pool cannot fund the batch — nothing was taken.
static bool ext2_batch_begin(ext2_batch_t *b, vfs_filesystem_t *fs, ext2_fs_t *e)
{
	memset(b, 0, sizeof(*b));
	b->fs = fs;
	b->e  = e;
	b->bm = wr_scratch_get(e);
	if (b->bm == NULL)
		return false;
	b->leaf = (uint32_t *)wr_scratch_get(e);
	if (b->leaf == NULL)
	{
		wr_scratch_put(e, b->bm);
		b->bm = NULL;
		return false;
	}
	return true;
}

// The data run: one device write for every contiguous block of it.
static void ext2_batch_flush_run(ext2_batch_t *b)
{
	if (b->run_count == 0)
		return;
	ext2_fs_t *e = b->e;
	vfs_filesystem_t *fs = b->fs;
	uint64_t sector = fs->block_device_info->block_device->partition_table
	                      ->parts[fs->partNumber]->partStartSector
	                  + (uint64_t)b->run_block * e->sectors_per_block;
	if (fs->bops->write(fs->block_device_info, sector, b->run_src,
	                    (uint64_t)b->run_count * e->sectors_per_block) != 0)
		b->failed = true;
	b->run_count = 0;
}

// The map: bitmap, then the counts that describe it — and the data run
// before either, so no flush point anywhere publishes a block whose content
// is still only in memory (a hole filled inside the file's size would
// otherwise read its previous owner's bytes after a crash). An allocation
// took its counts out of the cache as it happened; a release's are credited
// here, once the bitmap that publishes its blocks has landed — the point of
// no return ext2_free_block describes, with the same verdict when a ledger
// write fails past it.
static void ext2_batch_flush_map(ext2_batch_t *b)
{
	// A batch that has lost a write flushes nothing more: its callers are
	// on their way out, and ext2_batch_end gives back what it still holds.
	if (b->failed)
		return;
	ext2_batch_flush_run(b);
	if (b->failed)
		return;
	if (!b->bm_dirty && b->bm_freed == 0 && !b->bm_reconcile)
		return;
	ext2_fs_t *e = b->e;
	uint32_t g = b->bm_group;
	bool releasing = b->bm_freed != 0 || b->bm_reconcile;

	if (ext2_write_fs_block(b->fs, e, e->groups[g].bg_block_bitmap, b->bm) != 0)
	{
		// Nothing published: the disk still calls every block here used,
		// and every block allocated here still free. What that means for
		// the counts, the discard at ext2_batch_end settles — an
		// allocation's go back to the cache, a release's pending count is
		// reported to its caller.
		b->failed = true;
		if (releasing && b->free_rc == 0)
			b->free_rc = EXT2_FREE_FAILED;
		return;
	}
	else
	{
		if (releasing)
			b->exposed += b->bm_freed;   // free ON DISK now, still named by a durable pointer
		int ledger;
		if (b->bm_reconcile)
			ledger = ext2_reconcile_free_block_counts(b->fs, e, g, b->bm);
		else
		{
			e->groups[g].bg_free_blocks_count += (uint16_t)b->bm_freed;
			e->sb.s_free_blocks_count += b->bm_freed;
			ledger = (ext2_gd_writeback(b->fs, e, g) == 0 &&
			          ext2_sb_writeback(b->fs, e) == 0) ? 0 : -1;
		}
		if (ledger != 0)
		{
			// The bitmap landed, the ledgers did not. For a release that is
			// the reusable window: the retry map still names blocks anyone
			// may now take.
			b->failed = true;
			if (releasing)
				b->free_rc = EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK;
		}
	}
	b->bm_dirty = false;
	b->bm_alloced = 0;
	b->bm_freed = 0;
	b->bm_reconcile = false;
}

// A batch that cannot finish must not publish what it holds, and must give
// back what it took on the strength of a publication that never came: an
// allocation's counts return to the cache (the bits stay clear on disk),
// and a release's cleared-only-in-the-copy bits stay set on disk, where the
// retry map can still find them. Reports how many of those a release
// counted as freed that never landed. Idempotent — ext2_batch_end calls it
// for every failed batch, and a caller may have called it already.
static uint32_t ext2_batch_discard_map(ext2_batch_t *b)
{
	ext2_fs_t *e = b->e;
	if (b->bm_alloced != 0)
	{
		e->groups[b->bm_group].bg_free_blocks_count += (uint16_t)b->bm_alloced;
		e->sb.s_free_blocks_count += b->bm_alloced;
		b->bm_alloced = 0;
	}
	uint32_t unpublished = b->bm_freed;
	b->bm_dirty = false;
	b->bm_freed = 0;
	b->bm_reconcile = false;
	return unpublished;
}

// The reference: the leaf's new pointers, after the map that licenses them.
static void ext2_batch_flush_leaf(ext2_batch_t *b)
{
	if (!b->leaf_dirty)
		return;
	ext2_batch_flush_map(b);
	// A map that did not land licenses nothing: the pointers stay unwritten
	// (the blocks leak; a reference to unmarked blocks would dangle).
	if (!b->failed && ext2_write_fs_block(b->fs, b->e, b->leaf_block, b->leaf) != 0)
		b->failed = true;
	b->leaf_dirty = false;
}

// Everything, in doctrine order, and the scratch given back. Returns 0 or
// -1 for any write the batch lost — the caller's io_error.
static int ext2_batch_end(ext2_batch_t *b)
{
	ext2_batch_flush_run(b);
	ext2_batch_flush_map(b);
	ext2_batch_flush_leaf(b);
	// A batch that lost a write along the way still holds what that write
	// would have published: give it back rather than let it go stale.
	if (b->failed)
		(void)ext2_batch_discard_map(b);
	// LIFO pool: the leaf was taken after the bitmap, so it goes back first.
	wr_scratch_put(b->e, (uint8_t *)b->leaf);
	wr_scratch_put(b->e, b->bm);
	b->leaf = NULL;
	b->bm = NULL;
	return b->failed ? -1 : 0;
}

// Point the batch at group `g`'s bitmap: a switch flushes the old group's
// map first (its counts describe bits the disk must see before another
// group's do). Returns the buffer, or NULL on I/O failure.
static uint8_t *ext2_batch_bitmap(ext2_batch_t *b, uint32_t g)
{
	if (b->bm_loaded && b->bm_group == g)
		return b->bm;
	ext2_batch_flush_map(b);
	b->bm_loaded = false;
	if (ext2_read_fs_block(b->fs, b->e, b->e->groups[g].bg_block_bitmap, b->bm) != 0)
		return NULL;
	b->bm_loaded = true;
	b->bm_group  = g;
	b->bm_next   = 0;
	return b->bm;
}

// First clear bit at or after `from`, else before it; ~0u when none.
static uint32_t ext2_bitmap_find_free(const uint8_t *bm, uint32_t span, uint32_t from)
{
	if (from >= span)
		from = 0;
	for (uint32_t pass = 0; pass < 2; pass++)
	{
		uint32_t lo = pass == 0 ? from : 0;
		uint32_t hi = pass == 0 ? span : from;
		for (uint32_t bit = lo; bit < hi; bit++)
		{
			if (bm[bit / 8] == 0xFF) { bit |= 7; continue; }   // skip the full byte
			if (!(bm[bit / 8] & (uint8_t)(1u << (bit % 8))))
				return bit;
		}
	}
	return (uint32_t)~0u;
}

// Scan one group's block bitmap for a free bit. Returns the absolute block
// number (0 = group full / I/O error) with the bitmap and counts updated —
// on disk at once, or held in `batch` until its next flush point. If
// zero_fill, the block's CONTENT is zeroed on disk BEFORE the bitmap marks
// it used (allocate-then-reference: an indirect or directory block must
// never be reachable in a garbage state; a plain data block skips this
// because its caller writes the real content before any reference exists),
// and a batched zero_fill allocation flushes the map before returning, so
// the parent entry the caller writes next is ordered after the bit.
static uint32_t ext2_alloc_block_in_group(vfs_filesystem_t *fs, ext2_fs_t *e,
                                          uint32_t g, bool zero_fill,
                                          ext2_batch_t *batch)
{
	if (e->groups[g].bg_free_blocks_count == 0)
		return 0;

	// Blocks this group actually covers (the last group is usually partial).
	uint32_t bpg   = e->sb.s_blocks_per_group;
	uint32_t base  = e->sb.s_first_data_block + g * bpg;
	uint32_t span  = e->sb.s_blocks_count - base;
	if (span > bpg)
		span = bpg;

	uint8_t *bm;
	if (batch != NULL)
	{
		bm = ext2_batch_bitmap(batch, g);
		if (bm == NULL)
			return 0;
	}
	else
	{
		bm = wr_scratch_get(e);
		if (bm == NULL)
			return 0;
		if (ext2_read_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) != 0)
		{
			wr_scratch_put(e, bm);
			return 0;
		}
	}

	// From the hint when batched — consecutive allocations land on
	// consecutive blocks, which is what makes the data runs long — and from
	// the start otherwise, the shape every one-block caller had.
	uint32_t bit = ext2_bitmap_find_free(bm, span, batch != NULL ? batch->bm_next : 0);
	if (bit == (uint32_t)~0u)
	{
		// bg_free_blocks_count said yes but the bitmap says no — believe the
		// bitmap (it's the on-disk truth) and say so; fsck would flag this.
		if (batch == NULL)
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
			if (batch == NULL)
				wr_scratch_put(e, bm);
			return 0;
		}
		// Scratch arrives DIRTY (pool reuse) — make it the zero block by hand.
		memset(z, 0, e->block_size);
		if (ext2_write_fs_block(fs, e, block, z) != 0)
		{
			wr_scratch_put(e, z);
			if (batch == NULL)
				wr_scratch_put(e, bm);
			return 0;
		}
		wr_scratch_put(e, z);
	}

	bm[bit / 8] |= (uint8_t)(1u << (bit % 8));

	if (batch != NULL)
	{
		// The counts leave the cache now, with the bit; the flush gives
		// them back if the bitmap never lands (bm_alloced).
		e->groups[g].bg_free_blocks_count--;
		e->sb.s_free_blocks_count--;
		batch->bm_dirty = true;
		batch->bm_alloced++;
		batch->bm_next  = bit + 1;
		if (zero_fill)
		{
			ext2_batch_flush_map(batch);
			if (batch->failed)
				return 0;
		}
		return block;
	}

	// Unbatched: the counts follow the bitmap write, so a write that fails
	// leaves cache and disk agreeing that nothing happened.
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
                          uint32_t goal_group, bool zero_fill,
                          ext2_batch_t *batch)
{
	for (uint32_t i = 0; i < e->groups_count; i++)
	{
		uint32_t g = (goal_group + i) % e->groups_count;
		uint32_t block = ext2_alloc_block_in_group(fs, e, g, zero_fill, batch);
		if (block != 0)
			return block;
	}
	printd(DEBUG_VFS, "ext2: out of blocks (%u groups scanned)\n", e->groups_count);
	return 0;   // filesystem full — the caller turns this into a short write
}

// Free one block: bitmap bit off, counts up. Most callers have already
// removed every reference. Orphan replay deliberately retains its retry map
// until the free is durable; write_lock (or pre-allocation mount replay)
// prevents reuse in that interval, and an already-clear bit makes repeating
// the release after a crash harmless.
//
// With a batch the bit clears in the batch's copy of the group bitmap and
// the publication — bitmap, then counts — happens at its next flush point,
// which is where the result vocabulary above is then spoken (free_rc). An
// already-clear bit under a batch asks that flush to rebuild the counts
// from the bitmap, exactly what the immediate path does here itself.
int ext2_free_block(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t block,
                    ext2_batch_t *batch)
{
	if (block < e->sb.s_first_data_block || block >= e->sb.s_blocks_count)
		return -1;

	uint32_t rel = block - e->sb.s_first_data_block;
	uint32_t g   = rel / e->sb.s_blocks_per_group;
	uint32_t bit = rel % e->sb.s_blocks_per_group;
	if (g >= e->groups_count)
		return -1;

	if (batch != NULL)
	{
		uint8_t *bbm = ext2_batch_bitmap(batch, g);
		if (bbm == NULL)
			return EXT2_FREE_FAILED;
		if (batch->failed)
			return batch->free_rc;      // the group switch's flush lost something
		if (!(bbm[bit / 8] & (uint8_t)(1u << (bit % 8))))
		{
			// Already free: a replay revisiting a bit an earlier attempt
			// published. Free on disk AND still named by the durable map
			// that led us here — exposed from this moment, not from some
			// later publication, so a failure before its pointer is
			// cleared owes the reusable verdict.
			batch->bm_reconcile = true;
			batch->exposed++;
			return 0;
		}
		bbm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
		batch->bm_freed++;
		return 0;
	}

	uint8_t *bm = wr_scratch_get(e);
	if (bm == NULL)
		return -1;
	int rc = EXT2_FREE_FAILED;
	if (ext2_read_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) == 0)
	{
		// Replay may revisit a block whose bitmap update completed just before
		// power failed. It is already free, but the two count ledgers may still
		// describe it as allocated; repair both before declaring completion.
		// A reconcile that ITSELF fails leaves the caller's retry map naming a
		// block the bitmap calls free — the reusable window, not a clean miss.
		if (!(bm[bit / 8] & (uint8_t)(1u << (bit % 8))))
		{
			rc = ext2_reconcile_free_block_counts(fs, e, g, bm) == 0
			         ? 0 : EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK;
			goto out;
		}
		bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
		if (ext2_write_fs_block(fs, e, e->groups[g].bg_block_bitmap, bm) == 0)
		{
			e->groups[g].bg_free_blocks_count++;
			e->sb.s_free_blocks_count++;
			// PAST THE POINT OF NO RETURN. That bitmap write published this
			// block to every allocator on the filesystem; a count writeback
			// failing now cannot be reported as "nothing happened", because
			// the caller's retry map still names a block anyone may take.
			rc = (ext2_gd_writeback(fs, e, g) == 0 &&
			      ext2_sb_writeback(fs, e) == 0)
			         ? 0 : EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK;
		}
	}
out:
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
	int rc = EXT2_FREE_FAILED;
	if (ext2_read_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) == 0)
	{
		// Same replay rule as blocks: an orphan may have reached this bitmap
		// write before a crash but not either count ledger or final chain unlink.
		if (!(bm[bit / 8] & (uint8_t)(1u << (bit % 8))))
		{
			rc = ext2_reconcile_free_inode_counts(fs, e, g, bm) == 0
			         ? 0 : EXT2_FREE_RETRY_MAP_NAMES_FREE_INODE;
			goto out;
		}
		bm[bit / 8] &= (uint8_t)~(1u << (bit % 8));
		if (ext2_write_fs_block(fs, e, e->groups[g].bg_inode_bitmap, bm) == 0)
		{
			e->groups[g].bg_free_inodes_count++;
			e->sb.s_free_inodes_count++;
			if (is_dir && e->groups[g].bg_used_dirs_count > 0)
				e->groups[g].bg_used_dirs_count--;
			// Same point of no return as the block path, and worse if ignored:
			// this inode NUMBER is allocatable now, while the orphan chain may
			// still name it for teardown. A stale count is not the hazard —
			// a later replay releasing the new file that got this number is.
			rc = (ext2_gd_writeback(fs, e, g) == 0 &&
			      ext2_sb_writeback(fs, e) == 0)
			         ? 0 : EXT2_FREE_RETRY_MAP_NAMES_FREE_INODE;
		}
	}
out:
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
//
// With a batch, a DATA leaf's entry is set in the batch's copy of the
// indirect block and written at the batch's next flush point, after the
// bitmap that licenses it; a CHAIN child's entry is written at once, as
// its zero-filled content and its (batch-flushed) bit already are.
static uint32_t ext2_ind_get_or_alloc(vfs_filesystem_t *fs, ext2_fs_t *e,
                                      uint32_t ind_block, uint32_t idx,
                                      uint32_t goal_group, bool child_is_indirect,
                                      ext2_inode_t *ino, bool *fresh,
                                      ext2_batch_t *batch)
{
	if (ind_block == 0 || idx >= e->ptrs_per_block)
		return 0;

	if (batch != NULL && !child_is_indirect)
	{
		if (batch->leaf_block != ind_block)
		{
			// Another leaf: publish the old one — its data run and map
			// first, inside — then load this one; nothing of the old leaf
			// is left in memory once it is flushed.
			ext2_batch_flush_leaf(batch);
			batch->leaf_block = 0;
			if (ext2_read_fs_block(fs, e, ind_block, batch->leaf) != 0)
				return 0;
			batch->leaf_block = ind_block;
		}
		uint32_t child = batch->leaf[idx];
		if (child == 0)
		{
			child = ext2_alloc_block(fs, e, goal_group, false, batch);
			if (child != 0)
			{
				batch->leaf[idx] = child;
				batch->leaf_dirty = true;
				ino->i_blocks += e->sectors_per_block;
				if (fresh != NULL)
					*fresh = true;
			}
		}
		return child;
	}

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
		child = ext2_alloc_block(fs, e, goal_group, child_is_indirect, batch);
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
// (disk full) or I/O error. `batch` (or NULL) rides down to the allocator
// and the leaf writes; the chain roots in the inode need nothing from it.
uint32_t ext2_bmap_alloc(vfs_filesystem_t *fs, ext2_fs_t *e,
                         ext2_inode_t *ino, uint32_t fblock,
                         uint32_t goal_group, bool *fresh,
                         ext2_batch_t *batch)
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
			uint32_t b = ext2_alloc_block(fs, e, goal_group, false, batch);
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
			uint32_t ind = ext2_alloc_block(fs, e, goal_group, true, batch);
			if (ind == 0)
				return 0;
			ino->i_block[EXT2_IND_BLOCK] = ind;
			ino->i_blocks += e->sectors_per_block;
		}
		return ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_IND_BLOCK],
		                             fblock, goal_group, false, ino, fresh, batch);
	}
	fblock -= ppb;

	// Double indirect.
	if (fblock < (uint64_t)ppb * ppb)
	{
		if (ino->i_block[EXT2_DIND_BLOCK] == 0)
		{
			uint32_t dind = ext2_alloc_block(fs, e, goal_group, true, batch);
			if (dind == 0)
				return 0;
			ino->i_block[EXT2_DIND_BLOCK] = dind;
			ino->i_blocks += e->sectors_per_block;
		}
		uint32_t l1 = ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_DIND_BLOCK],
		                                    fblock / ppb, goal_group, true, ino, NULL, batch);
		if (l1 == 0)
			return 0;
		return ext2_ind_get_or_alloc(fs, e, l1, fblock % ppb,
		                             goal_group, false, ino, fresh, batch);
	}
	fblock -= ppb * ppb;

	// Triple indirect — same ten lines that bought the read side ~16GB.
	if (ino->i_block[EXT2_TIND_BLOCK] == 0)
	{
		uint32_t tind = ext2_alloc_block(fs, e, goal_group, true, batch);
		if (tind == 0)
			return 0;
		ino->i_block[EXT2_TIND_BLOCK] = tind;
		ino->i_blocks += e->sectors_per_block;
	}
	uint32_t l1 = ext2_ind_get_or_alloc(fs, e, ino->i_block[EXT2_TIND_BLOCK],
	                                    fblock / (ppb * ppb), goal_group, true, ino, NULL, batch);
	if (l1 == 0)
		return 0;
	uint32_t l2 = ext2_ind_get_or_alloc(fs, e, l1, (fblock / ppb) % ppb,
	                                    goal_group, true, ino, NULL, batch);
	if (l2 == 0)
		return 0;
	return ext2_ind_get_or_alloc(fs, e, l2, fblock % ppb,
	                             goal_group, false, ino, fresh, batch);
}

// ── Truncation ──────────────────────────────────────────────────────────────

// Free every block a retry map names: indirect chains bottom-up, then their
// roots, through one batch. A parent's pointers remain durable until its
// whole subtree's bits are PUBLISHED (the batch's map flush), and are then
// cleared in one write; a crash in between leaves replay a path to
// already-free children, whose bitmap release is idempotent. depth counts
// pointer levels: 0 = data block, 1..3 = indirect. Recursion is bounded by
// ext2's own geometry — three levels, ever.
//
// Results are the free path's shared vocabulary (declared above the batch,
// whose flush is where the reusable window opens): 0, EXT2_FREE_FAILED, or
// one of the EXT2_FREE_RETRY_MAP_NAMES_FREE_* results the caller must
// persist-then-demote on. A subtree that fails is left exactly as the disk
// has it — parent unwritten, pending bits unpublished — so the record above
// still leads replay to everything not yet released.
//
// EXPOSURE, kept honest here: the batch counts bits free on disk that a
// durable pointer still names (a publication adds the bits it cleared; a
// replay's already-clear bit is added the moment it is seen). A parent
// write that lands un-names every child it dropped, fresh or replayed, so
// they leave the count. What remains when a release fails is what decides
// the verdict it owes.
static int ext2_free_chain(vfs_filesystem_t *fs, ext2_fs_t *e,
                           uint32_t block, uint32_t depth,
                           uint32_t *freed, ext2_batch_t *batch)
{
	if (block == 0)
		return 0;

	if (depth > 0)
	{
		uint32_t *buf = (uint32_t *)wr_scratch_get(e);
		if (buf == NULL)
			return EXT2_FREE_FAILED;
		if (ext2_read_fs_block(fs, e, block, buf) != 0)
		{
			wr_scratch_put(e, (uint8_t *)buf);
			return EXT2_FREE_FAILED;
		}

		int rc = 0;
		uint32_t cleared = 0;     // pointers this parent will drop
		for (uint32_t i = 0; i < e->ptrs_per_block && rc == 0; i++)
		{
			if (buf[i] == 0)
				continue;
			rc = ext2_free_chain(fs, e, buf[i], depth - 1, freed, batch);
			if (rc == 0)
			{
				buf[i] = 0;
				cleared++;
			}
		}
		if (rc == 0)
		{
			// The subtree's bits, published — then its pointers, in one
			// write. A parent write that fails leaves durable pointers
			// naming published-free blocks: the reusable window, named.
			// One that lands un-names them: they leave the exposure count.
			ext2_batch_flush_map(batch);
			if (batch->failed)
				rc = batch->free_rc;
			else if (cleared != 0 && ext2_write_fs_block(fs, e, block, buf) != 0)
				rc = EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK;
			else
				batch->exposed -= cleared;
		}
		wr_scratch_put(e, (uint8_t *)buf);
		if (rc != 0)
			return rc;
	}
	// The block itself, into the batch; its own pointer, one level up, stays
	// until that level publishes.
	int free_rc = ext2_free_block(fs, e, block, batch);
	if (free_rc != 0)
		return free_rc;
	(*freed)++;
	return 0;
}

// Free all blocks named by `old` (a pre-truncate copy of the inode). On an
// error, `old` remains a replayable map — the whole of it: nothing is
// pruned until every bit is published, and over-naming is harmless because
// releasing an already-free block is idempotent. An indirect root may then
// lead replay through already-free children; that is the design.
static int ext2_free_inode_blocks(vfs_filesystem_t *fs, ext2_fs_t *e,
                                  ext2_inode_t *old)
{
	ext2_batch_t batch;
	if (!ext2_batch_begin(&batch, fs, e))
		return EXT2_FREE_FAILED;      // nothing taken, nothing changed
	uint32_t freed = 0;
	int rc = EXT2_FREE_FAILED;
	for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++)
	{
		rc = ext2_free_chain(fs, e, old->i_block[i], 0, &freed, &batch);
		if (rc < 0)
			goto failed;
	}
	for (uint32_t depth = 1; depth <= 3; depth++)
	{
		uint32_t slot = EXT2_IND_BLOCK + depth - 1;
		rc = ext2_free_chain(fs, e, old->i_block[slot], depth, &freed, &batch);
		if (rc < 0)
			goto failed;
	}
	// The last group's bits: the roots' own, and any direct blocks'.
	if (ext2_batch_end(&batch) != 0)
	{
		rc = batch.free_rc != 0 ? batch.free_rc : EXT2_FREE_FAILED;
		goto failed;
	}
	memset(old->i_block, 0, sizeof(old->i_block));
	old->i_blocks = 0;
	return 0;

failed:
	// Bits cleared only in the copy never landed: they stay used on disk and
	// named by the map, so they are not "released" for the record either.
	freed -= ext2_batch_discard_map(&batch);
	(void)ext2_batch_end(&batch);
	// But bits an EARLIER flush published, whose durable pointer — an
	// entry of the inode's own, or of a parent whose cleared pointers never
	// got written — is still in place, are free on disk AND named: the
	// reusable window, whatever the later failure was, and "nothing on
	// disk changed" would be a lie the allocator acts on. The exposure
	// count is exactly that set (a parent write that lands un-names its
	// children), so it, not the mere fact of a publication, upgrades.
	if (rc == EXT2_FREE_FAILED && batch.exposed > 0)
		rc = EXT2_FREE_RETRY_MAP_NAMES_FREE_BLOCK;
	uint64_t released_sectors = (uint64_t)freed * e->sectors_per_block;
	old->i_blocks = released_sectors < old->i_blocks
	                    ? old->i_blocks - (uint32_t)released_sectors : 0;
	return rc;
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
	uint32_t nb = ext2_bmap_alloc(fs, e, dir, dir_blocks, goal_group, &fresh, NULL);
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

// The 2GB-1 GROWTH cap: creating/extending a large file requires updating the
// superblock's RO_COMPAT_LARGE_FILE bookkeeping, which this writer does not
// yet own. Existing large files are nevertheless fully readable and may be
// overwritten in place: their high size word stays untouched, and no growth
// bookkeeping is involved. hexedit is the first consumer of that distinction.
#define EXT2_MAX_FILE_SIZE 0x7FFFFFFFu

static int ext2_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	ext2_handle_t *h = (ext2_handle_t *)vfs_file->handle;
	vfs_filesystem_t *fs = (vfs_filesystem_t *)vfs_file->owner;
	ext2_fs_t *e = (ext2_fs_t *)fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);
	if (fs->read_only)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

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
	h->size = ext2_regular_file_size(&h->inode);

	// Past the growth cap, overwrite only. Crossing EOF would require changing
	// the high size word and feature bookkeeping; touching existing allocated
	// bytes (or allocating into an existing sparse hole) does not.
	if (h->size > EXT2_MAX_FILE_SIZE)
	{
		if (h->pos >= h->size)
		{
			spinlock_release_irqrestore(&e->write_lock, lock_flags);
			return -1;
		}
		if (size > h->size - h->pos)
			size = h->size - h->pos;
	}
	else
	{
		// Clamp ordinary growth to the legacy-safe ceiling. A short write is
		// the existing "device full" shape at the syscall boundary.
		if (h->pos >= EXT2_MAX_FILE_SIZE)
		{
			spinlock_release_irqrestore(&e->write_lock, lock_flags);
			return -1;
		}
		if (size > EXT2_MAX_FILE_SIZE - h->pos)
			size = EXT2_MAX_FILE_SIZE - h->pos;
	}

	// Data blocks want the inode's own group — FFS locality.
	uint32_t goal_group = (h->ino - 1) / e->sb.s_inodes_per_group;

	uint8_t *scratch = wr_scratch_get(e);
	if (scratch == NULL)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

	// One batch for the call (the doctrine of its flush points is on the
	// type). Whole blocks join the pending data run while they stay
	// contiguous on disk and in the caller's buffer; anything else — a
	// partial block, a run broken by a hole's odd placement — flushes the
	// run and goes its own way.
	ext2_batch_t batch;
	if (!ext2_batch_begin(&batch, fs, e))
	{
		wr_scratch_put(e, scratch);
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

	uint64_t start_pos = h->pos;
	size_t done = 0;
	bool io_error = false;
	while (done < size)
	{
		if (batch.failed)
		{
			io_error = true;   // a flush already lost a write; stop piling on
			break;
		}
		uint32_t fblock   = (uint32_t)(h->pos / e->block_size);
		uint32_t in_block = (uint32_t)(h->pos % e->block_size);
		size_t   chunk    = e->block_size - in_block;
		if (chunk > size - done)
			chunk = size - done;

		bool fresh = false;
		uint32_t disk_block = ext2_bmap_alloc(fs, e, &h->inode, fblock,
		                                      goal_group, &fresh, &batch);
		if (disk_block == 0)
			break;   // disk full (or chain I/O error): report the short count

		int rc = 0;
		const uint8_t *src = (const uint8_t *)buffer + done;
		if (chunk == e->block_size)
		{
			// Whole block: no read needed, old content irrelevant. Extend
			// the run when this block follows its last one on disk AND
			// these bytes follow its last ones in memory; otherwise start
			// a new run here.
			if (batch.run_count != 0 &&
			    disk_block == batch.run_block + batch.run_count &&
			    src == batch.run_src + (size_t)batch.run_count * e->block_size)
				batch.run_count++;
			else
			{
				ext2_batch_flush_run(&batch);
				batch.run_src   = src;
				batch.run_block = disk_block;
				batch.run_count = 1;
			}
			if (batch.failed)
				rc = -1;
		}
		else if (fresh)
		{
			// Partial write into a block that has NO content yet: the rest
			// of the block must be zeros, not whatever the previous owner
			// left there. (This is both the hole-fill contract — holes read
			// as zeros — and a don't-leak-freed-data rule.)
			ext2_batch_flush_run(&batch);
			memset(scratch, 0, e->block_size);
			memcpy(scratch + in_block, src, chunk);
			rc = ext2_write_fs_block(fs, e, disk_block, scratch);
		}
		else
		{
			ext2_batch_flush_run(&batch);
			rc = ext2_rmw_fs_block(fs, e, disk_block, in_block, src, chunk);
		}
		if (rc != 0)
		{
			io_error = true;
			break;
		}

		done += chunk;
		h->pos += chunk;
	}
	// Everything the batch still holds, in doctrine order — run, map, leaf
	// — BEFORE the inode below references any of it. A batch that lost any
	// of those writes is an I/O error for the WHOLE call: the in-memory
	// inode names blocks the disk may not mark or may not hold, so it is
	// thrown away and re-read, the position rewound, and -1 returned. What
	// landed meanwhile is an overwrite of bytes the caller asked for
	// (harmless to repeat) or a marked block nothing references — a leak,
	// which fsck reclaims. The SHORT write (disk full, no flush lost)
	// keeps reporting exactly what landed, as it always has.
	bool lost = ext2_batch_end(&batch) != 0;
	wr_scratch_put(e, scratch);
	if (lost)
	{
		uint8_t *ibuf = wr_scratch_get(e);
		if (ibuf != NULL)
		{
			// A re-read that fails leaves the stale copy; the next write
			// re-reads before it mutates (the fresh-inode discipline), so
			// nothing on disk can come to depend on it.
			(void)ext2_read_inode_buf(fs, e, h->ino, &h->inode, ibuf);
			wr_scratch_put(e, ibuf);
		}
		h->size = ext2_regular_file_size(&h->inode);
		h->pos = start_pos;
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

	// WRITE-THROUGH: commit the inode now — size, mtime, and every block
	// pointer bmap_alloc added, in ONE 128-byte write (i_size never outruns
	// i_block[]). Committed even on the short-write path so the bytes that
	// DID land are durable and accounted.
	if (done > 0 || h->inode.i_blocks != 0)
	{
		if (h->pos > h->size)
			h->inode.i_size = (uint32_t)h->pos;
		h->inode.i_mtime = (uint32_t)kSystemCurrentTime;
		if (ext2_write_inode_disk(fs, e, h->ino, &h->inode) != 0)
			io_error = true;
		h->size = ext2_regular_file_size(&h->inode);
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
//   "u"      — existing file, read/write without truncating it
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
		case 'u':
			return ext2_open_existing(vfs_file, path, vfs_fs);

		case 'a':
		{
			if (vfs_fs->read_only)
				return -1;
			if (ext2_open_existing(vfs_file, path, vfs_fs) != 0)
			{
				// Absent: create it, then open the now-existing file. (A
				// concurrent rm between the unlock and the reopen makes the
				// reopen fail — correct, just unlucky.)
				uint64_t flags = spinlock_acquire_irqsave(&e->write_lock);
				if (vfs_fs->read_only)
				{
					spinlock_release_irqrestore(&e->write_lock, flags);
					return -1;
				}
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
			if (vfs_fs->read_only)
			{
				spinlock_release_irqrestore(&e->write_lock, flags);
				return -1;
			}

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

// Point an EXISTING directory entry at a different inode, in place. The
// name, its length, and the entry's rec_len are all unchanged — only the
// inode number (and the file_type byte that shadows it) move.
//
// This is the primitive that makes atomic replacement possible, and it is
// worth being explicit about WHY it is atomic: a directory entry's inode
// number is four bytes inside one block, and we publish the change by
// writing that whole block. A reader either sees the block before or after;
// there is no third state in which the name exists but points nowhere. That
// single block write is the instant at which "refresh.part" becomes
// "refresh" — everything else rename does is bookkeeping around it.
//
// Deliberately does NOT write the directory's inode back (unlike
// ext2_dir_insert, which must, because it may have grown the directory).
// The caller owns the parent inode struct here and often has its own
// link-count arithmetic to fold into the same write; two writers of one
// in-memory inode is how you lose one of the two updates.
//
// Returns 0 on success, -1 if the name isn't there or the write failed.
static int ext2_dir_repoint(vfs_filesystem_t *fs, ext2_fs_t *e,
                            ext2_inode_t *dir, const char *name, uint32_t len,
                            uint32_t new_ino, uint8_t file_type)
{
	// Same feature gate ext2_dir_insert applies: without FILETYPE the byte
	// is reserved-zero and writing a type into it would be a small lie on
	// disk that fsck is entitled to complain about.
	uint32_t incompat = (e->sb.s_rev_level >= EXT2_DYNAMIC_REV)
	                        ? e->sb.s_feature_incompat : 0;
	if (!(incompat & EXT2_FEATURE_INCOMPAT_FILETYPE))
		file_type = 0;

	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
		return -1;

	int rc = -1;
	uint32_t dir_blocks = dir->i_size / e->block_size;
	for (uint32_t fb = 0; fb < dir_blocks && rc != 0; fb++)
	{
		uint32_t disk_block = ext2_bmap(fs, e, dir, fb);
		if (disk_block == 0)
			continue;
		if (ext2_read_fs_block(fs, e, disk_block, buf) != 0)
			break;

		uint32_t pos = 0;
		while (pos < e->block_size)
		{
			ext2_dir_entry_2_t *de = (ext2_dir_entry_2_t *)(buf + pos);
			if (de->rec_len == 0)
				break;   // corrupt block — try the next one
			if (de->inode != 0 && de->name_len == len &&
			    memcmp(de->name, name, len) == 0)
			{
				de->inode = new_ino;
				de->file_type = file_type;
				rc = (ext2_write_fs_block(fs, e, disk_block, buf) == 0) ? 0 : -1;
				break;
			}
			pos += de->rec_len;
		}
	}

	wr_scratch_put(e, buf);
	return rc;
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

// ── The orphan list: inodes with no name and a handle ───────────────────────
//
// THE PROBLEM, stated once. A directory entry and a file are different
// things — that separation IS the inode, and it is the idea MS-DOS's
// filesystem did not have. So "delete this name" and "destroy this file"
// are different operations, and when a program still holds the file open,
// only the first one is safe. Until 2026-08-16 os64 sidestepped this by
// REFUSING (ruling 5, 2026-08-04: rm declines a busy file), which is honest
// and costs nothing right up until the thing you need to replace is the
// program that is running — /bin/husk, upgrading itself over the wire.
//
// THE FIX is the one Unix has had since the beginning: remove the NAME
// immediately, and let the FILE die at last close. Between those two
// moments the inode is an orphan — unreachable by any path, still perfectly
// readable through the handle keeping it alive. `os64 refresh` replaces
// husk's binary, the running husk keeps demand-paging the old image out of
// an inode with no name, and the new one is there at the next boot.
//
// THE CRASH QUESTION, and why this list is ON DISK. An orphan is a live
// allocation nothing points to; lose the record and you have leaked an
// inode and its blocks. An in-memory list would be simpler and would work
// perfectly until the machine lost power mid-window — and this feature is
// INVISIBLE, so the leak would accumulate silently, which is exactly the
// failure this house refuses to ship. ext2 already solved it: the
// superblock's s_last_orphan holds the head of a chain, each orphan storing
// the next one's inode number in i_dtime (free real estate — a file that
// still has a reader has no deletion time yet). Two things fall out:
//
//   1. WE replay the chain at mount and reclaim whatever a crash left.
//   2. e2fsck knows this field by heart and will clear a chain itself.
//
// (2) is the whole reason to use the real format instead of inventing a
// private list: it hands an otherwise unverifiable feature an independent
// judge. If this code is wrong, e2fsck says so, in words we did not write.
//
// SCOPE: regular files only. An open DIRECTORY still refuses removal — a
// directory handle is mid-walk through blocks, its parent's link count is
// in play, and no consumer has asked. The line is drawn where the demand is.

// Chain `ino` onto the orphan list. The caller has already removed its last
// NAME and holds write_lock; `node` is its inode, which this finishes
// (links_count 0) and writes.
//
// ORDER: the inode goes down FIRST carrying its next-pointer, THEN the
// superblock head that reaches it. A crash between the two leaves an inode
// off the list — one leaked inode e2fsck reclaims. The other order would
// leave the HEAD pointing at an inode whose i_dtime is not yet a next
// pointer: a chain into garbage, which is a worse thing to hand fsck.
static int ext2_orphan_add(vfs_filesystem_t *fs, ext2_fs_t *e,
                           uint32_t ino, ext2_inode_t *node)
{
	uint32_t old_head = e->sb.s_last_orphan;
	node->i_links_count = 0;
	node->i_dtime = old_head;   // 0 terminates the chain
	if (ext2_write_inode_disk(fs, e, ino, node) != 0)
		return -1;

	e->sb.s_last_orphan = ino;
	if (ext2_sb_writeback(fs, e) != 0)
	{
		// The disk still names old_head. Keep the cache saying the same thing
		// so close/replay cannot walk a candidate that was never published.
		e->sb.s_last_orphan = old_head;
		return -1;
	}

	printd(DEBUG_VFS, "ext2: inode %u orphaned (name gone, %u handle(s) still open)\n",
	       ino, ext2_openref_count(e, ino));
	return 0;
}

// Unchain `ino`. Singly linked, so removal walks from the head — which
// costs nothing real: the list is empty almost always and holds one entry
// the rest of the time. Bounded against a corrupt chain rather than trusted.
//
// The bound matches ext2_orphan_replay's 4096, NOT the open-table size,
// because the chain's length is not actually bounded by open handles: a
// reap that fails (read error, close racing rm — see ext2_rm) leaves its
// inode chained with nobody left to close it, and those stragglers
// accumulate until the next mount replays them. A walk bound smaller than
// replay's would start refusing legitimate removals exactly when the chain
// is at its unhealthiest, compounding the pile-up it should be draining.
static int ext2_orphan_remove(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t ino)
{
	uint32_t cur = e->sb.s_last_orphan;
	uint32_t prev = 0;
	ext2_inode_t node;

	for (uint32_t hops = 0; cur != 0 && hops < 4096; hops++)
	{
		if (ext2_read_inode(fs, e, cur, &node) != 0)
			return -1;
		uint32_t next = node.i_dtime;

		if (cur == ino)
		{
			if (prev == 0)
			{
				e->sb.s_last_orphan = next;
				if (ext2_sb_writeback(fs, e) == 0)
					return 0;
				// The disk still names `cur`; keep the RAM cache saying the
				// same thing so a later retry does not forget the orphan.
				e->sb.s_last_orphan = cur;
				return -1;
			}
			ext2_inode_t prev_node;
			if (ext2_read_inode(fs, e, prev, &prev_node) != 0)
				return -1;
			prev_node.i_dtime = next;
			return ext2_write_inode_disk(fs, e, prev, &prev_node);
		}
		prev = cur;
		cur = next;
	}
	return -1;   // not on the list (or the chain is nonsense)
}

// Destroy an inode's storage: the teardown ext2_rm does inline when nobody
// holds the file. Factored out because the orphan path needs the identical
// sequence at a completely different moment, and two copies of "free an
// inode" is how one of them quietly stops matching the other.
static int ext2_inode_release(vfs_filesystem_t *fs, ext2_fs_t *e,
                              uint32_t ino, ext2_inode_t *node)
{
	bool is_dir = (node->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
	ext2_inode_t old = *node;

	memset(node->i_block, 0, sizeof(node->i_block));
	node->i_links_count = 0;
	node->i_size = 0;
	node->i_blocks = 0;
	node->i_dtime = (uint32_t)kSystemCurrentTime;   // a REAL deletion time now
	if (ext2_write_inode_disk(fs, e, ino, node) != 0)
	{
		// Nothing was dereferenced on disk, so the caller may safely put the
		// original inode (including its orphan next-pointer) back on the list.
		*node = old;
		return -1;
	}

	int release_rc = ext2_free_inode_blocks(fs, e, &old);
	if (release_rc != 0)
	{
		// Restore the replayable map to the caller; orphan_add will persist it
		// before relinking the inode so a later mount can finish the release.
		// Propagate the special result too: if a parent-pointer write failed
		// after its child became free, the caller must persist this map BEFORE
		// publishing read-only state, then stop block reuse until replay.
		*node = old;
		return release_rc;
	}
	// Propagated for the same reason: if this clears the inode's bitmap bit but
	// not its ledgers, the caller must orphan the record BEFORE demoting, and
	// it can only know to demote if the reusable result reaches it.
	return ext2_free_inode(fs, e, ino, is_dir);
}

// Release an inode that is STILL ON the orphan chain. This ordering is the
// crash-safe half of the list's contract:
//
//   1. release its blocks while the inode still records the retry map;
//   2. persist the completed zero map while i_dtime still names the successor;
//   3. free the inode bitmap bit (idempotent if replay repeats it);
//   4. LAST, unlink the orphan from the durable chain.
//
// A crash before step 4 therefore leaves a record mount replay can revisit.
// Bitmap frees are idempotent specifically so a crash between a completed
// bitmap write and the next metadata write is safe to replay.
static int ext2_orphan_release(vfs_filesystem_t *fs, ext2_fs_t *e,
                               uint32_t ino, ext2_inode_t *node)
{
	bool is_dir = (node->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
	ext2_inode_t remaining = *node;

	int release_rc = ext2_free_inode_blocks(fs, e, &remaining);
	if (release_rc != 0)
	{
		// Persist the replayable map for a later close/mount retry. If it still
		// reaches an already-free child because the parent-pointer write failed,
		// stop allocation in this mount after the record is safely on disk.
		if (ext2_write_inode_disk(fs, e, ino, &remaining) != 0)
		{
			// Scope note (all four demotions in this function): the ambiguity
			// lives in THIS filesystem's bitmaps and only this filesystem's
			// allocator can act on it, so the pens come away from this mount.
			// Halting healthy sibling mounts — /home, the FAT lifeboat — would
			// protect nothing and take away the disks an operator analyzes with.
			// (Same scope the closed-rename path chose for the same hazard.)
			EXT2_ALARM("ext2: could not persist orphan %u release progress — demoting its mount to read-only\n", ino);
			vfs_demote_mount_readonly(fs);
		}
		else if (ext2_free_left_storage_reusable(release_rc))
		{
			// The map is safely on disk; NOW stop the allocator. Any reusable
			// result qualifies, not just the parent-pointer one: a block whose
			// bitmap write landed while its count writeback did not is every
			// bit as allocatable as one whose parent still points at it.
			EXT2_ALARM("ext2: orphan %u retry map names storage the bitmap already calls free — demoting its mount to read-only\n", ino);
			vfs_demote_mount_readonly(fs);
		}
		*node = remaining;
		return -1;
	}

	remaining.i_links_count = 0;
	remaining.i_size = 0;
	remaining.i_blocks = 0;
	// Do NOT replace i_dtime with a deletion timestamp yet: while the inode
	// is linked, that field is the durable pointer to the next orphan.
	if (ext2_write_inode_disk(fs, e, ino, &remaining) != 0)
	{
		// Blocks are now reusable but the disk inode may still name them. Stop
		// all further allocation; after reboot, replay runs before allocation
		// and the idempotent frees safely finish this inode.
		EXT2_ALARM("ext2: could not persist completed orphan %u block release — demoting its mount to read-only\n", ino);
		vfs_demote_mount_readonly(fs);
		return -1;
	}
	*node = remaining;

	// Free while still linked. If power fails after the bitmap lands, replay
	// sees the intact inode-table record, observes an already-free bit, and
	// continues to the final unlink before any allocation is permitted.
	int inode_rc = ext2_free_inode(fs, e, ino, is_dir);
	if (inode_rc != 0)
	{
		// A CLEAN failure is the benign one: the bit is still set, so this
		// inode number is nobody's to take, the chain still names it, and the
		// next mount finishes the job. The reusable result is the dangerous
		// twin and the one this branch exists for — the bitmap already calls
		// the inode free while the chain still names it for teardown, so an
		// allocator handing that number to a new file would arrange for replay
		// to release the NEW file's storage. Stop allocation until replay.
		if (ext2_free_left_storage_reusable(inode_rc))
		{
			EXT2_ALARM("ext2: orphan %u freed its inode bitmap bit but not its ledgers — demoting its mount to read-only\n", ino);
			vfs_demote_mount_readonly(fs);
		}
		return -1;
	}

	if (ext2_orphan_remove(fs, e, ino) != 0)
	{
		// The chain may still name an inode whose bitmap bit is free. That is
		// replay-safe but not safe for continued allocation in this mount.
		EXT2_ALARM("ext2: could not unlink released orphan %u — demoting its mount to read-only\n", ino);
		vfs_demote_mount_readonly(fs);
		return -1;
	}

	// Now that no chain entry needs i_dtime as a successor pointer, give the
	// freed inode its real deletion timestamp. write_lock still excludes an
	// allocator from reusing this inode until the table update is complete.
	remaining.i_dtime = (uint32_t)kSystemCurrentTime;
	if (ext2_write_inode_disk(fs, e, ino, &remaining) != 0)
	{
		EXT2_ALARM("ext2: could not stamp released orphan %u with its deletion time — run e2fsck\n", ino);
		return -1;
	}
	*node = remaining;
	return 0;
}

// Last handle on `ino` just closed. If it is a pending orphan, this is the
// moment its storage goes back. Called from ext2_close OUTSIDE open_lock:
// taking write_lock while holding open_lock would invert the order every
// other path here uses.
void ext2_orphan_reap_if_pending(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t ino)
{
	// The cheap no: an empty list is the overwhelmingly common case, and it
	// costs one compare rather than a lock and a disk read.
	if (e->sb.s_last_orphan == 0)
		return;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);
	if (fs->read_only)
		goto done;

	ext2_inode_t node;
	if (ext2_read_inode(fs, e, ino, &node) != 0)
		goto done;
	// A live inode is not ours to collect. Only something whose last name is
	// already gone reaches zero links while still being open.
	if (node.i_links_count != 0)
		goto done;
	if (ext2_orphan_release(fs, e, ino, &node) != 0)
		goto done;
	printd(DEBUG_VFS, "ext2: orphaned inode %u reaped at last close\n", ino);

done:
	spinlock_release_irqrestore(&e->write_lock, lock_flags);
}

// Mount-time replay: whatever the last mount was still holding open when it
// died. Runs before anything can open a file, so no handle can exist for
// these; every one of them is storage nobody will ever reach again.
//
// SPEAKS WHEN IT FINDS ANYTHING. A silent reclaim would make the one event
// that proves this whole mechanism works — a crash, recovered — look exactly
// like a boot where nothing happened.
void ext2_orphan_replay(vfs_filesystem_t *fs, ext2_fs_t *e)
{
	if (e->sb.s_last_orphan == 0)
		return;

	if (e->forced_ro || fs->read_only || fs->fops == NULL || fs->fops->write == NULL)
	{
		// We can see the debt and cannot pay it. Say so — an unannounced
		// leak on a read-only mount is still a leak.
		EXT2_ALARM("ext2: orphaned inode(s) from a previous mount, but this mount is READ-ONLY — run e2fsck\n");
		return;
	}

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);

	uint32_t reaped = 0;
	// Bounded: a corrupt chain must not become an infinite mount.
	while (e->sb.s_last_orphan != 0 && reaped < 4096)
	{
		uint32_t ino = e->sb.s_last_orphan;
		ext2_inode_t node;
		if (ext2_read_inode(fs, e, ino, &node) != 0)
			break;

		if (ext2_orphan_release(fs, e, ino, &node) != 0)
			break;
		reaped++;
	}

	spinlock_release_irqrestore(&e->write_lock, lock_flags);

	if (reaped > 0)
		EXT2_ALARM("ext2: reaped %u orphaned inode(s) left by the previous mount\n", reaped);
}

// os64's ONE removal verb (fops->rm): a file OR an empty directory. The
// ABI comment over SYSCALL_UNLINK promised this driver would be built to
// that contract the morning it was ratified; here it is, same afternoon.
// (FatFs's f_unlink grants the same shape natively — see fat_rm.)
static int ext2_rm(const char *filename, vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);
	if (vfs_fs->read_only)
		goto refuse;

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

	// Ruling 5 (2026-08-04) refused ANY open target. That refusal was right
	// for two years' worth of consumers and wrong for the first one that
	// mattered: replacing a running program. Since 2026-08-16 an open
	// REGULAR file is unlinked from its parent and ORPHANED — the name goes
	// now, the storage goes at last close (see the orphan section above).
	// An open DIRECTORY still refuses: nobody has asked, and a handle
	// mid-walk through a directory's blocks is a harder promise to keep.
	bool orphan_it = false;
	if (ext2_openref_count(e, child_ino) > 0)
	{
		if ((child.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG)
		{
			printd(DEBUG_VFS, "ext2: refusing rm of '%s' (inode %u) — open elsewhere and not a regular file (busy)\n",
			       name, child_ino);
			goto refuse;
		}
		// THE RACE THIS TOLERATES, on purpose: the holder can close between
		// this count read and ext2_orphan_add below (the close path never
		// takes write_lock just to close). Its reap then finds either a
		// still-nonzero links_count or no chain entry yet, does nothing, and
		// the inode we chain a moment later has nobody left to close it. The
		// cost is storage stranded until the NEXT MOUNT's replay — bounded,
		// self-healing, and announced when it happens. Closing the window
		// for real would mean holding open_lock across directory-block I/O,
		// which inverts the lock order everything else here lives by.
		orphan_it = true;
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
		else if (orphan_it)
		{
			// The name is gone but a reader is still here. Park the inode on
			// the orphan chain; ext2_close reaps it when the last handle
			// goes, and a mount after a crash reaps it instead.
			if (ext2_orphan_add(vfs_fs, e, child_ino, &child) != 0)
				goto refuse;
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
	if (vfs_fs->read_only)
		goto refuse;

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
	uint32_t block = ext2_alloc_block(vfs_fs, e, goal_group, false, NULL);
	if (block == 0)
	{
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}
	uint8_t *buf = wr_scratch_get(e);
	if (buf == NULL)
	{
		ext2_free_block(vfs_fs, e, block, NULL);
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
		ext2_free_block(vfs_fs, e, block, NULL);
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
		ext2_free_block(vfs_fs, e, block, NULL);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}

	// Parent last: its links bump (the new "..") rides dir_insert's
	// parent-inode write.
	parent.i_links_count++;
	if (ext2_dir_insert(vfs_fs, e, parent_ino, &parent, name, len,
	                    ino, EXT2_FT_DIR) != 0)
	{
		ext2_free_block(vfs_fs, e, block, NULL);
		ext2_free_inode(vfs_fs, e, ino, true);
		goto refuse;
	}

	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return 0;

refuse:
	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return -1;
}

// ── rename ──────────────────────────────────────────────────────────────────

// Is `maybe_ancestor` at or above `start_dir` in the tree? Walks ".." upward
// from start_dir (INCLUSIVE of start_dir itself) until the root, whose ".."
// points at the root.
//
// This exists for exactly one move: `mv a a/b/c`. Renaming a directory into
// its own descendant detaches the whole subtree into a ring that nothing in
// the tree points at — the classic filesystem loop, and one of the few
// things a rename can do that e2fsck cannot silently repair (it reattaches
// the wreckage under lost+found and the human works out what happened).
// Unix has refused this since 4.2BSD gave us rename at all; so do we.
//
// Refuses on ANY doubt — an unreadable inode or a pathological depth both
// return true (i.e. "treat as ancestor, refuse the move"). A rename we
// decline costs a puzzled user one error; a loop we create costs a fsck.
static bool ext2_is_ancestor(vfs_filesystem_t *fs, ext2_fs_t *e,
                             uint32_t maybe_ancestor, uint32_t start_dir)
{
	uint32_t cur = start_dir;
	for (uint32_t hops = 0; hops < 4096; hops++)
	{
		if (cur == maybe_ancestor)
			return true;
		ext2_inode_t node;
		if (ext2_read_inode(fs, e, cur, &node) != 0)
			return true;   // can't prove it safe — refuse
		uint32_t up = ext2_dir_find(fs, e, &node, "..", 2);
		if (up == 0 || up == cur)
			return false;  // reached the root: its ".." is itself
		cur = up;
	}
	return true;   // absurd depth, or a loop already on disk — refuse
}

// os64's rename verb (fops->rename): give a file a different name, possibly
// in a different directory of the SAME filesystem (syscall_rename refuses
// cross-mount before we ever see it).
//
// THE RULING (Chris, 2026-08-16) is ATOMIC REPLACE: if the destination name
// already holds a regular file, it is replaced, and at no instant does the
// destination name fail to resolve. That guarantee is the entire reason
// rename(2) was invented — 4.2BSD added it because the link-then-unlink
// idiom everyone was using had a window in the middle, and every "write a
// new version safely" recipe since (editors, package managers, and now
// os64get) is built on closing it.
//
// OPEN FILES, revised 2026-08-16 (the orphan slice, same afternoon):
//   - an open SOURCE is fine, and the original refusal here was simply
//     stricter than the facts. An ext2 handle holds an INODE NUMBER, not a
//     path (ext2_handle_t), so a reader is entirely unaffected by what its
//     file is called; renaming out from under one changes nothing it can
//     observe. The rule was inherited from rm, where it was load-bearing,
//     and repeated here where it never was. Directories still refuse, since
//     a directory handle IS mid-walk through the thing being moved.
//   - an open DESTINATION is the interesting one: it gets REPLACED, and its
//     displaced inode goes on the orphan chain instead of being freed, so
//     the program still reading it keeps reading it. That is what lets
//     `os64 refresh` put a new /bin/husk in place while husk is running.
//
// The refusals that survive:
//   - the destination is a DIRECTORY, or the source is a directory and the
//     destination exists at all. Replacement is file-onto-file ONLY; a
//     rename that quietly removes a directory, or swaps a directory in
//     where a file was, is a surprise with no upside.
//   - an open DIRECTORY on either side (see above).
// Symlinks, devices and the other exotic modes are refused outright, for
// the same reason ext2_rm refuses them: we do not know their storage rules
// well enough to move them safely.
//
// ORDER OF OPERATIONS, and why the link count goes UP before it goes down:
//
//   1. src.i_links_count++            (count 2, names 1 — over by one)
//   2. publish the new name           (count 2, names 2 — consistent)
//   3. remove the old name            (count 2, names 1 — over by one)
//   4. src.i_links_count--            (count 1, names 1 — consistent)
//
// A crash at any point leaves the link count EQUAL TO OR GREATER THAN the
// number of names, never less. That direction is the safe one: an inode
// with a spare link is a thing e2fsck notices and corrects, while an inode
// with a missing link is a live file the allocator is entitled to hand out
// from under you. e2fsck staying green is the constitution for a writable
// root, so the two extra inode writes are cheap insurance.
static int ext2_rename(const char *oldpath, const char *newpath,
                       vfs_filesystem_t *vfs_fs)
{
	ext2_fs_t *e = (ext2_fs_t *)vfs_fs->fs_specific;

	uint64_t lock_flags = spinlock_acquire_irqsave(&e->write_lock);
	if (vfs_fs->read_only)
		goto refuse;

	// Resolve both ends. split_path also refuses "." and ".." leaves and
	// verifies each parent really is a directory, so those cases never
	// reach the surgery below.
	ext2_inode_t old_parent, new_parent_storage;
	const char *old_name, *new_name;
	uint32_t old_len, new_len;

	uint32_t old_parent_ino = ext2_split_path(vfs_fs, e, oldpath, &old_parent,
	                                          &old_name, &old_len);
	if (old_parent_ino == 0)
		goto refuse;
	uint32_t new_parent_ino = ext2_split_path(vfs_fs, e, newpath, &new_parent_storage,
	                                          &new_name, &new_len);
	if (new_parent_ino == 0)
		goto refuse;

	// ONE in-memory inode per on-disk inode. When both names live in the
	// same directory, `old_parent` and `new_parent_storage` are two copies
	// of the same thing, and writing one then the other silently discards
	// whichever update went first (dir_insert's size growth, say, undone by
	// dir_remove's stale copy). Alias instead of copying.
	bool same_parent = (old_parent_ino == new_parent_ino);
	ext2_inode_t *np = same_parent ? &old_parent : &new_parent_storage;

	uint32_t src_ino = ext2_dir_find(vfs_fs, e, &old_parent, old_name, old_len);
	if (src_ino == 0)
		goto refuse;   // nothing by that name

	ext2_inode_t src;
	if (ext2_read_inode(vfs_fs, e, src_ino, &src) != 0)
		goto refuse;

	uint32_t src_kind = src.i_mode & EXT2_S_IFMT;
	if (src_kind != EXT2_S_IFREG && src_kind != EXT2_S_IFDIR)
	{
		printd(DEBUG_VFS, "ext2: refusing rename of '%s' — inode %u is neither file nor directory (mode 0x%04x)\n",
		       oldpath, src_ino, src.i_mode);
		goto refuse;
	}

	// An open SOURCE is only a problem for a DIRECTORY. A file's reader holds
	// an inode number, not a name (ext2_handle_t), so it cannot tell that its
	// file was renamed and has nothing to be protected from; a directory's
	// reader is walking the very blocks a move rearranges the context of.
	if (src_kind == EXT2_S_IFDIR && ext2_openref_count(e, src_ino) > 0)
	{
		printd(DEBUG_VFS, "ext2: refusing rename of directory '%s' (inode %u) — open elsewhere (busy)\n",
		       oldpath, src_ino);
		goto refuse;
	}

	// rename("a", "a"): the caller asked for a state the disk is already in.
	// Succeed without touching anything — doing the surgery would mean
	// removing the only name for an inode we just published under itself.
	if (same_parent && old_len == new_len &&
	    memcmp(old_name, new_name, old_len) == 0)
	{
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return 0;
	}

	bool src_is_dir = (src_kind == EXT2_S_IFDIR);

	// The destination, if anything is already there.
	ext2_inode_t dst;
	bool replacing = false;
	bool dst_open = false;   // ...and is somebody still reading what we displace?
	uint32_t dst_ino = ext2_dir_find(vfs_fs, e, np, new_name, new_len);
	if (dst_ino != 0)
	{
		// Both names already denote the same inode — a hard link, which os64
		// cannot create but a disk written elsewhere can carry. The request
		// is already satisfied; removing the old name would DROP a link the
		// caller never asked to lose.
		if (dst_ino == src_ino)
		{
			spinlock_release_irqrestore(&e->write_lock, lock_flags);
			return 0;
		}
		if (ext2_read_inode(vfs_fs, e, dst_ino, &dst) != 0)
			goto refuse;
		if ((dst.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG || src_is_dir)
		{
			printd(DEBUG_VFS, "ext2: refusing rename onto '%s' — replacement is file-onto-file only (dest mode 0x%04x, source mode 0x%04x)\n",
			       newpath, dst.i_mode, src.i_mode);
			goto refuse;
		}
		// An open DESTINATION is no longer a refusal — it is the whole point.
		// Note it now; step 6 sends the displaced inode to the orphan chain
		// instead of freeing it, so whoever is reading it keeps reading it.
		dst_open = ext2_openref_count(e, dst_ino) > 0;
		replacing = true;
	}

	// `mv a a/b` — see ext2_is_ancestor. Only a directory can loop, and only
	// when it is actually changing parents.
	if (src_is_dir && !same_parent &&
	    ext2_is_ancestor(vfs_fs, e, src_ino, new_parent_ino))
	{
		printd(DEBUG_VFS, "ext2: refusing rename of '%s' into its own descendant '%s'\n",
		       oldpath, newpath);
		goto refuse;
	}

	uint32_t now = (uint32_t)kSystemCurrentTime;
	uint8_t file_type = src_is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

	// ── Step 1: over-count, so every later crash window is the safe kind ──
	src.i_links_count++;
	src.i_ctime = now;
	if (ext2_write_inode_disk(vfs_fs, e, src_ino, &src) != 0)
		goto refuse;

	// ── Step 2: publish the new name. THIS is the atomic moment ──────────
	if (replacing)
	{
		// One block write swings the existing entry onto our inode. The
		// replaced inode is now nameless but still counted — step 6 collects
		// it, and a crash before that leaves e2fsck an unreferenced inode to
		// reclaim, never a live file with no link.
		if (ext2_dir_repoint(vfs_fs, e, np, new_name, new_len,
		                     src_ino, file_type) != 0)
			goto unwind;
		np->i_mtime = now;
		ext2_write_inode_disk(vfs_fs, e, new_parent_ino, np);
	}
	else
	{
		// A directory arriving in a new parent brings a ".." that will point
		// at it — bump BEFORE dir_insert, which writes the parent inode as
		// part of its own work (mkdir does exactly this).
		if (src_is_dir && !same_parent)
			np->i_links_count++;
		if (ext2_dir_insert(vfs_fs, e, new_parent_ino, np, new_name, new_len,
		                    src_ino, file_type) != 0)
		{
			if (src_is_dir && !same_parent)
				np->i_links_count--;
			goto unwind;
		}
	}

	// ── Step 3: the old name goes away ───────────────────────────────────
	if (ext2_dir_remove(vfs_fs, e, old_parent_ino, &old_parent,
	                    old_name, old_len) == 0)
	{
		// The new name is already published and the link count already says
		// two names, so the filesystem is CONSISTENT — it just has one more
		// name than the caller wanted. Refusing to unwind here is deliberate:
		// un-publishing would reopen the very window this design exists to
		// close. Say so loudly and report failure honestly.
		printd(DEBUG_VFS, "ext2: rename '%s' -> '%s': new name published but OLD NAME COULD NOT BE REMOVED — both names now exist (inode %u)\n",
		       oldpath, newpath, src_ino);
		spinlock_release_irqrestore(&e->write_lock, lock_flags);
		return -1;
	}

	old_parent.i_mtime = now;
	if (src_is_dir && !same_parent && old_parent.i_links_count > 0)
		old_parent.i_links_count--;   // the moved dir's ".." left this parent
	ext2_write_inode_disk(vfs_fs, e, old_parent_ino, &old_parent);

	// ── Step 4: the moved directory's ".." follows it ────────────────────
	// Done after the name surgery because it is the one piece of state that
	// is wrong-but-harmless in between: a directory whose ".." names its old
	// parent is still reachable, still walkable, and fsck's to correct.
	if (src_is_dir && !same_parent)
	{
		if (ext2_dir_repoint(vfs_fs, e, &src, "..", 2,
		                     new_parent_ino, EXT2_FT_DIR) != 0)
			printd(DEBUG_VFS, "ext2: rename '%s' -> '%s': moved directory's '..' still names inode %u\n",
			       oldpath, newpath, old_parent_ino);
	}

	// ── Step 5: give back the transient link ─────────────────────────────
	src.i_links_count--;
	src.i_ctime = now;
	ext2_write_inode_disk(vfs_fs, e, src_ino, &src);

	// ── Step 6: collect whatever we replaced ─────────────────────────────
	// Same teardown as ext2_rm's regular-file branch, and for the same
	// reason it respects a count above one: the disk can carry hard links
	// this OS cannot make, and freeing a still-linked inode is corruption
	// wearing a tidy-up's clothes.
	if (replacing)
	{
		if (dst.i_links_count > 1)
		{
			dst.i_links_count--;
			dst.i_ctime = now;
			ext2_write_inode_disk(vfs_fs, e, dst_ino, &dst);
		}
		else if (dst_open)
		{
			// THE UPGRADE-IN-PLACE CASE. Its last name just became ours, but
			// a program is still reading it — most likely running it. Park it
			// on the orphan chain: the pages it is demand-paging stay exactly
			// where they are, and the storage comes back at last close (or at
			// the next mount, if the machine dies first).
			if (ext2_orphan_add(vfs_fs, e, dst_ino, &dst) != 0)
				printd(DEBUG_VFS, "ext2: rename '%s' -> '%s': displaced inode %u could NOT be orphaned — it will need e2fsck\n",
				       oldpath, newpath, dst_ino);
		}
		else
		{
			int release_rc = ext2_inode_release(vfs_fs, e, dst_ino, &dst);
			if (release_rc != 0)
			{
				// The retry map must reach disk while writes are still permitted.
				// Demoting first turns orphan_add's inode/superblock writes into a
				// block-layer panic and loses the recovery path this map provides.
				int orphan_rc = ext2_orphan_add(vfs_fs, e, dst_ino, &dst);
				if (orphan_rc != 0)
					printd(DEBUG_VFS, "ext2: rename '%s' -> '%s': displaced inode %u could not be released or orphaned — run e2fsck\n",
					       oldpath, newpath, dst_ino);

				// Every reusable result, not just the indirect-parent one: a
				// half-completed block OR inode free leaves storage the orphan
				// record still names sitting in the allocator's free pool.
				if (ext2_free_left_storage_reusable(release_rc))
				{
					EXT2_ALARM("ext2: inode %u retry map %s after a partial release — forcing mount read-only\n",
					       dst_ino, orphan_rc == 0 ? "persisted" : "could not be persisted");
					vfs_demote_mount_readonly(vfs_fs);
				}
			}
		}
	}

	printd(DEBUG_VFS, "ext2: renamed '%s' -> '%s' (inode %u%s)\n",
	       oldpath, newpath, src_ino, replacing ? ", replaced" : "");
	spinlock_release_irqrestore(&e->write_lock, lock_flags);
	return 0;

unwind:
	// The new name never got published; take back the link we lent.
	src.i_links_count--;
	ext2_write_inode_disk(vfs_fs, e, src_ino, &src);

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
	ext2_rw_fops.rename  = ext2_rename;  // ...and the one renaming verb

	ext2_rw_dops = ext2_dops;
	ext2_rw_dops.mkdir   = ext2_mkdir;
}
