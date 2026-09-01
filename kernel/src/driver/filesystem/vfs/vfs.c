#include "vfs.h"
#include "part_table.h"
#include "driver/filesystem/vfs/vfs.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memcmp.h"
#include "memset.h"
#include "strings/strings.h"
#include "panic.h"
#include "ext2_fs.h"
#include "ext2_vfs.h"
#include "block_device.h"
#include "sprintf.h"
#include "fat_glue.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — mount-sweep lines belong on the glass too
#include "spinlock.h"        // the PLAIN variant — the open-file registry's lock
#include "task.h"            // task_t — unmount's cwd busy check walks the task spine
#include "scheduler.h"       // kTaskList + NO_TASK, the spine that walk follows
#include "os64/mount.h"      // the mount/unmount result vocabulary (shared with ring 3)

// The registry's owned path copy has to hold whatever a path syscall can
// carry. The two constants live in headers that cannot see each other; this
// is the one place that sees both.
_Static_assert(VFS_REG_PATH_MAX >= TASK_MAX_PATH_LEN,
               "VFS_REG_PATH_MAX must not be shorter than TASK_MAX_PATH_LEN");

// FatFs's volume numbers, handed out and GIVEN BACK. A FAT mount is "1:",
// "2:" … to FatFs, and that number is the only thread disk_read has back to
// its block device (fat_glue's drive_label / pdrv), so two live mounts must
// never share one and FF_VOLUMES is a ceiling FatFs cannot be argued out of.
//
// A bitmap and not a counter. The counter could only take back the number it
// handed out LAST, so unmounting the OLDER of two FAT mounts leaked its
// number for the rest of the boot, and a remount cycle on that partition
// walked the ceiling one number at a time. Nothing checked the ceiling
// either: past it, the counter handed FatFs a pdrv nothing resolves.
//
// Volume 0 is never allocated. FAT_DISK_NONE is 0xFF and a zeroed fs struct
// must not read as a valid FAT mount — see the FAT_DISK_NONE note in vfs.h
// for the root-corruption hunt that rule came from.
static volatile uint16_t kFatVolumesInUse = 0;   // bit N set = volume N is live

static uint8_t fat_volume_alloc(void)
{
	for (;;)
	{
		uint16_t inUse = kFatVolumesInUse;
		uint8_t n = 1;
		while (n < FF_VOLUMES && (inUse & (1u << n)) != 0)
			n++;
		if (n >= FF_VOLUMES)
			return FAT_DISK_NONE;   // every volume spoken for
		// CAS rather than a lock: this is reached from the boot sweep and from
		// the mount verb, and a self-contained atomic owes nothing to whichever
		// lock the caller happens to hold.
		if (__sync_bool_compare_and_swap(&kFatVolumesInUse, inUse,
		                                 (uint16_t)(inUse | (1u << n))))
			return n;
	}
}

static void fat_volume_free(uint8_t n)
{
	if (n == FAT_DISK_NONE || n >= FF_VOLUMES)
		return;
	__sync_fetch_and_and(&kFatVolumesInUse, (uint16_t)~(1u << n));
}

// The mount table (see vfs.h for the design note). Filled by
// kRegisterFilesystem and synthfs_mount, consulted by vfs_resolve_mount,
// mutated at runtime by the mount/unmount syscalls (2026-08-30 — the day the
// old "no lock, revisit when mount/umount syscalls exist" note here came
// due). Entries never move: fs == NULL is a free slot, a claim stores `fs`
// LAST and an unmount clears it FIRST, so a lock-free reader keyed on fs
// never sees a half-built entry. kMountCount is the high-water mark.
vfs_mount_entry_t kMountTable[VFS_MAX_MOUNTS];
int kMountCount = 0;

// Serializes table MUTATION (claim, strike) and any walk that must see a
// consistent whole (the sysfs render, unmount's own checks). The hot reader
// — vfs_resolve_mount, on every open — deliberately stays lockless per the
// ordering rule above. PLAIN spinlock: never taken from interrupt or fault
// context.
static spinlock_t kMountTableLock = 0;

// Serializes the NAMESPACE VERBS against each other — one mount or one
// unmount at a time, machine-wide. kMountTableLock protects the table's
// BYTES; this protects the DECISIONS taken from them. Both verbs are
// read-then-act — mount asks "is this GUID mounted, is this prefix free"
// and unmount asks "is anything using this" — and neither the claim nor the
// strike underneath them re-asks. Two cores unmounting one prefix both
// passed the busy check and both ran the teardown: a ring-3-triggerable
// double free. Two cores mounting one partition both published it.
//
// PLAIN spinlock, and it is HELD ACROSS REAL DISK I/O (a driver's
// initialize and uninitialize). That is the trade kOpenFileLock already
// makes, for the same reason: interrupts must stay on or a disk wait times
// out against its own frozen tick clock (spinlock.h). What spinlock.h calls
// unaffordable — a long critical section — is affordable here because these
// verbs are rare and human-paced; the waiter is another core spinning
// through one mount, not a hot path.
static spinlock_t kNamespaceLock = 0;

// ── The path gate (contract in vfs.h) ────────────────────────────────────────
// Readers are path operations; mount and unmount are writers. Readers pay two
// atomics; a writer spins until the room is empty and blocks the door while it
// works. A reader arriving while the door is blocked waits — path ops are all
// bounded (no path op parks on user input), so the wait is bounded too.
//
// kVfsPathWriter is a FLAG, not a lock: it excludes readers, and nothing
// more. Two writers both find it false and both set it true, which is why
// callers that close this gate hold kNamespaceLock first.
static volatile int  kVfsPathReaders = 0;
static volatile bool kVfsPathWriter  = false;

void vfs_path_enter(void)
{
	for (;;)
	{
		while (kVfsPathWriter)
			__builtin_ia32_pause();
		__sync_fetch_and_add(&kVfsPathReaders, 1);
		if (!kVfsPathWriter)
			return;
		// The writer arrived between our check and our increment — back out
		// and queue behind it, or its readers==0 wait never ends.
		__sync_fetch_and_sub(&kVfsPathReaders, 1);
	}
}

void vfs_path_exit(void)
{
	__sync_fetch_and_sub(&kVfsPathReaders, 1);
}

static void vfs_path_gate_close(void)
{
	kVfsPathWriter = true;
	__sync_synchronize();
	while (kVfsPathReaders != 0)
		__builtin_ia32_pause();
}

static void vfs_path_gate_open(void)
{
	__sync_synchronize();
	kVfsPathWriter = false;
}

// Claim a mount table entry — the ONE place a claim happens, for both doors
// (kRegisterFilesystem and synthfs_mount). Fills everything, publishes `fs`
// last. Free slots are reused so unmount/mount cycles don't consume the
// table. Returns false with the glass complaint when the table is full —
// the ceiling's contract (vfs.h) is honored here so neither door forgets it.
bool vfs_mount_claim(const char *prefix, const uint8_t *guid_or_null,
                     const char *fstype, vfs_filesystem_t *fs)
{
	spinlock_acquire(&kMountTableLock);
	int slot = -1;
	for (int i = 0; i < kMountCount; i++)
		if (kMountTable[i].fs == NULL)
		{
			slot = i;
			break;
		}
	if (slot < 0 && kMountCount < VFS_MAX_MOUNTS)
		slot = kMountCount;
	if (slot < 0)
	{
		spinlock_release(&kMountTableLock);
		printd(DEBUG_BOOT, "BOOT: mount table full (%u), %s not mounted\n",
		       VFS_MAX_MOUNTS, prefix);
		printf("MOUNT TABLE FULL (%u) — %s NOT mounted\n", VFS_MAX_MOUNTS, prefix);
		return false;
	}

	vfs_mount_entry_t *m = &kMountTable[slot];
	strncpy(m->prefix, prefix, VFS_MOUNT_PREFIX_MAX - 1);
	m->prefix[VFS_MOUNT_PREFIX_MAX - 1] = '\0';
	m->prefix_len = strlen(m->prefix);
	if (guid_or_null != NULL)
		memcpy(m->part_guid, guid_or_null, 16);
	else
		memset(m->part_guid, 0, sizeof(m->part_guid));
	m->fstype = fstype;
	__sync_synchronize();   // the entry is whole before anyone can find it
	m->fs = fs;
	if (slot == kMountCount)
		kMountCount++;
	spinlock_release(&kMountTableLock);
	return true;
}

vfs_filesystem_t *vfs_resolve_mount(const char *canonical_path, const char **tail)
{
	vfs_mount_entry_t *best = NULL;

	for (int i = 0; i < kMountCount; i++)
	{
		vfs_mount_entry_t *m = &kMountTable[i];
		if (m->fs == NULL)
			continue;   // a free (or freshly unmounted) slot
		if (m->prefix_len == 1)
		{
			// The root ("/") matches every absolute path, as the fallback.
			if (best == NULL)
				best = m;
			continue;
		}
		// "/fat" must match "/fat" and "/fat/…" but never "/fatso" — the
		// character after the prefix has to be a boundary.
		if (strncmp(canonical_path, m->prefix, m->prefix_len) == 0 &&
		    (canonical_path[m->prefix_len] == '\0' || canonical_path[m->prefix_len] == '/'))
		{
			if (best == NULL || m->prefix_len > best->prefix_len)
				best = m;
		}
	}

	if (best == NULL)
		return NULL;   // nothing mounted at all

	if (tail != NULL)
	{
		if (best->prefix_len == 1)
			*tail = canonical_path;                    // root: path unchanged
		else if (canonical_path[best->prefix_len] == '\0')
			*tail = "/";                               // "/fat" → fs root
		else
			*tail = canonical_path + best->prefix_len; // "/fat/x" → "/x"
	}
	return best->fs;
}

// The synthetic phase of mount-aware readdir (see the struct directory note
// in vfs.h). Called by syscall_readdir once the filesystem's own entries are
// exhausted; walks kMountTable from the dir's saved cursor and serves each
// mount point that sits DIRECTLY under this dir's mount prefix. A duplicate
// is possible in principle (a real "fat" directory in the root fs would list
// twice) — path RESOLUTION already gives the mount the last word there, and
// the curated tree keeps such shadowing from arising in practice.
int vfs_readdir_child_mounts(vfs_directory_t *dir, os64_dirent_t *entry)
{
	if (dir->mount_prefix == NULL)
		return 0;   // not a mount root — nothing synthetic to say

	while (dir->mount_scan < kMountCount)
	{
		vfs_mount_entry_t *m = &kMountTable[dir->mount_scan++];
		if (m->fs == NULL)
			continue;   // a free (or freshly unmounted) slot
		const char *rest = NULL;

		// A child of "/" is "/name"; a child of "/mnt" would be "/mnt/name".
		// (Nothing mounts below the first level today, but the parent match
		// is written for the tree, not for today's flat table.)
		if (dir->mount_prefix_len == 1)
		{
			if (m->prefix_len > 1)
				rest = m->prefix + 1;
		}
		else if (m->prefix_len > dir->mount_prefix_len &&
		         strncmp(m->prefix, dir->mount_prefix, dir->mount_prefix_len) == 0 &&
		         m->prefix[dir->mount_prefix_len] == '/')
		{
			rest = m->prefix + dir->mount_prefix_len + 1;
		}

		if (rest == NULL || rest[0] == '\0')
			continue;   // not under this dir (or IS this dir — the root entry)

		bool direct_child = true;   // "/mnt/usb" under "/" is /mnt's to list
		for (const char *c = rest; *c != '\0'; c++)
			if (*c == '/')
			{
				direct_child = false;
				break;
			}
		if (!direct_child)
			continue;

		memset(entry, 0, sizeof(*entry));
		// A mount point always lists as a directory, and says that it is one
		// — this is the synthetic entry a recursive walker would otherwise
		// climb into another filesystem through (dirent.h carries the whole
		// argument).
		entry->flags = OS64_DE_DIR | OS64_DE_MOUNT;
		size_t n = 0;
		while (rest[n] != '\0' && n < OS64_DIRENT_NAME_MAX)
		{
			entry->name[n] = rest[n];
			n++;
		}
		entry->name[n] = '\0';
		return 1;
	}
	return 0;   // table exhausted — readdir's sticky EOF from here on
}

// The mirror of kRegisterFilesystem's construction, in reverse — ONE
// teardown with two callers, a failed mount's unwind and vfs_unmount, so the
// two can never drift apart. `initialized` says whether the driver's
// initialize ran AND succeeded: only then is there driver state to reclaim,
// and fat_uninitialize would kfree(NULL) — a panic in this kernel — on a
// mount whose initialize never got as far as fs_specific.
//
// Every kfree below is of the per-mount COPY the register made. A driver that
// swapped one for a global table would hand this a kernel .data address,
// which is exactly how fat_initialize's fops overwrite was caught.
static void vfs_teardown_filesystem(vfs_filesystem_t *fs, bool initialized)
{
	if (fs == NULL)
		return;

	if (initialized && fs->fops != NULL && fs->fops->uninitialize != NULL)
		fs->fops->uninitialize(fs);
	fat_volume_free(fs->fatDiskNumber);

	for (dlist_node_t *n = kBlockDeviceDList->head; n != NULL; n = n->next)
		if (n->data == fs)
		{
			dlist_remove(kBlockDeviceDList, n);
			break;
		}

	if (fs->mount != NULL)
	{
		if (fs->mount->mnt_root != NULL)
		{
			if (fs->mount->mnt_root->d_name != NULL)
				kfree(fs->mount->mnt_root->d_name);
			kfree(fs->mount->mnt_root);
		}
		kfree(fs->mount);
	}
	if (fs->files != NULL) kfree(fs->files);
	if (fs->dirs  != NULL) kfree(fs->dirs);
	if (fs->bops  != NULL) kfree(fs->bops);
	if (fs->dops  != NULL) kfree(fs->dops);
	if (fs->fops  != NULL) kfree(fs->fops);
	kfree(fs);
}

vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps, int *out_reason)
{
    vfs_filesystem_t *fs;

	if (out_reason != NULL)
		*out_reason = OS64_MOUNT_OK;

    fs = kmalloc(sizeof(vfs_filesystem_t));
    memset(fs, 0, sizeof(vfs_filesystem_t));

	// Non-FAT until proven FAT: the memset above left fatDiskNumber at 0, and
	// 0 is a value FatFs can actually hand to disk_read/disk_write as a pdrv.
	// Every non-FAT fs in the dlist matching pdrv 0 meant a leaked drive
	// number resolved to the EXT2 ROOT and wrote FAT sectors over it (see the
	// FAT_DISK_NONE comment in vfs.h). The FAT32 branch below overwrites this
	// with a real 1-based number.
	fs->fatDiskNumber = FAT_DISK_NONE;

	fs->partNumber = partNo;
    fs->mount = kmalloc(sizeof(vfs_mount_t));

    fs->mount->mnt_root = kmalloc(sizeof(dentry_t));
    fs->mount->mnt_root->d_name = kmalloc(strlen(mountPoint) + 1);   // +1: the NUL
    strcpy(fs->mount->mnt_root->d_name,mountPoint);

    //See if the filesystem being mounted is the root of the filesystem
    if (strncmp(mountPoint,"/",1024)==0)
        fs->mount->mnt_root->d_parent = (dentry_t*)DENTRY_ROOT;
    else if (mountPoint[0] != '/')
        panic("Mounting filesystem at non-absolute mount point '%s'", mountPoint);
    // Any other absolute prefix is a legal mount point now — the mount table
    // routes it. (The old code allowed only "/", "/pipe/" and "/proc" and
    // panicked on everything else; that guard died with the single-root era.)

    fs->fops = kmalloc(sizeof(vfs_file_operations_t));
    memcpy(fs->fops, fileOps,sizeof(vfs_file_operations_t));
	fs->read_only = fs->fops->write == NULL;
	fs->dops = kmalloc(sizeof(vfs_directory_operations_t));
	memcpy(fs->dops, dirOps, sizeof(vfs_directory_operations_t));
    fs->bops = kmalloc(sizeof(block_operations_t));
	memcpy(fs->bops, device->block_device->ops, sizeof(block_operations_t));
    fs->files = kmalloc(sizeof(vfs_file_t*)*VFS_MAX_OPEN_FILES);
    fs->dirs = kmalloc(sizeof(vfs_directory_t*)*VFS_MAX_OPEN_DIRS);
    fs->vfsWriteBuffer = NULL;
    fs->vfsReadBuffer = NULL;
	fs->block_device_info = device;
	// The dlist entry must exist BEFORE initialize: FatFs's disk_read glue
	// resolves its drive number back to the device by walking this list.
	add_block_device(fs);
	if (device->block_device->partition_table->parts[partNo]->filesystemType==FILESYSTEM_TYPE_FAT32)
	{
		fs->fatDiskNumber = fat_volume_alloc();
		if (fs->fatDiskNumber == FAT_DISK_NONE)
		{
			printd(DEBUG_BOOT, "BOOT: no FatFs volume number free (%u in use) — %s not mounted\n",
			       FF_VOLUMES - 1, mountPoint);
			printf("NO FAT VOLUME FREE (%u) — %s NOT mounted\n", FF_VOLUMES - 1, mountPoint);
			if (out_reason != NULL)
				*out_reason = OS64_MOUNT_NO_FAT_VOLUME;
			vfs_teardown_filesystem(fs, false);
			return NULL;
		}
	}
	if (fs->fops->initialize != NULL)
	{
		// A failed initialize (unreadable superblock, feature we can't honor)
		// means NO mount: the fs never enters the mount table, so no path can
		// route to it — and every allocation above is given back, because ring
		// 3 can now ask for the same doomed mount as many times as it likes.
		int initResult = fs->fops->initialize(fs);
		if (initResult != 0)
		{
			printd(DEBUG_BOOT, "BOOT: filesystem at %s failed to initialize (%d) — not mounted\n",
			       mountPoint, initResult);
			if (out_reason != NULL)
				*out_reason = OS64_MOUNT_FAILED;
			vfs_teardown_filesystem(fs, false);
			return NULL;
		}
	}
	// initialize may strip write slots after inspecting on-disk features.
	fs->read_only = fs->read_only || fs->fops->write == NULL;

	// Claim the prefix in the mount table — this is the moment the filesystem
	// becomes reachable by path. The claim itself lives in vfs_mount_claim
	// (shared with synthfs_mount); the fstype word is derived from what the
	// partition scan found, because /sys/mounts answers "what is this?" in
	// the same vocabulary the sweep speaks.
	partEntry_t *part = device->block_device->partition_table->parts[partNo];
	const char *fstype = "?";
	switch (part->filesystemType)
	{
		case FILESYSTEM_TYPE_FAT:
		case FILESYSTEM_TYPE_FAT32: fstype = "fat";  break;
		case FILESYSTEM_TYPE_EXT2:  fstype = "ext2"; break;
		default: break;
	}
	if (!vfs_mount_claim(mountPoint, part->uniquePartGUID, fstype, fs))
	{
		// Claim's false means a full table (its contract in vfs.h keeps it
		// that way), so the caller hears which ceiling it hit rather than
		// "the driver refused it" — the two ask for different next moves
		// (os64/mount.h). The driver DID initialize, so this unwind is the
		// one that must undo that too.
		if (out_reason != NULL)
			*out_reason = OS64_MOUNT_TABLE_FULL;
		vfs_teardown_filesystem(fs, true);
		return NULL;
	}

	// The filesystem is reachable by path as of the line above — and ONLY as
	// of that line does the block layer agree this partition is mounted. Any
	// driver work that has to WRITE belongs here rather than in initialize;
	// see fops->mounted in vfs.h for the panic that taught us the difference.
	if (fs->fops->mounted != NULL)
		fs->fops->mounted(fs);

    return fs;
}

// (ext2_initialize_filesystem used to live here as a one-line wrapper around
// the exploration sketch's superblock dump; the real implementation is in
// driver/filesystem/ext2/ext2.c now, alongside the rest of the driver.)

// The classic component-stack canonicalizer (see vfs.h for the contract).
// `out` doubles as the stack: components are appended as "/name", and ".."
// pops by scanning back to the previous '/'. No allocation, no recursion —
// safe from any context that can hold two path buffers.
int vfs_canonicalize_path(const char *cwd, const char *path, char *out, size_t outlen)
{
	if (out == NULL || outlen < 2 || path == NULL)
		return -1;

	size_t len = 0;   // bytes of `out` in use; out[0..len) holds "/a/b" form

	// Seed: absolute input starts fresh at the root; relative input starts
	// from cwd — by canonicalizing cwd's own components first, so a cwd that
	// somehow carries a "." or ".." can't smuggle it past this choke point.
	const char *sources[2] = { NULL, NULL };
	int nsources = 0;
	if (path[0] != '/')
		sources[nsources++] = (cwd != NULL && cwd[0] == '/') ? cwd : "/";
	sources[nsources++] = path;

	for (int s = 0; s < nsources; s++)
	{
		const char *p = sources[s];
		while (*p != '\0')
		{
			while (*p == '/')
				p++;                       // collapse duplicate slashes
			if (*p == '\0')
				break;

			const char *start = p;
			while (*p != '\0' && *p != '/')
				p++;
			size_t clen = (size_t)(p - start);

			if (clen == 1 && start[0] == '.')
				continue;                  // "." — no movement
			if (clen == 2 && start[0] == '.' && start[1] == '.')
			{
				// ".." — pop the last component; at the root, stay put.
				while (len > 0 && out[len - 1] != '/')
					len--;
				if (len > 0)
					len--;                 // eat the '/' too
				continue;
			}

			if (len + 1 + clen + 1 > outlen)
				return -1;                 // canonical form doesn't fit
			out[len++] = '/';
			for (size_t i = 0; i < clen; i++)
				out[len++] = start[i];
		}
	}

	if (len == 0)
		out[len++] = '/';                  // everything popped: the root
	out[len] = '\0';
	return 0;
}

// GPT stores the first three GUID fields little-endian and the rest as bytes;
// rendering them with the right widths in order IS the byte-swap dance the
// dashed spelling wants (mixed-endian by RFC 4122's blessing).
void vfs_format_guid(const uint8_t *guid, char *out)
{
	uint32_t part1 = *(const uint32_t *)guid;
	uint16_t part2 = *(const uint16_t *)(guid + 4);
	uint16_t part3 = *(const uint16_t *)(guid + 6);
	sprintf(out, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	        part1, part2, part3,
	        guid[8], guid[9],
	        guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
}

// Does `rootPartUUID` name this partition? The dashed spelling is a FIXED 36
// characters, and the answer is exact equality — nothing else. It used to be
// "does the GUID appear at the start", which made `<valid-guid>junk` name a
// real partition: malformed administrative input acting on a disk instead of
// being refused. Returns the caller's own pointer on a match (its callers
// test for NULL) — a match has nothing else to hand back.
char* compare_part_uuids(const char* rootPartUUID, const char* currPartUUID)
{
	char* result = 0;

	char* currPartStr=kmalloc(40);
	vfs_format_guid((const uint8_t *)currPartUUID, currPartStr);

	if (strlen(rootPartUUID) == 36 && strncmp(rootPartUUID, currPartStr, 36) == 0)
		result = (char*)rootPartUUID;

	printd(DEBUG_BOOT | DEBUG_DETAILED, "\tBOOT: Compared rootPartUUID=%s with partition %s, result=%p\n", rootPartUUID, currPartStr, result);

    kfree(currPartStr);

	return result;
}

// Is this partition's GUID already backing a mount? (Dedupe: RAMDisk boots
// register the RAMDisk image AND leave the NVMe original visible, with
// identical GUIDs — see the call site comment.)
static bool vfs_guid_already_mounted(const uint8_t *guid)
{
	for (int i = 0; i < kMountCount; i++)
		if (kMountTable[i].fs != NULL &&
		    memcmp(kMountTable[i].part_guid, guid, 16) == 0)
			return true;
	return false;
}

// THE DEFAULT BOOT-MOUNT POLICY: with no /etc/mounts.conf, only partitions
// os64 itself authored join the namespace uninvited. Learned on the Bosgame
// P5, 2026-07-19, the hard way: the first bare-metal boot of the mount sweep
// auto-mounted the machine's WINDOWS EFI SYSTEM PARTITION — writable — plus
// its recovery partition and a stray ext2. One husk redirection away from an
// unbootable Windows. These GUIDs began life as a hard GATE (nothing else
// could mount at all, even by syscall); mounts.conf demoted them to what
// they are now — the conservative default a machine gets until its owner
// writes down a different choice. These are the constants from the root
// GNUmakefile (DISK_PARTUUID / EXT2_PARTUUID), unchanged since the disk
// image was born.
static const char *kKnownPartGUIDs[] = {
	"2f4fd02e-68b4-4c82-98bc-72467529b3fc",   // the os64 FAT partition
	"1ec5f5ab-71b7-45cd-a7a4-05646e878e57",   // the os64 ext2 partition
	"7a3c1d90-4e62-4f3b-9a55-0c6f2b8e41d7",   // the os64 data disk — /home
};

static bool vfs_guid_is_ours(const uint8_t *guid)
{
	for (size_t i = 0; i < sizeof(kKnownPartGUIDs) / sizeof(kKnownPartGUIDs[0]); i++)
		if (compare_part_uuids(kKnownPartGUIDs[i], (const char *)guid) != NULL)
			return true;
	return false;
}

// Is a GPT partition name usable as a mount point?
//
// GPT has carried a 36-character partition name since 2016 (gpt.c already
// converts it out of UTF-16 into partEntry_t.partName) and os64 never once
// looked at it. It is the natural place for a mount point to live: the
// partition is labelled at the moment it is created, the label travels WITH
// the disk to any machine, and there is no /etc/fstab to write, parse, keep
// in sync, or get wrong. Unix needs fstab because a mount point is a local
// policy decision; here it is a property of the partition. (fstab's one row
// conflated two questions. WHERE a partition mounts is answered above —
// by the disk itself. WHAT this machine chooses to mount at boot genuinely
// IS local policy, and /etc/mounts.conf answers only that — a list of
// names, no second column needed.)
//
// The rules are deliberately strict — letters, digits, '_' and '-' only. A
// mount prefix becomes the first component of every path on that filesystem,
// so a name containing '/', '.', a space or a control character would either
// break path resolution or forge a path that resolves somewhere else. Anything
// that fails falls back to the filesystem type, which is what os64 always did.
static bool vfs_partname_usable(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return false;

	size_t len = 0;
	for (const char *p = name; *p != '\0'; p++, len++)
	{
		// Leave room for the leading '/', a 2-digit dedupe suffix and the NUL.
		if (len >= VFS_MOUNT_PREFIX_MAX - 4)
			return false;
		bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		          (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
		if (!ok)
			return false;
	}
	return true;
}

// Is this mount prefix already claimed? The mount table IS the bookkeeping —
// no per-filesystem counters to keep in step with it (the old code kept one
// per fstype and had to hand a name BACK on mount failure).
static bool vfs_prefix_in_use(const char *prefix)
{
	for (int i = 0; i < kMountCount; i++)
		if (kMountTable[i].fs != NULL &&
		    strcmp(kMountTable[i].prefix, prefix) == 0)
			return true;
	return false;
}

// Does a live mount sit UNDER this path? (Contract in vfs.h.) The same scan
// unmount runs against the victim it is about to strike, asked here on behalf
// of unlink and rename — because a mount point's parent is an ordinary
// directory on an ordinary filesystem, and nothing about removing it consults
// the namespace. The mount would keep routing (longest-prefix matching is
// pure string work) while its point had no parent to be listed in: the child
// vanishes from every listing and `ls` is asked to invent the directory the
// mount verb refused to invent. Take the table lock: unlike unmount's copy of
// this scan, the caller here does not hold the namespace against mutations.
bool vfs_mount_under(const char *path)
{
	if (path == NULL || path[0] != '/' || path[1] == '\0')
		return false;   // "/" is not removable through any door, and everything is under it

	size_t len = strlen(path);
	bool found = false;
	spinlock_acquire(&kMountTableLock);
	for (int i = 0; i < kMountCount; i++)
		if (kMountTable[i].fs != NULL &&
		    strncmp(kMountTable[i].prefix, path, len) == 0 &&
		    kMountTable[i].prefix[len] == '/')
		{
			found = true;
			break;
		}
	spinlock_release(&kMountTableLock);
	return found;
}

// The stray-write tripwire's oracle (contract in vfs.h): is the partition at
// (dev, partNo) backed by a mount that installed a write path? The per-mount
// fops COPY is deliberately the thing consulted — kRegisterFilesystem clones
// the op tables per mount, so a read-only ext2 root and a writable /ext2
// secondary give different answers for the same driver. Matching on the
// device POINTER is sound: block_device_info_t objects are created once at
// detection and never move; the fs stores the same pointer the block layer
// hands to bops->write.
bool vfs_partition_mount_writable(block_device_info_t *dev, int partNo)
{
	for (int i = 0; i < kMountCount; i++)
	{
		vfs_filesystem_t *fs = kMountTable[i].fs;
		if (fs == NULL || fs->block_device_info != dev || fs->partNumber != partNo)
			continue;
		return !fs->read_only && fs->fops != NULL && fs->fops->write != NULL;
	}
	return false;   // no mount claims it — nothing legitimate writes there
}

// ── vfs_demote_all_mounts_readonly (TEST_RO's engine, 2026-08-08) ────────────
// The errors=remount-ro descendant: when a write-path test fails, continuing
// to write is how a bad driver compounds a bad day — but halting costs the
// operator the live system they need for analysis. So: take the pens away,
// keep the lights on. NULLing the write verbs on each mount's PRIVATE op-table
// copies (kRegisterFilesystem clones them per mount — the same property the
// stray-write tripwire builds on) makes every dispatch site refuse exactly the
// way a born-read-only mount refuses: syscall_write/unlink/mkdir already
// null-check these slots (ext2's forced_ro mounts ship NULLs from birth), the
// block tripwire's vfs_partition_mount_writable() reads these same copies so
// anything that DOES sneak a write past the fs layer panics by name, and logd
// stops reading the rings when its writes fail (its own starve-the-file rule)
// so the log rides serial again. Open files keep reading — their vfs_file_t
// fops POINT at the mount's copy, so the demotion reaches them too.
// The drill flag — see vfs.h for what it does and why it is not a mute
// switch. Plain bool, written only by the test thread between tests and read
// only when an alarm is already being raised; there is no race worth a lock
// here, and taking one inside a failure path would be its own hazard.
bool kTestingExpectedNoise = false;

void vfs_demote_mount_readonly(vfs_filesystem_t *fs)
{
	if (fs == NULL)
		return;

	// Publish the state FIRST. A callback retained before its slot is cleared
	// must see the demotion when it reaches its filesystem's write lock —
	// and an OPEN that creates must see it too, since no slot can be taken
	// away to stop that one (ext2_open_rw and fat_open both read this flag).
	fs->read_only = true;
	if (fs->fops != NULL)
	{
		fs->fops->write   = NULL;
		fs->fops->sync    = NULL;
		fs->fops->flush   = NULL;
		fs->fops->fputs   = NULL;
		fs->fops->fprintf = NULL;
		fs->fops->rm      = NULL;
		fs->fops->rename  = NULL;
	}
	if (fs->dops != NULL)
		fs->dops->mkdir = NULL;
}

void vfs_demote_all_mounts_readonly(const char *why)
{
	int demoted = 0;
	for (int i = 0; i < kMountCount; i++)
	{
		vfs_filesystem_t *fs = kMountTable[i].fs;
		if (fs == NULL || fs->fops == NULL || fs->fops->write == NULL)
			continue;
		vfs_demote_mount_readonly(fs);
		demoted++;
		printf("VFS: '%s' demoted to READ-ONLY (%s)\n", kMountTable[i].prefix, why);
		printd(DEBUG_VFS, "VFS: mount '%s' demoted to read-only: %s\n",
		       kMountTable[i].prefix, why);
	}
	if (demoted > 0)
		printf("VFS: %d mount(s) now read-only — analyze, then reboot to restore writes\n",
		       demoted);
}

// The panic-message companion (contract in vfs.h): mounted at all?
bool vfs_partition_mounted(block_device_info_t *dev, int partNo)
{
	for (int i = 0; i < kMountCount; i++)
	{
		vfs_filesystem_t *fs = kMountTable[i].fs;
		if (fs != NULL && fs->block_device_info == dev && fs->partNumber == partNo)
			return true;
	}
	return false;
}

// Which op tables (and which name) a partition's filesystem gets — one
// answer for both mount doors, the boot list and the mount syscall, so a
// partition mounts the same way however it arrives. Returns false for a
// filesystem os64 does not recognize.
//
// `ro` is the CALLER's read-only intent (OS64_MOUNT_RO, or an `ro` token on
// a mounts.conf line): the mount gets tables with the write verbs absent
// FROM BIRTH, the same NULL-slot shape a device that cannot write produces —
// so every dispatch site, and the stray-write tripwire reading the per-mount
// copies, enforces the request without a single new check anywhere.
static bool vfs_ops_for_part(block_device_info_t *devinfo, partEntry_t *part,
                             vfs_file_operations_t *fileOps,
                             vfs_directory_operations_t *dirOps,
                             const char **fsName, bool ro)
{
	switch (part->filesystemType)
	{
		case FILESYSTEM_TYPE_FAT32:
			*fsName = "fat";
			*fileOps = fat_fops;  *dirOps = fat_dops;
			// FAT has one pair, so read-only is that pair minus its pens —
			// the same slots vfs_demote_mount_readonly takes away, removed
			// before the mount is born instead of after it misbehaved.
			// Taking the pens away is not the whole answer for FAT: its
			// open CREATES (fat_glue.c says why), so fat_open asks
			// fs->read_only for itself. Both mechanisms, or a read-only
			// mount is one `>` away from a stray-write panic.
			if (ro)
			{
				fileOps->write   = NULL;
				fileOps->sync    = NULL;
				fileOps->flush   = NULL;
				fileOps->fputs   = NULL;
				fileOps->fprintf = NULL;
				fileOps->rm      = NULL;
				fileOps->rename  = NULL;
				dirOps->mkdir    = NULL;
			}
			return true;
		case FILESYSTEM_TYPE_EXT2:
			*fsName = "ext2";
			// Ext2 mounts get the WRITABLE pair (secondaries since
			// 2026-08-04's shakedown; the root joined 2026-08-07 when
			// writable-root was ratified). ext2_initialize_filesystem
			// may still strip the write slots if the disk's ro_compat
			// features outrun us — and a DEVICE whose driver cannot
			// write demotes the mount the same way (2026-08-08: the
			// P5's SATA disk, AHCI being read-only today). A REQUESTED
			// read-only mount takes the same pair — its `mounted` slot
			// reports leftover orphans without reclaiming them, which is
			// exactly what a mount that must not write should say.
			if (ro || devinfo->block_device->ops->write == NULL)
			{
				*fileOps = ext2_fops; *dirOps = ext2_dops;
				if (!ro)
					printf("%s: device's driver cannot write — mounting READ-ONLY\n",
					       vfs_partname_usable(part->partName) ? part->partName : *fsName);
			}
			else
			{
				ext2_rw_tables_init();
				*fileOps = ext2_rw_fops; *dirOps = ext2_rw_dops;
			}
			return true;
		default:
			return false;
	}
}

// Derive the mount prefix a partition gets when nobody names one: its own
// GPT name when usable, its filesystem type otherwise. First claimant mounts
// bare ("/fat"); anyone who wants a name that's taken gets a number starting
// at 2 ("/fat2"), like device names always have. Two partitions CAN carry
// the same GPT name — nothing stops a disk from having two called "home" —
// so this has to hold for names exactly as it did for filesystem types.
static void vfs_derive_mount_prefix(partEntry_t *part, const char *fsName,
                                    char *prefix /* VFS_MOUNT_PREFIX_MAX */)
{
	const char *baseName = vfs_partname_usable(part->partName)
	                       ? part->partName : fsName;
	sprintf(prefix, "/%s", baseName);
	for (unsigned n = 2; n < 100 && vfs_prefix_in_use(prefix); n++)
		sprintf(prefix, "/%s%u", baseName, n);
}

// ── /etc/mounts.conf — what joins the namespace at boot ─────────────────────
// The WHAT half of the question fstab answered with one table. GPT answered
// the WHERE half (the partition's own label — see vfs_partname_usable), so
// this file only says which partitions THIS machine brings into its
// namespace — the half that genuinely is local policy. House syntax:
//
//     # what joins the namespace once the root is up
//     mount = home
//     mount = scratch /mnt ro
//
// `mount = <gpt-name-or-guid> [/where] [ro]`, repeatable, applied in order.
// No /where means "mount it at its own label" (vfs_derive_mount_prefix).
// gui.conf's rule: if the file EXISTS, its lines are the WHOLE list — an
// empty file mounts nothing beyond root and the synthetics — and only an
// ABSENT file falls back to the built-in default (the authored-GUIDs sweep
// below). That rule is also why no example file ships in /etc: a shipped
// copy full of comments would read as "mount nothing". Root itself never
// appears here: the file lives ON root, and the lifeboat's premise is
// booting when that root is broken — ROOT= on the cmdline stays the one
// answer given from outside.
//
// The file is read straight off the root fs, NOT through the conf ladder —
// it is upstream of the ladder (it decides whether /home mounts at all),
// the same reasoning that makes os64.conf the one unsearched file.

#define VFS_MOUNTS_CONF        "/etc/mounts.conf"
#define VFS_MOUNTS_CONF_CAP    8192

static bool vfs_find_part(const char *what, int *out_idx, int *out_partno);

// Case-folding equality for SETTING words ("mount", "ro") — never for the
// partition names beside them, which are data and compared verbatim (the
// conf doctrine: folding is the reader's choice, and only for settings).
static bool vfs_word_eq_nocase(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0')
	{
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb)
			return false;
		a++; b++;
	}
	return *a == *b;
}

// Slurp the file whole, or not at all. NULL means "behave as if absent":
// silently for a file that isn't there, LOUDLY for one that is but can't be
// handed back complete — conf.c's discipline (an error is not an ending,
// and a truncated prefix must never pass as the whole policy).
static char *vfs_mounts_conf_slurp(void)
{
	const char *tail = NULL;
	vfs_filesystem_t *fs = vfs_resolve_mount(VFS_MOUNTS_CONF, &tail);
	if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL || fs->fops->read == NULL)
		return NULL;

	vfs_file_t *file = NULL;
	if (fs->fops->open(&file, tail, "r", fs) != 0)
		return NULL;   // no file — the default policy applies

	char *buf = kmalloc(VFS_MOUNTS_CONF_CAP + 1);
	size_t len = 0;
	bool refuse = (buf == NULL);
	while (!refuse)
	{
		if (len >= VFS_MOUNTS_CONF_CAP)
		{
			char probe;
			if (fs->fops->read(file, &probe, 1) != 0)
				refuse = true;   // continues past the cap, or unreadable
			break;
		}
		int n = fs->fops->read(file, buf + len, VFS_MOUNTS_CONF_CAP - len);
		if (n < 0) { refuse = true; break; }
		if (n == 0) break;
		len += (size_t)n;
	}
	if (fs->fops->close != NULL)
		fs->fops->close(file);

	if (refuse)
	{
		printf("mounts.conf: unreadable or larger than %u bytes — REFUSED, mounting the built-in default set\n",
		       (unsigned)VFS_MOUNTS_CONF_CAP);
		printd(DEBUG_BOOT, "BOOT: mounts.conf refused (oversize/unreadable) — default policy applies\n");
		if (buf != NULL)
			kfree(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static const char *vfs_mount_result_phrase(int r)
{
	switch (r)
	{
		case OS64_MOUNT_BAD_ARGS:        return "bad arguments";
		case OS64_MOUNT_NOT_FOUND:       return "no partition wears that name or GUID";
		case OS64_MOUNT_ALREADY_MOUNTED: return "that partition is already mounted";
		case OS64_MOUNT_PREFIX_IN_USE:   return "something else is mounted there";
		case OS64_MOUNT_NO_PARENT:       return "the mount point's parent does not exist";
		case OS64_MOUNT_TABLE_FULL:      return "the mount table is full";
		case OS64_MOUNT_UNSUPPORTED:     return "no filesystem os64 recognizes on it";
		default:                         return "the driver refused it";
	}
}

// One `mount =` line, already split off its key. A bad line is skipped with
// its reason on the glass; it never takes the rest of the file down with it
// (an absent thumb drive must not cost /home its mount).
static void vfs_mounts_conf_line(char *value)
{
	char *tok[4] = { NULL, NULL, NULL, NULL };
	int ntok = 0;
	for (char *p = value; *p != '\0' && ntok < 4; )
	{
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0')
			break;
		tok[ntok++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t') p++;
		if (*p != '\0')
			*p++ = '\0';
	}
	if (ntok == 0)
	{
		printf("mounts.conf: a mount line with nothing to mount — skipped\n");
		return;
	}

	const char *what = tok[0];
	const char *where = NULL;
	bool ro = false;
	for (int i = 1; i < ntok; i++)
	{
		if (tok[i][0] == '/' && where == NULL)
			where = tok[i];
		else if (vfs_word_eq_nocase(tok[i], "ro"))
			ro = true;
		else if (vfs_word_eq_nocase(tok[i], "rw"))
			;   // the default, spelled out
		else
		{
			printf("mounts.conf: \"%s\" is not a mount point, ro, or rw — line for %s skipped\n",
			       tok[i], what);
			return;
		}
	}

	char derived[VFS_MOUNT_PREFIX_MAX];
	if (where == NULL)
	{
		int idx, partno;
		if (!vfs_find_part(what, &idx, &partno))
		{
			printf("mounts.conf: no partition named \"%s\" — line skipped\n", what);
			return;
		}
		partEntry_t *part = kBlockDeviceInfo[idx].block_device->partition_table->parts[partno];
		const char *fsName;
		vfs_file_operations_t fileOps;
		vfs_directory_operations_t dirOps;
		if (!vfs_ops_for_part(&kBlockDeviceInfo[idx], part, &fileOps, &dirOps, &fsName, false))
		{
			printf("mounts.conf: %s — %s\n", what, vfs_mount_result_phrase(OS64_MOUNT_UNSUPPORTED));
			return;
		}
		vfs_derive_mount_prefix(part, fsName, derived);
		where = derived;
	}

	int r = vfs_mount_explicit(what, where, ro ? OS64_MOUNT_RO : 0);
	if (r == OS64_MOUNT_OK)
		printf("mounted %s at %s%s\n", what, where, ro ? " (ro)" : "");
	else
		printf("mounts.conf: %s at %s — %s\n", what, where, vfs_mount_result_phrase(r));
}

// Returns true when the file existed and spoke — the caller then mounts
// NOTHING on its own, however few lines the file carried.
static bool vfs_mounts_conf_apply(void)
{
	char *buf = vfs_mounts_conf_slurp();
	if (buf == NULL)
		return false;

	printd(DEBUG_BOOT, "BOOT: /etc/mounts.conf present — it is the whole boot-mount list\n");
	int applied = 0;
	char *line = buf;
	while (line != NULL)
	{
		char *next = NULL;
		for (char *p = line; *p != '\0'; p++)
			if (*p == '\n') { *p = '\0'; next = p + 1; break; }

		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		size_t slen = strlen(s);
		if (slen > 0 && s[slen - 1] == '\r')
			s[slen - 1] = '\0';

		if (*s != '\0' && *s != '#')
		{
			char *eq = NULL;
			for (char *p = s; *p != '\0'; p++)
				if (*p == '=') { eq = p; break; }
			if (eq == NULL)
				printf("mounts.conf: no '=' in \"%s\" — line skipped\n", s);
			else
			{
				*eq = '\0';
				// Trim the key, fold-compare it (a setting name); the value
				// beyond '=' is data and vfs_mounts_conf_line splits it raw.
				char *k = s;
				while (*k == ' ' || *k == '\t') k++;
				char *kend = k + strlen(k);
				while (kend > k && (kend[-1] == ' ' || kend[-1] == '\t'))
					*--kend = '\0';
				char *v = eq + 1;
				while (*v == ' ' || *v == '\t') v++;
				if (vfs_word_eq_nocase(k, "mount"))
				{
					vfs_mounts_conf_line(v);
					applied++;
				}
				else
					printf("mounts.conf: \"%s\" is not a setting this file knows — line skipped\n", k);
			}
		}
		line = next;
	}
	if (applied == 0)
		printd(DEBUG_BOOT, "BOOT: mounts.conf carried no mount lines — nothing joins the namespace beyond root\n");
	kfree(buf);
	return true;
}

// What joins the namespace besides root — once at boot, after the root is
// claimed (partition tables were already detected by vfs_mount_root_part's
// first pass). /etc/mounts.conf decides when it exists; the sweep below is
// the ABSENT-file default: every recognized, os64-authored partition, at its
// GPT name.
static void vfs_mount_secondary_partitions(void)
{
	if (vfs_mounts_conf_apply())
		return;

	for (int idx = 0; idx < kBlockDeviceInfoCount; idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_SATA_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_NVME_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_HD)
			continue;

		for (int partno = 0; partno < kBlockDeviceInfo[idx].block_device->part_count; partno++)
		{
			partEntry_t *part = kBlockDeviceInfo[idx].block_device->partition_table->parts[partno];

			// fsName is what the FILESYSTEM is (for the messages, and the
			// fallback mount point).
			const char *fsName;
			vfs_file_operations_t fileOps;
			vfs_directory_operations_t dirOps;
			if (!vfs_ops_for_part(&kBlockDeviceInfo[idx], part, &fileOps, &dirOps, &fsName, false))
				continue;   // unrecognized/no filesystem — not mountable

			const char *baseName = vfs_partname_usable(part->partName)
			                       ? part->partName : fsName;

			if (!vfs_guid_is_ours(part->uniquePartGUID))
			{
				// Somebody else's partition (a Windows ESP, a Linux root,
				// who knows) — the default policy touches NOTHING it didn't
				// author. A mounts.conf line naming it is how it gets in.
				printf("skipping foreign %s partition (device %u part %u)\n",
				       baseName, idx, partno);
				printd(DEBUG_BOOT, "BOOT: foreign %s partition on device %u part %u — not in the default set, not mounted\n",
				       baseName, idx, partno);
				continue;
			}

			if (vfs_guid_already_mounted(part->uniquePartGUID))
			{
				printd(DEBUG_BOOT, "BOOT: skipping %s partition %u on device %u — GUID already mounted (twin)\n",
				       baseName, partno, idx);
				continue;
			}

			char prefix[VFS_MOUNT_PREFIX_MAX];
			vfs_derive_mount_prefix(part, fsName, prefix);

			vfs_filesystem_t *fs = kRegisterFilesystem(prefix, &kBlockDeviceInfo[idx],
			                                           partno, &fileOps, &dirOps, NULL);
			if (fs != NULL)
			{
				printd(DEBUG_BOOT, "BOOT: mounted %s (device %u partition %u, GPT name \"%s\") at %s\n",
				       fsName, idx, partno, part->partName, prefix);
				// Screen too (permanent): on real hardware the sweep meets
				// partitions we didn't author — a Windows EFI partition, a
				// recovery blob — and what got mounted WHERE is exactly the
				// context a screen-only machine needs when something fails.
				printf("mounted %s at %s\n", fsName, prefix);
			}
			else
				printf("mount of %s (device %u part %u) FAILED\n", fsName, idx, partno);
		}
	}
}

int vfs_mount_root_part(char* rootPartUUID)
{
	vfs_file_operations_t fileOps;
	vfs_directory_operations_t dirOps;
	bool mounted=false;

	//First we need to get the partition tables of all of the detected drives, and identify the partition types of each of the partitions
	// Name the devices the moment the boot knows them all — before any
	// mount, so every later print of a device speaks the system's name.
	block_assign_dev_names();

	for (int idx=0;idx<kBlockDeviceInfoCount;idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_SATA_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_NVME_HD ||  kBlockDeviceInfo[idx].ATADeviceType == ATA_DEVICE_TYPE_HD)
		{
			kBlockDeviceInfo[idx].block_device->partTableType = detect_partition_table_type(&kBlockDeviceInfo[idx]);
			detect_partition_filesystem_types(&kBlockDeviceInfo[idx]);
		}
	}

	//Then we need to look for the partition UUID mentioned in the boot commandline parameter ROOTPARTUUID.  If it is found we'll mount it as the root of the filesystem
	for (int idx=0;idx<kBlockDeviceInfoCount;idx++)
	{
		for (int partno=0;partno<kBlockDeviceInfo[idx].block_device->part_count;partno++)
		{
			if (compare_part_uuids(rootPartUUID, (char*)kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->uniquePartGUID))
			{
				switch (kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->filesystemType)
				{
					case FILESYSTEM_TYPE_EXT2:
						// The OS that got off FAT: root on a real ext2
						// partition — WRITABLE, ratified 2026-08-07 after
						// the write driver's shakedown tour on secondary
						// mounts (2026-08-04 arc: first-boot green, e2fsck
						// clean, refuse-while-open, full write-through).
						// The seatbelts that made the ruling comfortable:
						// the FAT lifeboat keeps its own /bin and boot
						// entry for the day a stray write eats root, /home
						// lives on its own partition so user data never
						// shares fate with system space, and `make
						// fsck-ext2` remains the constitution — e2fsck
						// stays green or the write path is wrong.
						// ext2_initialize_filesystem still strips the
						// write slots if the disk's ro_compat features
						// outrun the driver.
						//
						// A DEVICE that cannot write demotes the mount the
						// same way ro_compat does (2026-08-08, the P5's
						// SATA disk: the AHCI driver is read-only today).
						// The filesystem's willingness means nothing on a
						// disk the driver can't put bytes onto — mount
						// read-only, say so on the glass, and the suite's
						// write tests skip gracefully instead of failing
						// against a wall. Retires when AHCI grows its
						// write half.
						if (kBlockDeviceInfo[idx].block_device->ops->write == NULL)
						{
							fileOps = ext2_fops;
							dirOps = ext2_dops;
							printf("root device's driver cannot write — ext2 root mounted READ-ONLY\n");
							printd(DEBUG_BOOT, "BOOT: Root filesystem found (ext2), device write-less, mounting read-only\n");
						}
						else
						{
							ext2_rw_tables_init();
							fileOps = ext2_rw_fops;
							dirOps = ext2_rw_dops;
							printd(DEBUG_BOOT, "BOOT: Root filesystem found (ext2), mounting read-write\n");
						}
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps, NULL);
						mounted = (kRootFilesystem != NULL);
						// The glass line (2026-08-08): the SECONDARY mounts
						// announce on the framebuffer, but the root — the
						// biggest mount news of any boot — only spoke via
						// printd, so a nolog boot (the P5's) mounted its
						// root in total silence. Parity for the headliner.
						if (mounted)
							printf("mounted ext2 at / (root, %s)\n",
							       (fileOps.write != NULL) ? "read-write" : "read-only");
						break;
					case FILESYSTEM_TYPE_FAT32:
						fileOps = fat_fops;
						dirOps = fat_dops;
						printd(DEBUG_BOOT, "BOOT: Root filesystem found, mounting\n");
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps, NULL);
						mounted = (kRootFilesystem != NULL);
                        if (mounted)
                            printd(DEBUG_BOOT, "BOOT: Root filesystem successfully mounted\n");
                        break;
					default:
						panic("Could not mount root filesystem, type=%u", kBlockDeviceInfo[idx].block_device->partition_table->parts[partno]->filesystemType);
					break;
				}
			}
			if (mounted)
				break;
		}
		if (mounted)
			break;
	}
	if (kRootFilesystem==NULL)
		panic("BOOT: Could not find/mount root filesystem\n");

	// With the root claimed, sweep the remaining partitions and auto-mount
	// every recognized filesystem at "/<fstype>" — whichever partition ISN'T
	// the root becomes reachable by prefix ("/fat" when ext2 is root, "/ext2"
	// when FAT is). Twins are skipped by partition GUID: a RAMDisk boot sees
	// the RAMDisk copy AND the NVMe original with identical GUIDs, and
	// mounting both would put two names on the same nominal volume — first
	// registered wins, the same rule the root scan applies.
	vfs_mount_secondary_partitions();
	return 0;
}
// ── The open-file registry (the sync(8) slice, 2026-08-06) ──────────────────
// One global intrusive list of every open vfs_file, threaded through the
// openNext/openPrev links in struct file. The fs glues call register at the
// bottom of a successful open, mark_closing at the TOP of close, and
// unregister at the BOTTOM of it (the `closing` contract in vfs.h — the two
// walkers want opposite answers about a file that is mid-close, and a file
// that had left the list while still touching its filesystem let an unmount
// tear that filesystem down underneath it). The walkers are vfs_sync_all
// below, unmount's busy count (vfs_openfiles_on), and /sys/openfiles'
// snapshot. This exists because sync(1)'s whole job is
// reaching OTHER tasks' open files — the kernel had no way to enumerate them
// (the "ping.log reads empty while ping runs" mystery, solved the same
// afternoon FAT's deferred directory-entry update was identified as 1977
// working as designed).
//
// LOCK DISCIPLINE: kOpenFileLock is the PLAIN spinlock (interrupts stay on)
// because the sync walk performs real disk I/O under it and disk completion
// paths read the tick clock — see spinlock.h for the full argument. Nothing
// here may ever be called from interrupt or fault context.
static spinlock_t kOpenFileLock = 0;
static vfs_file_t *kOpenFileHead = NULL;

void vfs_openfile_register(vfs_file_t *file)
{
	if (file == NULL)
		return;
	// The one moment the caller's path is guaranteed alive (both glues set
	// f_path before registering) — take the owned copy the registry renders
	// from (reg_path's contract in vfs.h).
	if (file->f_path != NULL)
	{
		strncpy(file->reg_path, file->f_path, sizeof(file->reg_path) - 1);
		file->reg_path[sizeof(file->reg_path) - 1] = '\0';
	}
	spinlock_acquire(&kOpenFileLock);
	file->openPrev = NULL;
	file->openNext = kOpenFileHead;
	if (kOpenFileHead != NULL)
		kOpenFileHead->openPrev = file;
	kOpenFileHead = file;
	spinlock_release(&kOpenFileLock);
}

void vfs_openfile_mark_closing(vfs_file_t *file)
{
	if (file == NULL)
		return;
	spinlock_acquire(&kOpenFileLock);
	file->closing = true;
	spinlock_release(&kOpenFileLock);
}

void vfs_openfile_unregister(vfs_file_t *file)
{
	if (file == NULL)
		return;
	spinlock_acquire(&kOpenFileLock);
	// Unlinking a file that was never registered (both links NULL and not
	// the head) is a harmless no-op rather than a corruption — glue close
	// paths run on files from every era of caller.
	if (file->openPrev != NULL)
		file->openPrev->openNext = file->openNext;
	else if (kOpenFileHead == file)
		kOpenFileHead = file->openNext;
	if (file->openNext != NULL)
		file->openNext->openPrev = file->openPrev;
	file->openNext = NULL;
	file->openPrev = NULL;
	spinlock_release(&kOpenFileLock);
}

int64_t vfs_sync_all(void)
{
	int64_t synced = 0;
	bool anyFailed = false;

	spinlock_acquire(&kOpenFileLock);
	for (vfs_file_t *f = kOpenFileHead; f != NULL; f = f->openNext)
	{
		if (f->closing)
			continue;   // mid-close: its data is already committed or lost, and
			            // its handle is one instruction from being freed
		if (f->fops == NULL || f->fops->sync == NULL)
			continue;   // read-only mount or a filesystem with nothing to say
		if (f->fops->sync(f) < 0)
			anyFailed = true;   // note it, keep sweeping — a broom does not
			                    // stop at the first dusty corner
		else
			synced++;
	}
	spinlock_release(&kOpenFileLock);

	printd(DEBUG_VFS, "vfs_sync_all: %ld file(s) synced%s\n",
	       synced, anyFailed ? " (with failures)" : "");
	return anyFailed ? -1 : synced;
}

// How many open files sit on `fs` — unmount's first busy question, and
// /sys/mounts' "open" column. The registry was built for sync(8); this is
// the second customer its comment predicted.
//
// A file being CLOSED right now counts. Close is not a path operation, so
// the gate cannot hold one off, and both glues keep using the filesystem
// after they start closing (FAT's f_close is disk I/O; ext2 reads
// fs_specific and may reap an orphan). Counting it makes the unmount say
// BUSY for the microseconds that takes, which is the honest answer — the
// alternative was freeing the filesystem under a close in flight.
int vfs_openfiles_on(vfs_filesystem_t *fs)
{
	int n = 0;
	spinlock_acquire(&kOpenFileLock);
	for (vfs_file_t *f = kOpenFileHead; f != NULL; f = f->openNext)
		if (f->owner == fs)
			n++;
	spinlock_release(&kOpenFileLock);
	return n;
}

// /sys/openfiles' data (contract in vfs.h): copy the registry out row by
// row so rendering happens after the lock is gone. vfs_prefix_for_fs only
// COMPARES the fs pointer against the mount table — it never dereferences
// it — so asking under this lock is sound, and the nesting (open-file lock,
// then mount-table lock) has no reverse twin: nothing that holds the mount
// table lock ever takes this one.
int vfs_openfiles_snapshot(vfs_openfile_row_t *rows, int max_rows)
{
	int total = 0;
	spinlock_acquire(&kOpenFileLock);
	for (vfs_file_t *f = kOpenFileHead; f != NULL; f = f->openNext, total++)
	{
		if (rows == NULL || total >= max_rows)
			continue;   // keep counting so the caller can say how much was cut
		vfs_openfile_row_t *r = &rows[total];
		r->mount[0] = '\0';
		if (f->owner != NULL)
			vfs_prefix_for_fs((vfs_filesystem_t *)f->owner, r->mount, sizeof(r->mount));
		// reg_path, never f_path: the caller's pointer may be long dead by
		// now (reg_path's contract in vfs.h). Every allocation is zeroed, so
		// an empty copy means the open carried no path at all.
		if (f->reg_path[0] != '\0')
		{
			strncpy(r->tail, f->reg_path, sizeof(r->tail) - 1);
			r->tail[sizeof(r->tail) - 1] = '\0';
		}
		else
		{
			r->tail[0] = '-';
			r->tail[1] = '\0';
		}
		r->ident = f->f_ident;
		r->handles = f->handleRefCount;
	}
	spinlock_release(&kOpenFileLock);
	return total;
}

bool vfs_prefix_for_fs(vfs_filesystem_t *fs, char *out, size_t cap)
{
	if (fs == NULL || out == NULL || cap == 0)
		return false;
	spinlock_acquire(&kMountTableLock);
	for (int i = 0; i < kMountCount; i++)
	{
		if (kMountTable[i].fs != fs)
			continue;
		// Root answers "" so prefix+tail concatenates to the tail itself.
		const char *p = (kMountTable[i].prefix_len == 1) ? "" : kMountTable[i].prefix;
		size_t need = strlen(p) + 1;
		if (need > cap)
			break;
		strcpy(out, p);
		spinlock_release(&kMountTableLock);
		return true;
	}
	spinlock_release(&kMountTableLock);
	return false;
}

// ── The namespace verbs (contracts in vfs.h and os64/mount.h) ────────────────
// KERNEL CONTEXT REQUIRED for both: mounting stats the parent and runs the
// driver's initialize, unmounting runs uninitialize — all real disk I/O. The
// syscalls hop via call_in_kernel_context; the fixture calls from kernel_init.

// Find the partition `what` names — GPT name first (verbatim compare: names
// are data), the dashed GUID spelling second. Returns false if nothing wears
// that name.
//
// BOTH COMPARES ARE EXACT. partName is a 36-byte array that a full-length
// name fills with no terminator (gpt.c), so the compare has to be bounded —
// and a bounded compare alone made `<36-char-name>junk` a hit, because it
// stopped looking at 36. The length test is what turns "starts with" back
// into "is".
static bool vfs_find_part(const char *what, int *out_idx, int *out_partno)
{
	for (int idx = 0; idx < kBlockDeviceInfoCount; idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_SATA_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_NVME_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_HD)
			continue;
		for (int partno = 0; partno < kBlockDeviceInfo[idx].block_device->part_count; partno++)
		{
			partEntry_t *part = kBlockDeviceInfo[idx].block_device->partition_table->parts[partno];
			bool hit = (part->partName[0] != '\0' && strlen(what) <= 36 &&
			            strncmp(what, part->partName, 36) == 0) ||
			           (compare_part_uuids(what, (const char *)part->uniquePartGUID) != NULL);
			if (hit)
			{
				*out_idx = idx;
				*out_partno = partno;
				return true;
			}
		}
	}
	return false;
}

// The mount verb's body, run under kNamespaceLock. Everything from here down
// is read-then-act on the namespace — "is this GUID mounted", "is this prefix
// free", then the claim — and the claim re-asks neither question.
static int vfs_mount_locked(const char *what, const char *where, uint64_t flags, bool derive)
{
	int idx = -1, partno = -1;
	if (!vfs_find_part(what, &idx, &partno))
		return OS64_MOUNT_NOT_FOUND;
	partEntry_t *part = kBlockDeviceInfo[idx].block_device->partition_table->parts[partno];

	// No authorship test here, on purpose (it stood 2026-08-30/31 as a
	// Windows-safety gate, then mounts.conf arrived): a partition named by
	// its label or GUID to an explicit verb — typed, or written into the
	// boot list — IS the deliberateness. The default boot policy is where
	// "not ours, not touched" still lives.
	if (vfs_guid_already_mounted(part->uniquePartGUID))
		return OS64_MOUNT_ALREADY_MOUNTED;

	const char *fsName;
	vfs_file_operations_t fileOps;
	vfs_directory_operations_t dirOps;
	if (!vfs_ops_for_part(&kBlockDeviceInfo[idx], part, &fileOps, &dirOps, &fsName,
	                      (flags & OS64_MOUNT_RO) != 0))
		return OS64_MOUNT_UNSUPPORTED;

	char derived[VFS_MOUNT_PREFIX_MAX];
	if (derive)
	{
		vfs_derive_mount_prefix(part, fsName, derived);   // numbering dodges any collision
		where = derived;
	}
	else if (vfs_prefix_in_use(where))
		return OS64_MOUNT_PREFIX_IN_USE;

	// THE PARENT MUST BE REAL (ratified 2026-08-30): the mount point itself
	// is the table's — no directory needed — but its parent has to be an
	// existing directory on a disk mount (or "/"), so `ls` never has to
	// invent a directory that exists nowhere. (A derived point sits at root
	// level, so this walk lands on "/" and asks nothing.)
	const char *last = where + strlen(where);
	while (last > where && *last != '/')
		last--;
	if (last != where)   // parent is not "/"
	{
		char parent[VFS_MOUNT_PREFIX_MAX];
		memcpy(parent, where, (size_t)(last - where));
		parent[last - where] = '\0';
		const char *ptail = NULL;
		vfs_filesystem_t *pfs = vfs_resolve_mount(parent, &ptail);
		os64_dirent_t entry;
		if (pfs == NULL || pfs->block_device_info == NULL ||
		    pfs->dops == NULL || pfs->dops->stat == NULL ||
		    pfs->dops->stat(ptail, &entry, pfs) != 0 ||
		    (entry.flags & OS64_DE_DIR) == 0)
			return OS64_MOUNT_NO_PARENT;
	}

	int reason = OS64_MOUNT_OK;
	vfs_filesystem_t *fs = kRegisterFilesystem((char *)where, &kBlockDeviceInfo[idx],
	                                           partno, &fileOps, &dirOps, &reason);
	if (fs == NULL)
		return reason;   // driver refusal, a full table, no FatFs volume — it says which

	printd(DEBUG_VFS, "mount: %s (%s, GPT name \"%s\") at %s\n",
	       what, fsName, part->partName, where);
	return OS64_MOUNT_OK;
}

int vfs_mount_explicit(const char *what, const char *where, uint64_t flags)
{
	// Argument SHAPE first: nonsense needs no lock to refuse, and holding the
	// namespace against a whole disk mount is not something to pay for a typo.
	if (what == NULL || what[0] == '\0')
		return OS64_MOUNT_BAD_ARGS;
	if ((flags & ~(uint64_t)OS64_MOUNT_RO) != 0)
		return OS64_MOUNT_BAD_ARGS;   // an unknown bit must fail loudly, not grant less than asked

	// NULL/empty `where` means "at the partition's own label" — `mount fat`
	// and a one-token mounts.conf line are the same ask, and the answer
	// (label, fstype fallback, collision numbering) lives in one place.
	bool derive = (where == NULL || where[0] == '\0');
	if (!derive)
	{
		if (where[0] != '/')
			return OS64_MOUNT_BAD_ARGS;
		size_t wlen = strlen(where);
		if (wlen < 2 || wlen >= VFS_MOUNT_PREFIX_MAX || where[wlen - 1] == '/')
			return OS64_MOUNT_BAD_ARGS;   // "/" is not mountable-over; canonical paths carry no trailing slash
	}

	spinlock_acquire(&kNamespaceLock);
	// THE GATE, CLOSED ACROSS THE WHOLE VERB — mount is its second writer,
	// and for the same reason unmount is its first: everything below is
	// read-then-act on a namespace that path operations can move underneath
	// it. Both directions need it. Outbound, our parent check ("is /mnt a
	// directory?") is worthless if an unlink already in flight removes /mnt
	// between the stat and the claim. Inbound, unlink's own guard ("is
	// anything mounted under /mnt?") is worthless if we publish /mnt/usb
	// after it looked. Neither can be fixed on one side alone: the two
	// operations have to be ordered, and the gate is what orders them.
	//
	// Held across the driver's initialize, which is real disk I/O — the
	// trade unmount already makes, affordable for the same reason (these
	// verbs are rare and human-paced, and every path op is bounded).
	vfs_path_gate_close();
	int r = vfs_mount_locked(what, where, flags, derive);
	vfs_path_gate_open();
	spinlock_release(&kNamespaceLock);
	return r;
}

// The unmount verb's body, run under kNamespaceLock. Nothing here is safe to
// interleave with another namespace mutation: two callers reaching the strike
// with the same `fs` both tore it down, freeing every allocation twice.
static int vfs_unmount_locked(const char *where)
{
	// Identify the victim first, cheaply, before disturbing anything.
	spinlock_acquire(&kMountTableLock);
	vfs_mount_entry_t *m = NULL;
	for (int i = 0; i < kMountCount; i++)
		if (kMountTable[i].fs != NULL && strcmp(kMountTable[i].prefix, where) == 0)
		{
			m = &kMountTable[i];
			break;
		}
	vfs_filesystem_t *fs = m ? m->fs : NULL;
	spinlock_release(&kMountTableLock);

	if (fs == NULL)
		return OS64_UNMOUNT_NOT_A_MOUNT;
	if (m->prefix_len == 1)
		return OS64_UNMOUNT_ROOT;
	if (fs->block_device_info == NULL)
		return OS64_UNMOUNT_SYNTH;   // /proc, /sys, /dev — the machine's own furniture

	// A MOUNT UNDER THIS ONE HOLDS IT DOWN. Nothing about the child's own
	// filesystem breaks if the parent leaves — longest-prefix routing would
	// still find it — but the namespace around it stops making sense: the
	// child vanishes from root's synthetic listing (it lists as a CHILD of
	// /mnt, and /mnt is gone), its mount point's parent no longer exists, and
	// the next filesystem mounted at /mnt silently inherits it. Refused, not
	// cascaded: unmounting one thing must never take out a second thing the
	// caller did not name.
	//
	// No table lock for the scan: every mutation a running machine can make
	// is one of the two namespace verbs, and we hold the lock that makes
	// those exclusive. (The boot-time claims — synthfs_mount and the
	// partition sweep — go straight to vfs_mount_claim, and run before a
	// second caller exists.)
	for (int i = 0; i < kMountCount; i++)
	{
		if (kMountTable[i].fs == NULL || kMountTable[i].fs == fs)
			continue;
		if (strncmp(kMountTable[i].prefix, where, m->prefix_len) == 0 &&
		    kMountTable[i].prefix[m->prefix_len] == '/')
		{
			printd(DEBUG_VFS, "unmount: %s holds a mount at %s — unmount that first\n",
			       where, kMountTable[i].prefix);
			return OS64_UNMOUNT_HAS_MOUNTS;
		}
	}

	// Close the path gate: from here to the strike, no path operation is in
	// flight anywhere, so the busy answers below cannot be falsified by an
	// open that had resolved but not yet registered, and no new operation
	// can resolve to this fs (the strike happens before the gate reopens).
	vfs_path_gate_close();

	int open_files = vfs_openfiles_on(fs);
	int open_dirs  = fs->open_dir_count;
	bool cwd_inside = false;
	// The task spine is append-only (procfs walks it the same way); exited
	// tasks are skipped — a zombie's cwd is on its way to the undertaker.
	for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next)
	{
		if (t->exited || t->cwd == NULL)
			continue;
		if (strncmp(t->cwd, where, m->prefix_len) == 0 &&
		    (t->cwd[m->prefix_len] == '\0' || t->cwd[m->prefix_len] == '/'))
		{
			cwd_inside = true;
			break;
		}
	}

	if (open_files > 0 || open_dirs > 0 || cwd_inside)
	{
		vfs_path_gate_open();
		printd(DEBUG_VFS, "unmount: %s is busy — %d open file(s), %d open dir(s)%s\n",
		       where, open_files, open_dirs, cwd_inside ? ", a task's cwd inside" : "");
		return OS64_UNMOUNT_BUSY;
	}

	// The strike: clear `fs` first (the lock-free readers' marker), leave the
	// prefix bytes in place — a racing synthetic-readdir scan that already
	// passed the fs check prints a name one microsecond stale, which is what
	// a listing of a moving namespace means.
	spinlock_acquire(&kMountTableLock);
	m->fs = NULL;
	__sync_synchronize();
	m->fstype = NULL;
	spinlock_release(&kMountTableLock);

	vfs_path_gate_open();

	// Teardown, outside the gate — nothing can reach this fs any more — but
	// still under kNamespaceLock, so the FatFs volume number this gives back
	// cannot be handed to a new mount while the old one is still using it.
	vfs_teardown_filesystem(fs, true);

	printd(DEBUG_VFS, "unmount: %s unmounted\n", where);
	return OS64_MOUNT_OK;
}

int vfs_unmount(const char *where)
{
	if (where == NULL || where[0] != '/')
		return OS64_MOUNT_BAD_ARGS;

	spinlock_acquire(&kNamespaceLock);
	int r = vfs_unmount_locked(where);
	spinlock_release(&kNamespaceLock);
	return r;
}
