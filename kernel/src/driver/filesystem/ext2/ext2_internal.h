#ifndef EXT2_INTERNAL_H
#define EXT2_INTERNAL_H

// ext2_internal.h — the PRIVATE seam between the ext2 driver's two halves:
// ext2.c (the read paths and mount, verified 2026-07-18 against a filesystem
// os64 never touched) and ext2_write.c (the write paths, born 2026-08-04 —
// the day os64 wrote its first ext2 byte). Nothing outside this directory
// includes this header; the public face stays ext2_vfs.h (the op tables) and
// vfs.h (ext2_initialize_filesystem). It lives beside the .c files rather
// than in include/ for exactly that reason.

#include <stdint.h>
#include <stdbool.h>
#include "driver/filesystem/ext2/ext2_fs.h"
#include "vfs.h"
#include "spinlock.h"

// i_mode high-nibble type codes. NOTE: deliberately NOT the E_EXT2_INODE_TYPE_T
// enum in ext2_fs.h — that enum holds the DIRECTORY-ENTRY file_type codes
// (1=file, 2=dir), which are a different namespace from i_mode's S_IF* bits.
// Confusing the two reads every regular file as a socket.
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

// The one INCOMPAT feature we understand: dirents carry a file_type byte.
// Anything else in the incompat set means the on-disk format has constructs
// we would misparse — the superblock's own doc says refuse, so we refuse.
#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002

// RO_COMPAT features a WRITER may coexist with. These two are not optional
// generosity — our own test image carries both (mkfs.ext2 authors them by
// default; verified with dumpe2fs 2026-08-04):
//
//   SPARSE_SUPER (0x1): backup superblocks/GDTs live only in select groups.
//   Safe because os64 maintains ONLY the primary superblock + GDT at runtime
//   and never touches backups — which is what Linux itself does; backups are
//   the province of mkfs/fsck/resize.
//
//   LARGE_FILE (0x2): files > 2GB keep size high bits in i_dir_acl. Safe
//   because we never CREATE or EXTEND one (growth is capped at 2GB-1), while
//   in-place overwrites preserve both size words unchanged (see ext2_write).
//
// Any OTHER ro_compat bit means the disk relies on bookkeeping we don't
// maintain — the mount is forced read-only (write slots nulled in the
// per-mount op tables), exactly what "ro_compat" asks of a writer.
#define EXT2_SUPPORTED_RO_COMPAT 0x0003
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002

// ── The open-inode refcount table ───────────────────────────────────────────
// Ruling of 2026-08-04: rm (or a truncating "w" open) of a file another
// handle holds open is REFUSED, not raced. Every open — file or directory,
// read or write mode, on either op table — registers its inode here; every
// close deregisters. A "w" open checks BEFORE registering itself.
//
// WHAT THE COUNT MEANS CHANGED 2026-08-16, and the old comment here said the
// alternative "can be built the day a consumer does" — so here is that day.
// The consumer was replacing /bin/husk over the network while husk runs.
// Now:
//   - an open DIRECTORY still refuses removal or a move (its reader is
//     mid-walk through the blocks in question);
//   - an open regular FILE does not refuse. Its NAME goes immediately and
//     its storage waits for the last close, on the on-disk orphan chain
//     (ext2_write.c). The count is what tells those two moments apart, and
//     ext2_openref_unregister reporting the zero-transition is what fires
//     the reap.
#define EXT2_OPEN_TABLE_SLOTS 64

typedef struct {
	uint32_t ino;      // 0 = free slot
	uint32_t count;    // concurrent handles on this inode
} ext2_open_ref_t;

// ── The per-mount context (fs->fs_specific) ─────────────────────────────────
// Geometry is filled once by ext2_initialize_filesystem and read-only forever
// after. The WRITE ERA (2026-08-04) added three mutable members, each with
// its own discipline — see the locking doctrine atop ext2.c:
//   sb + groups     — free counts mutate under write_lock; write paths are
//                     the only readers of those fields, so read paths stay
//                     lock-free.
//   write_lock      — serializes every write-shaped op, whole-body.
//   open_lock/open_refs — the refcount table above; held only across the
//                     table scan, never across disk I/O.
typedef struct {
	ext2_super_block_t sb;        // cached superblock (the first 1024 bytes of it)
	uint32_t block_size;          // bytes per fs block (1024 << s_log_block_size)
	uint32_t sectors_per_block;   // fs block -> disk sectors
	uint32_t inode_size;          // on-disk inode record size (128 or 256)
	uint32_t groups_count;
	ext2_group_desc_t *groups;    // the whole descriptor table, cached — and
	                              // under write_lock, the SOURCE OF TRUTH the
	                              // on-disk copy is written back from
	uint32_t ptrs_per_block;      // block_size / 4 — the indirection fan-out

	bool forced_ro;               // unknown ro_compat bits ⇒ writes were
	                              // stripped from this mount at initialize
	spinlock_t write_lock;        // one writer at a time, whole-op
	spinlock_t open_lock;         // guards open_refs only — never held across I/O
	ext2_open_ref_t open_refs[EXT2_OPEN_TABLE_SLOTS];

	// ── The write-path scratch pool (2026-08-04 evening, the allocator-abuse
	// affair) ────────────────────────────────────────────────────────────────
	// The write half used to kmalloc/kfree a block-sized scratch for every
	// disk touch — ~1,100 allocations/second under a logd soak. That churn
	// was inherited from the read half, where per-call scratch BUYS
	// lock-freedom; the write half runs whole-body under write_lock, so the
	// purchase bought nothing and the bill was real: the allocator's
	// zero-on-alloc choke point memset a gigabyte of scratch per soak, and
	// the alloc/free stream rode the allocator's next-fit address march
	// straight into the paging-pool exhaustion panic (Chris called the march
	// on a bet; the odometer probes in paging.c hold the receipts).
	//
	// These buffers are allocated ONCE at mount and handed out LIFO by
	// wr_scratch_get/put (ext2_write.c) — exclusively owned by the
	// write_lock holder, so no lock of their own. Deepest nesting is ~6
	// (write's data scratch + the batch's bitmap and leaf, then a chain
	// allocation's parent buffer with its zero-fill scratch or RMW scratch,
	// and the superblock writeback's); 8 leaves headroom, and
	// get() falls back to kmalloc (loudly) rather than fail if a future
	// path nests deeper. NOTE: reused buffers arrive DIRTY — any code that
	// leaned on kmalloc's zero-on-alloc now memsets explicitly (the two
	// sites are commented).
	uint8_t *wr_scratch[8];       // block_size each, mount-time allocation
	int wr_scratch_used;          // LIFO depth currently handed out
} ext2_fs_t;

// One open file OR directory. pos is bytes for files, and for directories the
// byte offset of the NEXT dirent to deliver (dirents never cross a block
// boundary — ext2 pads rec_len to the block end — so block-at-a-time works).
typedef struct {
	ext2_inode_t inode;           // copy of the on-disk inode (128-byte core)
	uint32_t ino;
	uint64_t pos;
	uint64_t size;                // i_size low + i_dir_acl high for regular files
	uint8_t *blockbuf;            // directories: current block's contents
	uint32_t blockbuf_index;      // which file-block blockbuf holds (~0 = none)
} ext2_handle_t;

// ext2 rev 1 stores a regular file's size high word in i_dir_acl. Directories
// still use that field as ACL metadata, so callers use this only after proving
// S_IFREG (the file open/read/write paths do exactly that).
static inline uint64_t ext2_regular_file_size(const ext2_inode_t *inode)
{
	return (uint64_t)inode->i_size | ((uint64_t)inode->i_dir_acl << 32);
}

// ── The block-map cursor (2026-08-20, the cp-to-/dev/null measurement) ──────
//
// ext2_bmap re-walks the indirect chain from scratch for EVERY file block,
// and each level of that walk used to kmalloc a block, read it, and kfree it.
// For sequential reading that is pure waste twice over, and it was measured
// rather than guessed: `cp /pattern2.bin /dev/null` on a 24MB file (24,576
// blocks of 1KB) issued 73,848 block-cache requests — a 3.00x READ
// AMPLIFICATION — and ~49,000 kmalloc/kfree pairs, each one a full linear
// scan of kMemoryStatus under the interrupts-off allocator spinlock. With the
// cache 100% warm and the disk untouched (zero misses) it still ran at
// 7.5 MiB/s: 129us per kilobyte, none of it the disk's fault.
//
// The waste is that consecutive file blocks share their pointer blocks — 256
// of them at 1KB blocks — so the walk re-reads the same indirect block 255
// times out of 256. This cursor holds one scratch buffer PER LEVEL and
// remembers which fs block each currently holds.
//
// ONE SLOT PER LEVEL, not one slot: a double-indirect walk touches the DIND
// block and then the level-2 block it points at, so a single shared slot
// would have the two evicting each other on every single lookup and hit
// exactly never. That mistake is the whole reason this is an array.
//
// Block 0 is the "empty" sentinel: ext2 never uses block 0 as an indirect
// pointer (a zero pointer IS the definition of a hole), so it can never
// collide with a real cached block.
//
// LIFETIME AND STALENESS: a cursor is scoped to ONE read call and freed at
// its end, deliberately. The read paths are lock-free by doctrine, so a
// writer changing this file's block map mid-read is already racing at the
// data level; per-call scope keeps the window exactly as wide as one read()
// and no wider, which is why the cursor does not live in ext2_handle_t.
#define EXT2_BMAP_LEVELS 3

typedef struct
{
	uint8_t  *buf[EXT2_BMAP_LEVELS];    // one block-sized scratch per level
	uint32_t  held[EXT2_BMAP_LEVELS];   // fs block in buf[i]; 0 = empty
} ext2_bmap_cache_t;

// Arm a cursor. Cannot fail and allocates nothing: each level's buffer is
// born LAZILY, the first time the walk actually reaches that level. A read
// that stays inside the 12 direct blocks therefore costs exactly what it did
// before this cursor existed — which is the difference between making big
// reads faster and making small ones pay for it.
//
// If that lazy allocation ever fails, the walk falls back to the old
// allocate-per-lookup path for that level: short on memory means slow again,
// never broken. Always pair with _free (kfree(NULL) panics in os64; _free
// guards, so calling it on a cursor that never allocated is fine).
void ext2_bmap_cache_init(ext2_bmap_cache_t *c);
void ext2_bmap_cache_free(ext2_bmap_cache_t *c);

// ── Shared read-side helpers (ext2.c) ───────────────────────────────────────
// De-static'd for the write half; contracts unchanged — see their bodies.
int      ext2_read_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                            uint32_t block, void *buffer);
int      ext2_read_inode(vfs_filesystem_t *fs, ext2_fs_t *e,
                         uint32_t ino, ext2_inode_t *out);
uint32_t ext2_bmap(vfs_filesystem_t *fs, ext2_fs_t *e,
                   const ext2_inode_t *ino, uint32_t fblock);
// The same walk, with a cursor. `c` may be NULL — that is exactly what
// ext2_bmap passes, which is why every existing caller keeps its old
// behavior to the byte.
uint32_t ext2_bmap_cached(vfs_filesystem_t *fs, ext2_fs_t *e,
                          const ext2_inode_t *ino, uint32_t fblock,
                          ext2_bmap_cache_t *c);
uint32_t ext2_dir_find(vfs_filesystem_t *fs, ext2_fs_t *e,
                       const ext2_inode_t *dir_ino,
                       const char *name, uint32_t len);
uint32_t ext2_resolve_path(vfs_filesystem_t *fs, ext2_fs_t *e,
                           const char *path, ext2_inode_t *out);
// The shared open core: resolve an EXISTING regular file, count it open,
// build the handle. The RO table's open adds only the "r" mode check; the
// RW table's open (ext2_write.c) adds create/truncate/append around it.
int      ext2_open_existing(vfs_file_t **vfs_file, const char *path,
                            vfs_filesystem_t *vfs_fs);
// read_inode with caller-supplied block scratch (the write path passes its
// mount scratch; ext2_read_inode wraps this with a kmalloc for the
// lock-free read paths, where per-call scratch still earns its keep).
int      ext2_read_inode_buf(vfs_filesystem_t *fs, ext2_fs_t *e,
                             uint32_t ino, ext2_inode_t *out, uint8_t *buf);

// ── Open-inode refcount API (ext2.c — both halves call it) ──────────────────
// register: count the inode as open (grows an existing slot or claims a free
// one). Returns false only when the table is full — the open FAILS then,
// loudly, rather than silently escaping the rm protection.
bool     ext2_openref_register(ext2_fs_t *e, uint32_t ino);
// TRUE = that was the LAST handle, and the caller then owes the orphan chain
// a look (ext2_orphan_reap_if_pending) — which must happen OUTSIDE open_lock.
bool     ext2_openref_unregister(ext2_fs_t *e, uint32_t ino);
uint32_t ext2_openref_count(ext2_fs_t *e, uint32_t ino);

// ── The orphan chain (ext2_write.c) ─────────────────────────────────────────
// Inodes whose last NAME is gone while a handle still holds them open — what
// lets a running program be replaced on disk. The list lives ON DISK in the
// superblock's s_last_orphan; see the essay above ext2_orphan_add for why
// that matters more than the convenience of an in-memory one.
// Last close just happened: if `ino` is a pending orphan, free it now.
void ext2_orphan_reap_if_pending(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t ino);
// Mount time: reclaim whatever the previous mount died still holding open.
// Announces on the glass when it finds anything — a silent recovery would
// look exactly like a boot where nothing had ever gone wrong.
void ext2_orphan_replay(vfs_filesystem_t *fs, ext2_fs_t *e);

// ── Write substrate (ext2_write.c) ──────────────────────────────────────────
// The block-level primitives every write-shaped op composes. All return
// 0 / -1 (the house convention); all take their scratch from kmalloc.
int ext2_write_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                        uint32_t block, const void *buffer);
int ext2_rmw_fs_block(vfs_filesystem_t *fs, ext2_fs_t *e,
                      uint32_t block, uint32_t offset,
                      const void *src, uint32_t len);
int ext2_write_inode_disk(vfs_filesystem_t *fs, ext2_fs_t *e,
                          uint32_t ino, const ext2_inode_t *in);
int ext2_sb_writeback(vfs_filesystem_t *fs, ext2_fs_t *e);
int ext2_gd_writeback(vfs_filesystem_t *fs, ext2_fs_t *e, uint32_t group);

#endif // EXT2_INTERNAL_H
