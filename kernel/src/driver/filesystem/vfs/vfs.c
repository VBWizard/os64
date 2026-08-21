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

uint8_t kFatDiskNumber=0;

// The mount table (see vfs.h for the design note). Filled by
// kRegisterFilesystem; consulted by vfs_resolve_mount. Boot-time only writes
// today, so no lock — revisit when mount/umount syscalls exist.
vfs_mount_entry_t kMountTable[VFS_MAX_MOUNTS];
int kMountCount = 0;

vfs_filesystem_t *vfs_resolve_mount(const char *canonical_path, const char **tail)
{
	vfs_mount_entry_t *best = NULL;

	for (int i = 0; i < kMountCount; i++)
	{
		vfs_mount_entry_t *m = &kMountTable[i];
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
		entry->flags = OS64_DE_DIR;   // a mount point always lists as a directory
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

vfs_filesystem_t* kRegisterFilesystem(char *mountPoint, block_device_info_t *device, int partNo, vfs_file_operations_t* fileOps, vfs_directory_operations_t* dirOps)
{
    vfs_filesystem_t *fs;

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
		fs->fatDiskNumber=++kFatDiskNumber;
	if (fs->fops->initialize != NULL)
	{
		// A failed initialize (unreadable superblock, feature we can't honor)
		// means NO mount: the fs never enters the mount table, so no path can
		// route to it. The dlist entry stays behind (unwinding it needs a
		// remove that doesn't exist yet) — harmless, its fatDiskNumber is
		// unique and nothing else matches on it. Boot-time, rare, logged.
		int initResult = fs->fops->initialize(fs);
		if (initResult != 0)
		{
			printd(DEBUG_BOOT, "BOOT: filesystem at %s failed to initialize (%d) — not mounted\n",
			       mountPoint, initResult);
			return NULL;
		}
	}
	// initialize may strip write slots after inspecting on-disk features.
	fs->read_only = fs->read_only || fs->fops->write == NULL;

	// Claim the prefix in the mount table — this is the moment the filesystem
	// becomes reachable by path.
	if (kMountCount >= VFS_MAX_MOUNTS)
	{
		// Same reasoning as synthfs_mount's copy: a partition that failed to
		// mount is an absence, and an absence needs someone to announce it
		// (vfs.h, THE CEILING).
		printd(DEBUG_BOOT, "BOOT: mount table full (%u), %s not mounted\n",
		       VFS_MAX_MOUNTS, mountPoint);
		printf("MOUNT TABLE FULL (%u) — %s NOT mounted\n", VFS_MAX_MOUNTS, mountPoint);
		return NULL;
	}
	vfs_mount_entry_t *m = &kMountTable[kMountCount];
	strncpy(m->prefix, mountPoint, VFS_MOUNT_PREFIX_MAX - 1);
	m->prefix[VFS_MOUNT_PREFIX_MAX - 1] = '\0';
	m->prefix_len = strlen(m->prefix);
	memcpy(m->part_guid,
	       device->block_device->partition_table->parts[partNo]->uniquePartGUID, 16);
	m->fs = fs;
	kMountCount++;

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

char* compare_part_uuids(const char* rootPartUUID, const char* currPartUUID)
{
	char* result = 0;

	//Make currPartUUID look like rootPartUUID
	//The format is U32-U16-U16-BYBY-BYBYBYBYBYBY
	uint32_t currPart1=(uint32_t)*(uint32_t*)currPartUUID;
	currPartUUID+=4;
	uint16_t currPart2=(uint16_t)*(uint16_t*)currPartUUID;
	currPartUUID+=2;
	uint16_t currPart3=(uint16_t)*(uint16_t*)currPartUUID;
	currPartUUID+=2;
	uint8_t currPart4[2];
	currPart4[0]=*(uint8_t*)currPartUUID;
	currPartUUID+=1;
	currPart4[1]=*(uint8_t*)currPartUUID;
	currPartUUID+=1;

	uint8_t currPart5[6];
	for (int cnt=0;cnt<6;cnt++)
	{
		currPart5[cnt]=*(uint8_t*)currPartUUID;
		currPartUUID+=1;
	}

	char* currPartStr=kmalloc(20);
	sprintf(currPartStr, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			currPart1, currPart2, currPart3, 
			currPart4[0], currPart4[1],
			currPart5[0], currPart5[1], currPart5[2], currPart5[3], currPart5[4], currPart5[5]);

	//Compare the formatted currPartUUID to the part UUID passed to be the root
	result = strnstr(rootPartUUID, currPartStr,36);

	printd(DEBUG_BOOT | DEBUG_DETAILED, "\tBOOT: Compared rootPartUUID=%s with partition %s, result=%p\n", rootPartUUID, currPartStr, result);

    kfree(currPartStr);

    if (result == rootPartUUID)
    {
        return result;
    }
	return NULL;
}

// Is this partition's GUID already backing a mount? (Dedupe: RAMDisk boots
// register the RAMDisk image AND leave the NVMe original visible, with
// identical GUIDs — see the call site comment.)
static bool vfs_guid_already_mounted(const uint8_t *guid)
{
	for (int i = 0; i < kMountCount; i++)
		if (memcmp(kMountTable[i].part_guid, guid, 16) == 0)
			return true;
	return false;
}

// THE AUTO-MOUNT ALLOWLIST: only partitions os64 itself authored may join
// the namespace uninvited. Learned on the Bosgame P5, 2026-07-19, the hard
// way: the first bare-metal boot of the mount sweep auto-mounted the
// machine's WINDOWS EFI SYSTEM PARTITION — writable — plus its recovery
// partition and a stray ext2. One husk redirection away from an unbootable
// Windows. Foreign partitions now stay untouched until a deliberate mount
// syscall exists; these GUIDs are the constants from the root GNUmakefile
// (DISK_PARTUUID / EXT2_PARTUUID), unchanged since the disk image was born.
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
// policy decision; here it is a property of the partition.
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
		if (strcmp(kMountTable[i].prefix, prefix) == 0)
			return true;
	return false;
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
	// must see the demotion when it reaches its filesystem's write lock.
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

// Auto-mount every recognized, OS64-AUTHORED non-root partition at its GPT
// partition NAME ("/home"), falling back to its filesystem type ("/fat",
// "/ext2") when the name is missing or unusable. Runs once at boot, after the
// root is claimed; partition tables were already detected by
// vfs_mount_root_part's first pass.
static void vfs_mount_secondary_partitions(void)
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

			// fsName is what the FILESYSTEM is (for the messages, and the
			// fallback mount point); baseName is what we'll actually mount it as.
			const char *fsName;
			vfs_file_operations_t fileOps;
			vfs_directory_operations_t dirOps;
			switch (part->filesystemType)
			{
				case FILESYSTEM_TYPE_FAT32:
					fsName = "fat";
					fileOps = fat_fops;  dirOps = fat_dops;
					break;
				case FILESYSTEM_TYPE_EXT2:
					fsName = "ext2";
					// Ext2 mounts get the WRITABLE pair (secondaries since
					// 2026-08-04's shakedown; the root joined 2026-08-07 when
					// writable-root was ratified). ext2_initialize_filesystem
					// may still strip the write slots if the disk's ro_compat
					// features outrun us — and a DEVICE whose driver cannot
					// write demotes the mount the same way (2026-08-08: the
					// P5's SATA disk, AHCI being read-only today).
					if (kBlockDeviceInfo[idx].block_device->ops->write == NULL)
					{
						fileOps = ext2_fops; dirOps = ext2_dops;
						printf("%s: device's driver cannot write — mounting READ-ONLY\n",
						       vfs_partname_usable(part->partName) ? part->partName : fsName);
					}
					else
					{
						ext2_rw_tables_init();
						fileOps = ext2_rw_fops; dirOps = ext2_rw_dops;
					}
					break;
				default:
					continue;   // unrecognized/no filesystem — not mountable
			}

			const char *baseName = vfs_partname_usable(part->partName)
			                       ? part->partName : fsName;

			if (!vfs_guid_is_ours(part->uniquePartGUID))
			{
				// Somebody else's partition (a Windows ESP, a Linux root,
				// who knows) — acknowledge on the glass, touch NOTHING.
				printf("skipping foreign %s partition (device %u part %u)\n",
				       baseName, idx, partno);
				printd(DEBUG_BOOT, "BOOT: foreign %s partition on device %u part %u — not ours, not mounted\n",
				       baseName, idx, partno);
				continue;
			}

			if (vfs_guid_already_mounted(part->uniquePartGUID))
			{
				printd(DEBUG_BOOT, "BOOT: skipping %s partition %u on device %u — GUID already mounted (twin)\n",
				       baseName, partno, idx);
				continue;
			}

			// First claimant mounts bare ("/fat"); anyone who wants a name
			// that's taken gets a number starting at 2 ("/fat2"), like device
			// names always have. Two partitions CAN carry the same GPT name —
			// nothing stops a disk from having two called "home" — so this has
			// to hold for names exactly as it did for filesystem types.
			char prefix[VFS_MOUNT_PREFIX_MAX];
			sprintf(prefix, "/%s", baseName);
			for (unsigned n = 2; n < 100 && vfs_prefix_in_use(prefix); n++)
				sprintf(prefix, "/%s%u", baseName, n);

			vfs_filesystem_t *fs = kRegisterFilesystem(prefix, &kBlockDeviceInfo[idx],
			                                           partno, &fileOps, &dirOps);
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
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps);
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
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps);
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
// bottom of a successful open and unregister at the top of close; the only
// walker is vfs_sync_all below. This exists because sync(1)'s whole job is
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
	spinlock_acquire(&kOpenFileLock);
	file->openPrev = NULL;
	file->openNext = kOpenFileHead;
	if (kOpenFileHead != NULL)
		kOpenFileHead->openPrev = file;
	kOpenFileHead = file;
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
