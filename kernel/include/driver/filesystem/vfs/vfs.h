#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dlist.h"
#include "types.h"
#include "os64/dirent.h"   // os64_dirent_t — the fs-neutral dops->read contract
#include "os64/syscall_numbers.h" // OS64_RENAME_* — rename policy crosses this seam


#define DENTRY_ROOT 0xFFFFFFFF    
#define VFS_MAX_OPEN_FILES 512
#define VFS_MAX_OPEN_DIRS 64
#define VFS_FILE_ALLOC_SIZE 65535+FS_FILE_COPYBUFFER_SIZE
#define VFS_MAX_PARTITIONS 128
#define DEFAULT_SECTOR_SIZE 512
// The longest fs-local path the open-file registry keeps — the longest path
// the kernel carries anywhere (task.h's TASK_MAX_PATH_LEN), spelled again
// because vfs.h sits BELOW task.h in the include graph. vfs.c static-asserts
// the two against each other, so raising one and forgetting the other stops
// the build instead of quietly shortening a path.
#define VFS_REG_PATH_MAX 256

#define SEEK_SET	0	/* Seek from beginning of file.  */
#define SEEK_CUR	1	/* Seek from current position.  */
#define SEEK_END	2	/* Seek from end of file.  */

struct vfs_partition_table;

typedef struct directory vfs_directory_t;
typedef struct direntry vfs_dirent_t;
typedef struct dir_operations vfs_directory_operations_t;
typedef struct inode vfs_inode_t;
typedef struct dentry dentry_t;
typedef struct vfsmount vfs_mount_t;
typedef struct inode_operations vfs_inode_operations_t;
typedef struct vfs_filesystem vfs_filesystem_t;
typedef struct inode inode_t;
typedef struct file vfs_file_t;
typedef struct file_operations vfs_file_operations_t;
typedef struct block_device block_device_t;
typedef struct block_operations block_operations_t;
typedef struct vfs_partition_table vfs_partition_table_t;

typedef enum
{
	FILESYSTEM_TYPE_UNDEFINED,
	FILESYSTEM_TYPE_FAT,
	FILESYSTEM_TYPE_FAT32,
	FILESYSTEM_TYPE_EXT2,
	FILESYSTEM_TYPE_NTFS
} e_filesystem_type;

enum whichBus
{
	BUS_NONE,
    BUS_ATA_PRIMARY,
    BUS_ATA_SECONDARY,
    BUS_SATA,
	BUS_NVME
};

typedef enum
{
	FILETYPE_FILE = 1,
	FILETYPE_PIPE = 2,
	FILETYPE_PROCFILE = 3,
	FILETYPE_SYSFILE = 4,   // /sys — the machine as files (sysfs.c)
	FILETYPE_DEVFILE = 5    // /dev — the kernel's objects as files (devfs.c)
} eFileType;

enum whichDrive
{
    master = 0,
    slave = 1
};

typedef struct 
{
    uint16_t ATAIdentifyData[256];
    char ATADeviceModel[80];
    bool queryATAData;
    uint8_t DeviceAvailable;
    int ATADeviceType;
    uint32_t totalSectorCount;
    uint32_t sectorSize;
    bool lbaSupported;
    bool lba48Supported;
    bool dmaSupported;
    enum whichBus bus; 
    enum whichDrive driveNo;
    uintptr_t ioPort;
    uint8_t irqNum;
    uint8_t driveHeadPortDesignation;
	int major;
	block_device_t* block_device;
	void* block_extra_info;
} __attribute__((packed)) block_device_info_t;

// Return CONVENTION (despite the size_t): 0 = success, nonzero = error — NOT
// a sector/byte count. The FAT glue's disk_read/disk_write propagate these
// values directly as FatFs DRESULTs (0 = RES_OK), so a driver that returns a
// count on success reports every successful transfer as a disk error. (An
// nvme write returning void through the (void*) cast made "success" read as
// RAX garbage once — see nvme_vfs_write_disk.)
struct block_operations
{
	//int (*seek) (void *dev, long offset, int origin);
	size_t (*read) (void* device, uint64_t sector, void * buffer, uint64_t sectorCount);
	size_t (*write) (void* device, uint64_t sector, const void * buffer, uint64_t sectorCount);
};

typedef enum
{
	PART_TABLE_TYPE_UNINITIALIZED,
	PART_TABLE_TYPE_UNKNOWN,
	PART_TABLE_TYPE_MBR,
	PART_TABLE_TYPE_GPT,
	PART_TABLE_TYPE_ERROR
} e_part_table_type;

struct block_device
{
	// The device's own story — a model string, a serial, "ramdisk0" —
	// whatever its driver wants the logs to say. Not user-facing.
	char* name;
	// THE SYSTEM'S HANDLE for this device: bus + ordinal — "nvme0", "sata0",
	// "ram0" (and "usb0" the day mass storage arrives). Assigned once by
	// block_assign_dev_names, printed by /sys/block, and COMMITTED as the
	// name /dev block nodes will reuse when they arrive (ruled 2026-08-30,
	// so the two can never disagree — Linux's /dev/disk/by-* sediment is
	// what happens when they do).
	char dev_name[12];
	block_device_info_t* device;
	block_operations_t* ops;
	int part_count;
	e_part_table_type partTableType;
	vfs_partition_table_t* partition_table;
};

struct inode
{
	unsigned int            i_dev;          //12 bits major, 20 bits minor
	unsigned short          i_mode;
	unsigned short          i_opflags;
	unsigned int            i_uid;
	unsigned int            i_gid;
	unsigned int            i_flags;
	const vfs_inode_operations_t   *i_op;
	struct vfsmount         *i_vfsmount;
};

struct vfs_inode_operations
{
	int (*create) (vfs_inode_t *,dentry_t *);
	int (*mkdir) (vfs_inode_t *,dentry_t *);
	int (*rmdir) (vfs_inode_t *,dentry_t *);
	int (*mknod) (vfs_inode_t *,dentry_t);
	int (*rename) (vfs_inode_t *, dentry_t *,inode_t *, dentry_t *, unsigned int);
};

struct vfsmount 
{
	dentry_t *mnt_root;        /* root of the mounted tree */
	struct super_block *mnt_sb;     /* pointer to superblock */
	int mnt_flags;
};

struct directory
{
	char* f_path;
	inode_t* f_inode;
	vfs_directory_operations_t* dops;
	void* handle;
	dlist_t listEntry;
	void *owner;
	// Mount-aware readdir (tagged by syscall_open, consumed by
	// syscall_readdir): the mount points living directly under this
	// directory ("/fat" when listing "/", "/mnt/stick" when listing "/mnt")
	// are served as SYNTHETIC entries after the filesystem's own entries run
	// out. A mount point is namespace routing, not directory content — the
	// fs on disk has no such entry to return, so the VFS must speak for it
	// or `ls` can't see what `cd` can reach. mount_prefix is the directory's
	// own CANONICAL path, a kmalloc'd copy owned by this struct — set by
	// syscall_open for every directory it opens, freed by
	// handle_dir_object_close, NULL for kernel-internal opendirs (the
	// allocator's zeroing). It stopped pointing into kMountTable the day
	// mounts learned to unmount; mount_scan is the resume cursor for the
	// synthetic phase.
	const char *mount_prefix;
	size_t mount_prefix_len;
	int mount_scan;
	//arena_t* arena;
};

struct dir_operations
{
	int (*open) (vfs_directory_t** vfs_dir, const char* path, vfs_filesystem_t* vfs_fs);
	// Fills ONE fs-neutral entry per call. Returns 1 = entry produced,
	// 0 = end of directory, <0 = error (the os64_dirent_t contract — see
	// abi/include/os64/dirent.h). The driver translates its own on-disk
	// shape (FILINFO, ext2_dir_entry_2, ...) internally; fs-specific structs
	// never cross this seam. (The old signature passed a raw FILINFO through
	// void* — a contract no second filesystem could ever have implemented.)
	int (*read) (vfs_directory_t* vfs_dir, os64_dirent_t* entry);
	int (*close) (vfs_directory_t* vfs_dir);
	int (*mkdir) (char* path, vfs_filesystem_t* vfs_f);
	// stat is readdir for exactly one name: fill the SAME os64_dirent_t that
	// read() yields, for the object at `path` — file OR directory. (It lives
	// in dops because the dirent is directory-entry vocabulary, not because
	// the target must be a directory.) Returns 0 = filled, <0 = no such
	// path. The path arrives fs-local — mount prefix already stripped.
	int (*stat) (const char* path, os64_dirent_t* entry, vfs_filesystem_t* vfs_fs);
};
	
struct dentry
{
	char* d_name;
	struct inode* d_inode;
	dentry_t* d_parent;
};

typedef struct
{
    uint32_t partStartSector; //LBA address of partition
    uint32_t partEndSector;
    uint32_t partTotalSectors;
	uuid_t partTypeGUID;
	uuid_t uniquePartGUID;
    bool bootable;
    uint8_t systemID;
	char partName[36];
	e_filesystem_type filesystemType;
	block_device_info_t* block_device_info;
} partEntry_t;

struct vfs_partition_table
{
    partEntry_t* parts[VFS_MAX_PARTITIONS];
    int partCount;
    uint8_t diskID[10];
    bool validBootSector;
} __attribute__((packed));

// fatDiskNumber sentinel for non-FAT filesystems. Real FAT mounts get 1, 2, …
// (++kFatDiskNumber); everything else must hold a value FatFs can never present
// as a pdrv that resolves. Zero is NOT that value — kRegisterFilesystem memsets
// the struct, and a stale/leaked pdrv of 0 once matched the EXT2 ROOT in the
// dlist walk, aiming FAT-relative sector writes at the root partition (the
// 2026-07-26 root-inode corruption hunt). vfs_get_device_by_fat_disk_number
// refuses to match this value outright.
#define FAT_DISK_NONE 0xFF

struct vfs_filesystem
{
	vfs_mount_t *mount; 
	vfs_inode_operations_t* iops;
	//Block operations
	block_operations_t* bops;
	//File operations
	vfs_file_operations_t* fops;
	vfs_directory_operations_t* dops;
	dlist_t inode_list;
	vfs_file_t *files;
	vfs_directory_t *dirs;
	char* vfsReadBuffer, *vfsWriteBuffer;
	int partNumber;
	int blockSize;
	int inodes_per_block;
	int inode_table_blocks;
	void* super_block;
	void* block_group_descriptor;
	void* root_dir_inode;
	block_device_info_t* block_device_info;	
	uint8_t major;
	uint8_t minor;
	uint8_t fatDiskNumber;
	// The mount-level authority for runtime demotion. Function slots are also
	// stripped for fast syscall refusal, but retained/saved callbacks must
	// independently honor this state before performing any mutation.
	bool read_only;
	// Open DIRECTORY handles on this mount. Directories have no refcount and
	// no registry (files have both), so unmount's busy check needs a count
	// kept at the two doors every user-held directory passes through:
	// syscall_open's dir branch (++, under the path gate) and
	// handle_dir_object_close (--). Kernel-internal opendirs (boot-time
	// tests) don't count and don't need to: they never overlap an unmount
	// syscall. __sync-adjusted; read with the gate held.
	int open_dir_count;
	void* fs_specific;
};

struct file
{
	eFileType filetype;
	char* f_path;
	inode_t* f_inode;

	// WHICH FILE THIS HANDLE IS OPEN ON — a filesystem-local identity, not a
	// path. The driver fills it at open: ext2 writes the inode number, FAT the
	// start cluster. Zero means "this filesystem has no identity to give"
	// (procfs, sysfs, devfs, pipes — none of which can host a program).
	//
	// It exists because a path is not a file, and the shared-object registry
	// (shared_object.c) has to tell the difference: it caches a program's
	// relocated pages under its path, and `os64get` replaces a binary by
	// renaming a new inode over the old name, so the path compares equal while
	// the file is a different one entirely.
	//
	// THE NUMBER IS ONLY MEANINGFUL WHILE THE HANDLE IS OPEN — which is
	// exactly when anyone can ask. Both filesystems refuse to recycle the
	// identity of a file somebody holds open (ext2 keeps an unlinked inode
	// allocated on its orphan chain until last close; FatFs's lock table
	// refuses to unlink an open file), so a live handle pins its own answer.
	uint64_t f_ident;

	vfs_file_operations_t* fops;
	void* handle;
	void *pipe, *pipeContent, **pipeContentPtr;
	void *copyBuffer;
	uint32_t verification;
	void *owner;
	// HANDLE-LAYER refcount (task.h handle table, NOT a VFS concept): how many
	// task handles reference this open file. Spawn redirection shares one open
	// file between parent and child (same pattern as pipe end refcounts), and
	// only the LAST close may run fops->close — otherwise the parent closing
	// its copy frees the FIL out from under the child. Managed exclusively by
	// syscall_open (=1), spawn_do_create (++), and handle_file_object_close
	// (--, close at 0); kernel-internal users that call fops->open/close
	// directly (ELF loader etc.) never touch it.
	int handleRefCount;
	//arena_t* arena;

	// OPEN-FILE REGISTRY links (vfs.c, since 2026-08-06 — the sync(8) slice).
	// Every fs-glue open threads its file onto one global list; close unlinks
	// it. This is the bookkeeping that lets vfs_sync_all() reach OTHER tasks'
	// dirty files — the whole reason sync(1) can exist (a FAT file's true
	// length lives only in the writer's FIL until someone syncs it; ask
	// ping.log). The other customers: unmount's busy count, /sys/openfiles.
	vfs_file_t *openNext;
	vfs_file_t *openPrev;
	// THE CLOSING WINDOW. Set at the top of a glue's close, cleared by
	// leaving the list at the bottom of it. It exists because the registry's
	// two walkers want OPPOSITE answers about a file that is mid-close:
	// vfs_sync_all must skip it (its handle is about to be freed), and
	// unmount's busy count must still SEE it (everything between those two
	// points runs on the filesystem — FAT's f_close is disk I/O, ext2 reads
	// fs_specific and may reap an orphan — and unmount is what frees the
	// filesystem). Leaving the list at the top served the first walker and
	// betrayed the second: close is not a path operation, so the gate cannot
	// hold one off, and a close in flight looked like no open file at all.
	volatile bool closing;
	// Registration-time COPY of f_path, owned by this struct. f_path itself
	// is the CALLER's memory (the lifetime rule above the mount table), and
	// kernel-internal openers pass buffers that die long before the file
	// closes — the shared-object registry's resolve tail did, and rendering
	// it later printed garbage (a freed HEAP path would have walked
	// /sys/openfiles into the use-after-free tripwire instead). Anything
	// that names a registered file after open returns reads THIS — at FULL
	// length: a copy that silently drops the tail of a long path hands
	// /sys/openfiles and lsof a different file, and for a kernel-held open
	// (a running image, a resident .so) that row is the only one there is.
	char reg_path[VFS_REG_PATH_MAX];

	// THE POSITION LOCK (2026-08-15). An open file carries ONE seek position,
	// so "seek here, then read" is only atomic if nobody re-seeks in between —
	// and the demand pager does exactly that pair, from any core, for any
	// thread that touches an unresolved page. The day a task grew a second
	// thread, two of them could fault on two different code pages at once and
	// each receive the OTHER's file offset: a page of perfectly valid machine
	// code from the wrong part of the binary, executed, with the crash landing
	// somewhere unrelated and unrepeatable.
	//
	// Zero = unlocked, and every allocation in this kernel is zeroed at the
	// choke point, so existing creators need no change. Taken ONLY around
	// seek+read pairs on a file that more than one thread can reach; the
	// dynamic-linking path solved the identical problem first with
	// shared_object_t's io_lock, and this is the same lock one layer down.
	volatile uint64_t pos_lock;
};

struct file_operations
{
    int (*open)(vfs_file_t** vfs_file, const char* path, const char* mode, vfs_filesystem_t* vfs_fs);
    int (*read)(vfs_file_t* vfs_file, void* buffer, size_t size);
	char* (*fgets)(vfs_file_t* vfs_file, char* buffer, int length);
	int (*fputs)(vfs_file_t* vfs_file, char* buffer);
	int64_t (*tell)(vfs_file_t* vfs_file);
	int (*fprintf)(vfs_file_t* vfs_file, const char* fmt, ...);
    int (*write)(vfs_file_t* vfs_file, const void* buffer, size_t size);
    int (*seek)(vfs_file_t* vfs_file, long offset, int whence);
	int (*sync)(vfs_file_t* vfs_file);
    int (*close)(vfs_file_t* vfs_file);
	int (*flush) (void *f);
	// Delete a file. The path arrives fs-local — mount prefix already
	// stripped — exactly like open's.
	//
	// The vfs_fs argument is not decoration: FatFs addresses volumes by
	// number ("2:/os64.log"), so an rm with no filesystem cannot name the
	// file it is being asked to delete. This slot sat in the struct from the
	// beginning taking only a filename, which is why nothing ever implemented
	// it. A filesystem with no write path simply leaves it NULL, and
	// syscall_unlink reports read-only rather than dispatching through zero
	// (see the same lesson in fat_glue.c's disk_write).
	int (*rm) (const char *filename, vfs_filesystem_t* vfs_fs);
	// Give a file a different name. BOTH paths arrive fs-local (mount
	// prefix already stripped) and BOTH belong to THIS filesystem —
	// syscall_rename refuses a cross-mount rename before dispatching here,
	// because moving bytes between two filesystems is a copy, not a rename,
	// and it belongs in userland where a partial copy can be cleaned up.
	// (Unix draws the same line and calls it EXDEV; ours is the same line
	// drawn for the same reason.)
	//
	// With flags zero, an existing REGULAR destination is REPLACED — the
	// original contract and the behavior every existing caller keeps.
	// rename(2) exists at all
	// because 4.2BSD could not make link+unlink atomic, and every safe
	// "write a new version of this file" idiom since — write to a temp
	// name, verify it, put it in place — rests on there being no instant
	// at which the destination does not exist. os64get is that idiom's
	// first customer here.
	//
	// Two refusals survive the replacement rule, both inherited from
	// ext2_rm's ruling rather than invented here:
	//   - the destination is a DIRECTORY (empty or not). Directories are
	//     not interchangeable with files, and a silent rmdir hiding inside
	//     a rename is exactly the surprise this house does not ship.
	//   - an open DIRECTORY on either side. Its reader is walking the very
	//     blocks whose naming context a move changes. Open regular files are
	//     fine; their handles retain inode identity across rename/replacement.
	//
	// Nonzero OS64_RENAME_* flags ask the filesystem for a stronger policy:
	// NOREPLACE refuses an existing destination atomically, while
	// REQUIRE_ATOMIC_REPLACE refuses replacement on a filesystem that would
	// have to unlink first. They are mutually exclusive; unknown combinations
	// are refused. The syscall validates too, but the driver still owns the
	// guarantee because in-kernel callers use this seam directly.
	//
	// Legacy replacement atomicity is the filesystem's to grant, not this
	// seam's to promise.
	// ext2 gets it honestly (one directory-block write publishes the new
	// name; the old name's removal and the doomed inode's teardown follow).
	// FAT cannot: FatFs's f_rename refuses an existing destination outright,
	// so fat_rename must remove-then-rename and there IS a window. That is
	// stated at the FAT implementation and booked in DEBTS rather than
	// papered over — the lifeboat is allowed to be a lifeboat, but not
	// allowed to lie about it.
	//
	// A filesystem with no write path leaves this NULL and syscall_rename
	// reports read-only rather than dispatching through zero (the same
	// lesson fat_glue.c's disk_write taught the hard way).
	// Returns 0 on success, negative on refusal.
	int (*rename) (const char *oldpath, const char *newpath,
	               vfs_filesystem_t* vfs_fs, uint64_t flags);
	int (*initialize) (vfs_filesystem_t* device);
	// "You are now reachable by path." Called by kRegisterFilesystem AFTER
	// the mount table entry exists — the moment that separates a filesystem
	// being READ into memory from a filesystem being MOUNTED.
	//
	// This slot exists because that distinction turned out to be load-bearing
	// and nothing had ever needed to notice it. `initialize` runs BEFORE the
	// mount table is claimed, so a filesystem that WRITES during initialize
	// writes to a partition the kernel does not yet consider mounted — which
	// the block-layer stray-write tripwire correctly panics on, and did
	// (2026-08-16: ext2's orphan replay, the first mount-time write os64 has
	// ever had). Two rules fall out, and they are the whole point of the slot:
	//
	//   initialize: READ what you need in order to decide whether to mount.
	//   mounted:    WRITE whatever mounting obliges you to do.
	//
	// The second reason is independent of the tripwire: a mount can still
	// FAIL after initialize succeeds (a full mount table), and work done in
	// initialize would already have touched the disk of a filesystem that
	// never became reachable by any path.
	//
	// Return value is advisory — the mount has already happened and will not
	// be undone. Report trouble; do not expect to veto.
	int (*mounted) (vfs_filesystem_t* fs);
	int (*uninitialize) (vfs_filesystem_t* device);
	// How big and how full — the numbers df exists to print, in BYTES.
	// Answered from mount-time state (ext2's cached superblock) or the
	// driver's own accounting (FatFs walks the FAT); a filesystem with no
	// meaningful answer (the synthetics) leaves the slot NULL and
	// /sys/mounts prints dashes. Returns 0 on success.
	int (*space) (vfs_filesystem_t* fs, uint64_t* total_bytes, uint64_t* free_bytes);
};


extern dlist_t* kBlockDeviceDList;
extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;
extern vfs_filesystem_t* kRootFilesystem;

// ── The mount table ──────────────────────────────────────────────────────────
// One namespace, many filesystems (the idea is Unix v1's — mount(1) predates
// almost everything else): each mounted filesystem claims a PATH PREFIX, and
// every canonical absolute path is routed to the longest matching prefix. The
// root claims "/"; secondary partitions found at boot are auto-mounted at
// "/<fstype>" ("/fat", "/ext2", then "/fat2"… if twins appear). A prefix is
// purely a namespace claim — no directory of that name needs to exist on the
// root filesystem (mount points as real directories are a refinement Unix
// added later; we start where it started).
//
// kRootFilesystem remains the "/" entry's fs, kept as a convenient alias
// because half the kernel gates on "is a root mounted yet?".

// THE CEILING (raised 8 → 16 on 2026-08-20, when /dev arrived). Eight was
// never a considered number, and a normal boot had quietly climbed to six or
// seven: "/", "/home", the "/fat" lifeboat, "/proc", "/sys", "/dev", plus
// whatever the secondary-partition sweep finds ("/ext2", "/fat2"… when twins
// appear). Nothing is sized by this but the table itself — no packed struct,
// no on-disk record, nothing the ABI can see — so the whole cost of doubling
// it is ~512 bytes of BSS, and the whole benefit is that the next subsystem
// to want a mount does not have to think about it.
//
// The failure this replaces was worse than the limit: a full table used to
// fail as a printd(DEBUG_BOOT) line and an ignored return value, which on a
// normal boot means /proc silently does not exist and NOTHING says so. Both
// mount paths now put that on the glass (see kRegisterFilesystem and
// synthfs_mount) — tripwires over silence, and a mount that did not happen is
// exactly the kind of absence that reads as a bug somewhere else entirely.
#define VFS_MAX_MOUNTS 16
#define VFS_MOUNT_PREFIX_MAX 32

typedef struct {
	char prefix[VFS_MOUNT_PREFIX_MAX]; // canonical: "/" for root, else "/name"
	size_t prefix_len;                 // strlen(prefix); 1 marks the root entry
	uint8_t part_guid[16];             // backing partition GUID — dedupe key, so
	                                   // a RAMDisk and the NVMe disk it was
	                                   // imaged from never both mount (first
	                                   // registered wins, matching the root scan)
	// What is mounted here, as a word: "ext2", "fat", or a synthetic's own
	// name ("proc", "sys", "dev"). A static string or a pointer into this
	// entry's own prefix — never allocated, never freed. /sys/mounts prints
	// it; nothing dispatches on it (dispatch is what the op tables are for).
	const char *fstype;
	// fs == NULL marks a FREE slot (never mounted, or unmounted). Entries
	// never move and kMountCount is a high-water mark, so an open directory's
	// synthetic-readdir cursor (mount_scan) stays meaningful across an
	// unmount; every walker skips NULL. A claim fills the entry and stores
	// `fs` LAST; an unmount clears `fs` FIRST (vfs.c, under kMountTableLock).
	vfs_filesystem_t *fs;
} vfs_mount_entry_t;

extern vfs_mount_entry_t kMountTable[VFS_MAX_MOUNTS];
extern int kMountCount;

// Route a CANONICAL absolute path (vfs_canonicalize_path output — this is why
// canonicalization happens first) to its filesystem by longest-prefix match.
// On success, *tail (if non-NULL) points at the fs-local remainder, always
// itself an absolute path: "/fat/bin/ls" → FAT fs, tail "/bin/ls"; "/fat" →
// FAT fs, tail "/". For the root fs the tail is the path unchanged. The tail
// points INTO the caller's path buffer (or at a static "/") — never free it,
// and clone it if it must outlive the buffer (syscall_open does exactly this).
// Returns NULL only when nothing is mounted. Pure string matching — no disk
// I/O, safe from any context.
vfs_filesystem_t *vfs_resolve_mount(const char *canonical_path, const char **tail);

// The synthetic phase of mount-aware readdir: serve the next mount point
// that is a DIRECT child of `dir`'s canonical path (grandchildren belong to
// deeper listings) as an os64_dirent_t — directory flag set, size 0.
// Returns 1 = entry filled, 0 = no more (or the dir carries no path tag —
// a kernel-internal opendir). Pure kMountTable string scan — no disk I/O,
// safe from any CR3.
int vfs_readdir_child_mounts(vfs_directory_t *dir, os64_dirent_t *entry);

// ── The namespace verbs (2026-08-30) ─────────────────────────────────────────
// mount/unmount at runtime. `what` is a GPT partition name or a GUID string —
// never a device path (abi/include/os64/mount.h carries the argument, flag
// and result vocabulary; the syscalls return these values verbatim). `where`
// is a CANONICAL absolute path — or NULL/empty, meaning "at the partition's
// own GPT label" (fstype fallback, collision numbering), the same derivation
// a one-token mounts.conf line gets. `flags` is the OS64_MOUNT_* bits (RO
// mounts get write-stripped op tables from birth). Callable from kernel
// context (the fixture and the mounts.conf boot reader do) and from the
// syscalls.
//
// BOTH VERBS ARE EXCLUSIVE — one namespace mutation at a time, machine-wide
// (kNamespaceLock, vfs.c). Each is read-then-act on a table the layer below
// re-reads nothing from, so admitting two at once let two unmounts free one
// filesystem twice and two mounts publish one partition twice. This is why
// mount is NOT a path-gate reader: it is the gate's other writer.
int vfs_mount_explicit(const char *what, const char *where, uint64_t flags);
int vfs_unmount(const char *where);

// THE PATH GATE — what makes unmount sound against in-flight path operations.
// Every operation that RESOLVES a path to a filesystem and then acts on it
// (open, chdir, stat, mkdir, rm, rename, spawn's ELF+library loads, the conf
// walker's probes) enters as a READER for the duration of the operation;
// readers never wait for each other. THE TWO NAMESPACE VERBS ARE THE WRITERS:
// each closes the gate, waits for the readers already inside to leave, and
// holds it shut across its decisions.
//
// Unmount needs it so that "no open files, no open dirs, no cwd inside"
// cannot be falsified by an operation that was mid-fault between resolve and
// open when the check ran, and so the fs it tears down is one no path
// operation can still be touching. Mount needs it because its parent check
// and unlink's mount-under check (vfs_mount_under) are each other's mirror:
// unordered, one of them always reads a namespace the other is halfway
// through changing, and the result is a live mount whose point has no parent.
// Enter/exit MUST bracket the whole resolve-to-done span, never just the
// resolve.
//
// The gate excludes READERS and nothing else — its writer flag is a flag,
// not a lock, and writers exclude each other through kNamespaceLock (vfs.c).
// And it cannot cover a CLOSE, which resolves no path and can arrive at any
// moment: that hole is closed by `closing` in struct file instead.
void vfs_path_enter(void);
void vfs_path_exit(void);

// The mount prefix serving `fs`, for printing a FULL path from an fs-local
// tail (procfs handles, diagnostics). Root answers "" so prefix+tail
// concatenates cleanly. Returns false if no live entry names this fs.
bool vfs_prefix_for_fs(vfs_filesystem_t *fs, char *out, size_t cap);

// Is a live mount rooted UNDER `path`? Asked by unlink and rename before
// they remove or move a directory: the mount verb insisted the point's
// parent exist, and nothing else in the tree knows to keep it existing —
// a mount point's parent is an ordinary directory to every other verb.
// Answers false for "/" (not removable, and everything is under it).
bool vfs_mount_under(const char *path);

// How many open files sit on `fs` (the open-file registry's count) —
// unmount's busy arithmetic and /sys/mounts' open column.
int vfs_openfiles_on(vfs_filesystem_t *fs);

// One row of /sys/openfiles — a COPY of a registered open file's public
// face, safe to render after the registry lock is gone. `handles` is the
// handle-layer refcount: 0 means no task handle references it, so the
// KERNEL is the holder — a running program's image held for demand paging,
// a resident shared object, an orphan kept alive by its last opener. Those
// are exactly the opens no /proc/<pid>/handles walk can see, which is why
// lsof needs this list and not only the handle tables.
typedef struct {
	char mount[VFS_MOUNT_PREFIX_MAX];  // "" for the root mount — prefix+tail concatenates
	char tail[VFS_REG_PATH_MAX];       // fs-local path (the registration-time reg_path copy)
	uint64_t ident;                    // f_ident: ext2 inode / FAT start cluster
	int handles;
} vfs_openfile_row_t;

// Copy out the registry, newest first (registration order). Returns the
// TOTAL registered count; at most max_rows rows are written, so a caller
// seeing total > max_rows knows the listing was cut, and by how much.
int vfs_openfiles_snapshot(vfs_openfile_row_t *rows, int max_rows);

// Render 16 raw GPT GUID bytes in the dashed spelling ROOT= speaks —
// "2f4fd02e-68b4-4c82-98bc-72467529b3fc". `out` needs 37 bytes.
void vfs_format_guid(const uint8_t *guid, char *out);

// Claim a mount table entry — the one place a claim happens, shared by
// kRegisterFilesystem and synthfs_mount. Reuses free slots, publishes `fs`
// last (the lock-free-reader ordering rule at vfs_mount_entry_t), announces
// a full table on the glass itself. `fstype` must be a string that outlives
// the mount (a literal, or a pointer into static storage).
//
// FALSE MEANS THE TABLE WAS FULL, and only that. kRegisterFilesystem reads
// this bool and answers its caller OS64_MOUNT_TABLE_FULL, so a second reason
// to refuse cannot just be added here — it needs a reason out-param, or the
// mount verb starts naming the wrong ceiling.
bool vfs_mount_claim(const char *prefix, const uint8_t *guid_or_null,
                     const char *fstype, vfs_filesystem_t *fs);

// ── The open-file registry (the sync(8) slice, 2026-08-06) ──────────────────
// Each filesystem glue calls register at the bottom of a successful open,
// mark_closing at the TOP of close, and unregister at the BOTTOM of it —
// after the last line that touches the filesystem, before the frees (see
// `closing` in struct file for why the two ends are not the same moment).
// Task/kernel-thread context ONLY (the registry lock is a plain,
// interrupts-on spinlock — see spinlock.h for the discipline).
void vfs_openfile_register(vfs_file_t *file);
void vfs_openfile_mark_closing(vfs_file_t *file);
void vfs_openfile_unregister(vfs_file_t *file);

// sync(1)'s engine: walk every registered open file and run its fops->sync.
// Filesystems whose writes are already durable-and-visible (ext2's
// write-through) have cheap or no-op syncs; FAT's f_sync is the one doing
// real work (data flush + the directory-entry size update that makes a
// still-open file's true length visible to fresh opens). Returns the number
// of files synced, or negative if any individual sync reported failure —
// after still attempting all the rest (a broom does not stop at the first
// dusty corner).
int64_t vfs_sync_all(void);

// Does (device, partNo) back a mount whose filesystem can write? The
// stray-write tripwire's question (block_verify_write_allowed): a disk write
// is legitimate only if the MOUNTED filesystem over that partition installed
// a write path — and the per-mount fops copy is the authority, because that
// is where a read-only ext2 mount's write slot is NULL even though a
// write-capable table exists elsewhere. Not mounted ⇒ false: nothing
// legitimate writes to a partition no filesystem has claimed. Pure
// kMountTable scan — no disk I/O, safe from any context/CR3.
bool vfs_partition_mount_writable(block_device_info_t *dev, int partNo);

// Demote one mount in place. Public so focused tests can exercise the exact
// transition on a private mount-table copy without disabling the running OS.
void vfs_demote_mount_readonly(vfs_filesystem_t *fs);

// ── THE DRILL FLAG (2026-08-20) ─────────────────────────────────────────────
// Set while a test is DELIBERATELY inducing a failure that would otherwise
// shout at the operator. The fault-injection tests drive real demotion paths
// on a CLONED mount struct (see test_main.c) precisely so the damage lands on
// a throwaway copy — but the alarms those paths raise are written for a human
// watching a real filesystem get taken away, and printing five of them on
// every healthy boot is how an alarm stops meaning anything.
//
// So: while this is true, an alarm goes to the LOG; otherwise it goes to the
// GLASS. It never suppresses the message, only chooses the audience — a REAL
// demotion still lands on screen, which is the entire reason it exists.
//
// Same shape and same reason as kTestingPageFaults (exceptions.h), which
// keeps the deliberate-#PF tests from tripping the fatal-fault reporter.
// Chris's ruling, 2026-08-20: "The test messages need to go to the log. Real
// ext2 messages can stay on the screen."
extern bool kTestingExpectedNoise;

// TEST_RO's engine (2026-08-08): NULL the write verbs on every mount's
// private op-table copies — every dispatch site then refuses exactly like a
// born-read-only mount, the stray-write tripwire starts refusing at the
// block layer, and the system stays up for analysis. errors=remount-ro,
// os64 edition. One-way until reboot (the write tables' addresses aren't
// kept anywhere to restore from — deliberately: a system that failed a
// write test does not get its pens back by asking).
void vfs_demote_all_mounts_readonly(const char *why);

// The tripwire's second question, asked only while composing the panic
// message: is (dev, partNo) mounted at all? Distinguishes "mounted
// read-only" (a write aimed at a filesystem that refused the pen) from "not
// mounted" (a write aimed at nothing — the classic misroute). Same pure
// scan, same any-context safety.
bool vfs_partition_mounted(block_device_info_t *dev, int partNo);

// Resolve `path` against `cwd` into a CANONICAL absolute path in `out`:
// relative paths are prefixed with cwd, "." disappears, ".." pops a component
// (".." at the root stays at the root, per tradition), duplicate slashes
// collapse. This is THE one place path hygiene happens — filesystems and
// syscalls downstream only ever see clean absolute paths. Returns 0, or -1
// if the result won't fit in outlen.
int vfs_canonicalize_path(const char *cwd, const char *path, char *out, size_t outlen);

void init_block();
// Build a mount and publish it. Returns NULL on refusal, having given back
// every allocation it made — a runtime mount can be asked for over and over
// by any task, so a failure that leaked would be an exhaustion attack. When
// out_reason is non-NULL it receives the OS64_MOUNT_* code saying WHICH
// refusal it was (os64/mount.h); boot-time callers, which have a log line
// instead of a caller to answer, pass NULL.
vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps, int *out_reason);
int ext2_initialize_filesystem(vfs_filesystem_t* device);
int vfs_mount_root_part(char* rootPartUUID);

#endif
