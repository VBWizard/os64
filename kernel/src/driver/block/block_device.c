#include "block_device.h"
#include "kmalloc.h"
#include "panic.h"

dlist_t* kBlockDeviceDList;
int SATAMinor=0, NVMEMinor=0;

// The shared device table every storage driver (AHCI, NVMe, RAMDisk)
// registers into. These historically lived in ahci.c and were allocated by
// init_AHCI(), which meant a noahci boot never got a table at all — any
// other driver registering into it dereferenced NULL. Owned here now.
block_device_info_t* kBlockDeviceInfo = NULL;
int kBlockDeviceInfoCount = 0;

#define MAX_BLOCK_DEVICES 20

void init_block()
{
	// Idempotent: kernel_init() calls this before any storage driver runs,
	// but init_AHCI() also still calls it (its original call site) — the
	// second invocation is a no-op.
	if (kBlockDeviceDList != NULL)
		return;

	kBlockDeviceDList = kmalloc(sizeof(dlist_t));
	if (kBlockDeviceDList == NULL)
	{
		panic("init_block: failed to allocate block device dlist\n");
	}
	dlist_init(kBlockDeviceDList);

	kBlockDeviceInfo = kmalloc(MAX_BLOCK_DEVICES * sizeof(block_device_info_t));
	if (kBlockDeviceInfo == NULL)
	{
		panic("init_block: failed to allocate block device info table\n");
	}
	kBlockDeviceInfoCount = 0;
}

// ── Stray-write tripwire ────────────────────────────────────────────────────
// Born 2026-07-26, from forensics on a corrupted root filesystem: ONE stray
// 1KB write zero-filled the ext2 block holding inode 2 — the root directory —
// on a partition whose driver cannot write BY DESIGN. The pen had to be
// borrowed: a write-capable path (FAT glue, NVMe) aimed at LBAs that were
// never its to touch.
//
// The rule this enforces (upgraded 2026-08-04, the day ext2 learned to
// write): a disk write is legitimate ONLY if it lands entirely inside a
// partition whose MOUNTED filesystem installed a write path. The original
// rule allowlisted FAT partitions by TYPE — correct while ext2 was read-only
// by design, wrong the moment it wasn't. Asking the mount instead keeps
// every old guarantee and adds a new one: a read-only ext2 mount (the root,
// until it's ratified writable) still panics on any write aimed at it, GPT
// headers/tables and unpartitioned space still panic, unmounted partitions
// still panic — and the writable /ext2 secondary passes only because its
// per-mount fops carry a real write op. The backtrace of the caller is worth
// infinitely more than a silently corrupted root filesystem discovered three
// boots later.
//
// Called from every bops->write implementation (NVMe, RAMDisk) BEFORE the
// bytes move. Reads are never gated — this guards the disk, not the reader.
void block_verify_write_allowed(block_device_info_t* device, uint64_t sector, uint64_t count)
{
	if (device == NULL || device->block_device == NULL)
		panic("block write tripwire: write of %lu sector(s) at LBA %lu on a NULL device\n",
		      count, sector);

	struct vfs_partition_table* table = device->block_device->partition_table;
	if (table == NULL)
		panic("block write tripwire: write of %lu sector(s) at LBA %lu before partition scan — nothing legitimate writes that early\n",
		      count, sector);

	// A zero-length write still names a target sector; judge it as one sector
	// so the interval math below can't underflow.
	uint64_t last = sector + (count ? count : 1) - 1;

	for (int i = 0; i < table->partCount; i++)
	{
		partEntry_t* part = table->parts[i];
		if (part == NULL)
			continue;
		// Fully inside this partition? (partEndSector is inclusive — GPT's
		// partLastLBA semantics, see gpt.c.)
		if (sector >= part->partStartSector && last <= part->partEndSector)
		{
			if (vfs_partition_mount_writable(device, i))
				return;   // a mounted filesystem with a write path owns it
			panic("block write tripwire: write of %lu sector(s) at LBA %lu targets partition %d ('%s', fs type %d) which is %s — no code is allowed to write there\n",
			      count, sector, i, part->partName, part->filesystemType,
			      vfs_partition_mounted(device, i) ? "mounted read-only" : "not mounted");
		}
	}

	// Straddling a partition boundary or landing in no partition at all —
	// GPT structures, the gap before the first partition, or past the last.
	panic("block write tripwire: write of %lu sector(s) at LBA %lu lands outside every partition (or straddles one) — misrouted write\n",
	      count, sector);
}

dlist_node_t* add_block_device(vfs_filesystem_t* vfs_block_device)
{
	int major = 0;
	int minor = 0;

	switch (vfs_block_device->block_device_info->bus)
	{
		case BUS_SATA:
			major = 8;
			minor = SATAMinor++;
			break;
		case BUS_NVME:
			major = 259;
			minor = NVMEMinor++;
		break;
		default:
			major = 0;
			minor = 0;
	}
	vfs_block_device->major = major;
	vfs_block_device->minor = minor;

	dlist_node_t* retVal = dlist_add(kBlockDeviceDList, vfs_block_device);
	retVal->major = major;
	retVal->minor = minor;
	return retVal;
}
