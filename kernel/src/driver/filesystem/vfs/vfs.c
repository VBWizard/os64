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

	// Claim the prefix in the mount table — this is the moment the filesystem
	// becomes reachable by path.
	if (kMountCount >= VFS_MAX_MOUNTS)
	{
		printd(DEBUG_BOOT, "BOOT: mount table full (%u), %s not mounted\n",
		       VFS_MAX_MOUNTS, mountPoint);
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
};

static bool vfs_guid_is_ours(const uint8_t *guid)
{
	for (size_t i = 0; i < sizeof(kKnownPartGUIDs) / sizeof(kKnownPartGUIDs[0]); i++)
		if (compare_part_uuids(kKnownPartGUIDs[i], (const char *)guid) != NULL)
			return true;
	return false;
}

// Auto-mount every recognized, OS64-AUTHORED non-root partition at
// "/<fstype>". Runs once at boot, after the root is claimed; partition
// tables were already detected by vfs_mount_root_part's first pass.
static void vfs_mount_secondary_partitions(void)
{
	int fatCount = 0, ext2Count = 0;

	for (int idx = 0; idx < kBlockDeviceInfoCount; idx++)
	{
		if (kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_SATA_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_NVME_HD &&
		    kBlockDeviceInfo[idx].ATADeviceType != ATA_DEVICE_TYPE_HD)
			continue;

		for (int partno = 0; partno < kBlockDeviceInfo[idx].block_device->part_count; partno++)
		{
			partEntry_t *part = kBlockDeviceInfo[idx].block_device->partition_table->parts[partno];

			const char *baseName;
			int *counter;
			vfs_file_operations_t fileOps;
			vfs_directory_operations_t dirOps;
			switch (part->filesystemType)
			{
				case FILESYSTEM_TYPE_FAT32:
					baseName = "fat";  counter = &fatCount;
					fileOps = fat_fops;  dirOps = fat_dops;
					break;
				case FILESYSTEM_TYPE_EXT2:
					baseName = "ext2"; counter = &ext2Count;
					fileOps = ext2_fops; dirOps = ext2_dops;
					break;
				default:
					continue;   // unrecognized/no filesystem — not mountable
			}

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

			// First of a type mounts bare ("/fat"); siblings get a number
			// starting at 2 ("/fat2"), like device names always have.
			char prefix[VFS_MOUNT_PREFIX_MAX];
			(*counter)++;
			if (*counter == 1)
				sprintf(prefix, "/%s", baseName);
			else
				sprintf(prefix, "/%s%u", baseName, (unsigned)*counter);

			vfs_filesystem_t *fs = kRegisterFilesystem(prefix, &kBlockDeviceInfo[idx],
			                                           partno, &fileOps, &dirOps);
			if (fs != NULL)
			{
				printd(DEBUG_BOOT, "BOOT: mounted %s (device %u partition %u) at %s\n",
				       baseName, idx, partno, prefix);
				// Screen too (permanent): on real hardware the sweep meets
				// partitions we didn't author — a Windows EFI partition, a
				// recovery blob — and what got mounted WHERE is exactly the
				// context a screen-only machine needs when something fails.
				printf("mounted %s at %s\n", baseName, prefix);
			}
			else
			{
				printf("mount of %s (device %u part %u) FAILED\n", baseName, idx, partno);
				(*counter)--;   // mount failed — give the name back
			}
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
						// partition, read-only for now (the driver is
						// read-only by design — see ext2.c).
						fileOps = ext2_fops;
						dirOps = ext2_dops;
						printd(DEBUG_BOOT, "BOOT: Root filesystem found (ext2), mounting read-only\n");
						kRootFilesystem = kRegisterFilesystem("/", &kBlockDeviceInfo[idx], partno, &fileOps, &dirOps);
						mounted = (kRootFilesystem != NULL);
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