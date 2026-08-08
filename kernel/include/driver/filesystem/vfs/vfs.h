#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dlist.h"
#include "types.h"
#include "os64/dirent.h"   // os64_dirent_t — the fs-neutral dops->read contract


#define DENTRY_ROOT 0xFFFFFFFF    
#define VFS_MAX_OPEN_FILES 512
#define VFS_MAX_OPEN_DIRS 64
#define VFS_FILE_ALLOC_SIZE 65535+FS_FILE_COPYBUFFER_SIZE
#define VFS_MAX_PARTITIONS 128
#define DEFAULT_SECTOR_SIZE 512

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
	FILETYPE_PROCFILE = 3
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
	char* name;
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
	// syscall_readdir): when this handle opened the ROOT directory of its
	// mount (fs-local path "/"), the mount points living directly under it
	// ("/fat" when listing "/") are served as SYNTHETIC entries after the
	// filesystem's own entries run out. A mount point is namespace routing,
	// not directory content — the fs on disk has no such entry to return,
	// so the VFS must speak for it or `ls /` can't see what `cd /fat`
	// can reach. mount_prefix points into kMountTable (static storage,
	// mounts never unmount) and stays NULL for any non-mount-root dir;
	// mount_scan is the resume cursor for the synthetic phase (the
	// allocator's zeroing initializes both).
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
	void* fs_specific;
};

struct file
{
	eFileType filetype;
	char* f_path;
	inode_t* f_inode;
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
	// ping.log). Future customers: lsof, umount refusal counts.
	vfs_file_t *openNext;
	vfs_file_t *openPrev;
};

struct file_operations
{
    int (*open)(vfs_file_t** vfs_file, const char* path, const char* mode, vfs_filesystem_t* vfs_fs);
    int (*read)(vfs_file_t* vfs_file, void* buffer, size_t size);
	char* (*fgets)(vfs_file_t* vfs_file, char* buffer, int length);
	int (*fputs)(vfs_file_t* vfs_file, char* buffer);
	int (*tell)(vfs_file_t* vfs_file);
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
	int (*initialize) (vfs_filesystem_t* device);
	int (*uninitialize) (vfs_filesystem_t* device);
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

#define VFS_MAX_MOUNTS 8
#define VFS_MOUNT_PREFIX_MAX 32

typedef struct {
	char prefix[VFS_MOUNT_PREFIX_MAX]; // canonical: "/" for root, else "/name"
	size_t prefix_len;                 // strlen(prefix); 1 marks the root entry
	uint8_t part_guid[16];             // backing partition GUID — dedupe key, so
	                                   // a RAMDisk and the NVMe disk it was
	                                   // imaged from never both mount (first
	                                   // registered wins, matching the root scan)
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
// that is a DIRECT child of `dir`'s mount prefix (grandchildren belong to
// deeper listings) as an os64_dirent_t — directory flag set, size 0.
// Returns 1 = entry filled, 0 = no more (or dir isn't a mount root).
// Pure kMountTable string scan — no disk I/O, safe from any CR3.
int vfs_readdir_child_mounts(vfs_directory_t *dir, os64_dirent_t *entry);

// ── The open-file registry (the sync(8) slice, 2026-08-06) ──────────────────
// Called by each filesystem glue at the bottom of a successful open and the
// top of close. Task/kernel-thread context ONLY (the registry lock is a
// plain, interrupts-on spinlock — see spinlock.h for the discipline).
void vfs_openfile_register(vfs_file_t *file);
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
vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps);
int ext2_initialize_filesystem(vfs_filesystem_t* device);
int vfs_mount_root_part(char* rootPartUUID);

#endif
