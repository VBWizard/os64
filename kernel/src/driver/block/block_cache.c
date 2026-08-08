// block_cache.c — the buffer cache, read half (design brief in block_cache.h).
//
// LOCK DISCIPLINE (the day's thrice-paid lesson, applied from birth): one
// PLAIN spinlock (interrupts stay on) guards the hash + LRU + counters, and
// it is NEVER held across a disk fill — a miss marks nothing, drops the
// lock, reads the line through the driver's original verb, retakes the
// lock, and installs (discarding its work if a racing filler won — the
// no-merging ruling). Hit copies DO run under the lock: 64KB of memcpy is
// microseconds, and holding keeps the line pinned against eviction without
// inventing a refcount. Nothing here may run in interrupt context; the
// block layer never has.

#include <stdint.h>
#include <stdbool.h>
#include "driver/block/block_cache.h"
#include "vfs.h"                 // block_device_info_t, block_operations_t
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "spinlock.h"
#include "serial_logging.h"
#include "BasicRenderer.h"       // printf — the boot line
#include "CONFIG.h"

extern block_device_info_t* kBlockDeviceInfo;
extern int kBlockDeviceInfoCount;

int  kBlockCacheCapMB = 64;      // CACHE=<MB> overrides; 0 = off
bool kBlockCacheDisabled = false;

#define BC_LINE_BYTES   (64u * 1024u)
#define BC_HASH_BUCKETS 1024u    // power of two; ~16 lines/bucket at 1GB

// One interposed device. The registry is tiny and scanned linearly — a
// machine with more than eight disks can grow it the day it exists.
typedef struct {
	block_device_info_t *dev;    // the pointer every caller passes as arg0
	size_t (*orig_read)(void*, uint64_t, void*, uint64_t);
	size_t (*orig_write)(void*, uint64_t, const void*, uint64_t);
	uint32_t line_sectors;       // BC_LINE_BYTES / dev->sectorSize
	uint64_t total_sectors;      // device capacity, or UINT64_MAX when the
	                             // driver never said (NVMe registration
	                             // leaves totalSectorCount zero — the first
	                             // boot bypassed EVERY read on a `> 0` edge
	                             // check; capacity-unknown must mean
	                             // "assume in range, let the fill try")
} bc_device_t;

#define BC_MAX_DEVICES 8
static bc_device_t sDevices[BC_MAX_DEVICES];
static int sDeviceCount = 0;

typedef struct bc_line {
	bc_device_t *owner;          // NULL = this node is free
	uint64_t lineNo;             // sector / owner->line_sectors
	uint8_t *data;               // BC_LINE_BYTES, kmalloc'd once, recycled forever
	struct bc_line *hashNext;
	struct bc_line *lruPrev, *lruNext;
} bc_line_t;

static bc_line_t *sHash[BC_HASH_BUCKETS];
static bc_line_t *sLruHead = NULL;   // most recent
static bc_line_t *sLruTail = NULL;   // eviction candidate
static spinlock_t sLock = 0;
static bool sEnabled = false;
static uint64_t sCapBytes = 0;
static block_cache_stats_t sStats;

static inline uint32_t bc_bucket(bc_device_t *d, uint64_t lineNo)
{
	// Knuth's multiplicative hash over the line number, salted with the
	// device slot so twin disks don't shadow each other bucket-for-bucket.
	uint64_t h = (lineNo * 2654435761u) ^ ((uintptr_t)d >> 4);
	return (uint32_t)(h & (BC_HASH_BUCKETS - 1));
}

// ── LRU + hash surgery (call with sLock held) ───────────────────────────────

static void bc_lru_unlink(bc_line_t *l)
{
	if (l->lruPrev) l->lruPrev->lruNext = l->lruNext; else sLruHead = l->lruNext;
	if (l->lruNext) l->lruNext->lruPrev = l->lruPrev; else sLruTail = l->lruPrev;
	l->lruPrev = l->lruNext = NULL;
}

static void bc_lru_push_front(bc_line_t *l)
{
	l->lruPrev = NULL;
	l->lruNext = sLruHead;
	if (sLruHead) sLruHead->lruPrev = l;
	sLruHead = l;
	if (sLruTail == NULL) sLruTail = l;
}

static void bc_hash_remove(bc_line_t *l)
{
	uint32_t b = bc_bucket(l->owner, l->lineNo);
	bc_line_t **pp = &sHash[b];
	while (*pp && *pp != l)
		pp = &(*pp)->hashNext;
	if (*pp)
		*pp = l->hashNext;
	l->hashNext = NULL;
}

static bc_line_t *bc_lookup(bc_device_t *d, uint64_t lineNo)
{
	for (bc_line_t *l = sHash[bc_bucket(d, lineNo)]; l; l = l->hashNext)
		if (l->owner == d && l->lineNo == lineNo)
			return l;
	return NULL;
}

// Produce an empty line node with a data buffer: allocate fresh while under
// budget, recycle the LRU tail once at it. Returns NULL only when kmalloc
// itself declines (the caller then just reads uncached — never a dead disk).
static bc_line_t *bc_take_node(void)
{
	if (sStats.bytes_cached + BC_LINE_BYTES <= sCapBytes)
	{
		bc_line_t *l = kmalloc(sizeof(bc_line_t));
		if (l == NULL)
			return NULL;
		l->data = kmalloc(BC_LINE_BYTES);
		if (l->data == NULL)
		{
			kfree(l);
			return NULL;
		}
		sStats.bytes_cached += BC_LINE_BYTES;
		l->owner = NULL;
		l->hashNext = NULL;
		l->lruPrev = l->lruNext = NULL;
		return l;
	}

	// At budget: the least-recently-used line donates its body. The
	// allocation count plateaus at the cap and stays there for the uptime —
	// no churn, no fragmentation pressure.
	bc_line_t *victim = sLruTail;
	if (victim == NULL)
		return NULL;
	bc_lru_unlink(victim);
	bc_hash_remove(victim);
	victim->owner = NULL;
	sStats.evictions++;
	return victim;
}

// ── The shims ────────────────────────────────────────────────────────────────

static bc_device_t *bc_find_device(void *dev)
{
	for (int i = 0; i < sDeviceCount; i++)
		if ((void *)sDevices[i].dev == dev)
			return &sDevices[i];
	return NULL;
}

static size_t bc_read_shim(void *dev, uint64_t sector, void *buffer, uint64_t sectorCount)
{
	bc_device_t *d = bc_find_device(dev);
	if (d == NULL || !sEnabled)
	{
		// A caller we don't recognize through an ops struct we interposed
		// shouldn't exist (the arg0 convention is universal — part_table,
		// gpt, both filesystems), but if one ever appears it gets the
		// driver, not a mystery. sDevices[0] is safe as a fallback ONLY
		// because interposition is per-ops-struct and each device has its
		// own; still, prefer the matched record loudly.
		if (sDeviceCount > 0)
			return sDevices[0].orig_read(dev, sector, buffer, sectorCount);
		return (size_t)-1;
	}

	uint8_t *out = (uint8_t *)buffer;
	uint32_t ssz = d->dev->sectorSize;
	uint64_t remaining = sectorCount;
	uint64_t cur = sector;

	while (remaining > 0)
	{
		uint64_t lineNo    = cur / d->line_sectors;
		uint64_t lineStart = lineNo * d->line_sectors;
		uint64_t offInLine = cur - lineStart;
		uint64_t take      = d->line_sectors - offInLine;
		if (take > remaining)
			take = remaining;

		// A line that would run past the device's end is served uncached —
		// the fill would have to invent sectors that don't exist. Rare
		// (the last 64KB of a disk) and honest.
		if (lineStart + d->line_sectors > d->total_sectors)
		{
			sStats.bypass_edge++;
			if (d->orig_read(dev, cur, out, take) != 0)
				return (size_t)-1;
			out += take * ssz;
			cur += take;
			remaining -= take;
			continue;
		}

		spinlock_acquire(&sLock);
		bc_line_t *l = bc_lookup(d, lineNo);
		if (l != NULL)
		{
			// Hit: copy under the lock (pins the line; µs for ≤64KB).
			memcpy(out, l->data + offInLine * ssz, take * ssz);
			bc_lru_unlink(l);
			bc_lru_push_front(l);
			sStats.hits++;
			spinlock_release(&sLock);
		}
		else
		{
			// Miss: take a node under the lock, FILL OUTSIDE IT.
			bc_line_t *mine = bc_take_node();
			sStats.misses++;
			spinlock_release(&sLock);

			if (mine == NULL)
			{
				// No memory for a line — read uncached and move on.
				if (d->orig_read(dev, cur, out, take) != 0)
					return (size_t)-1;
			}
			else
			{
				if (d->orig_read(dev, lineStart, mine->data, d->line_sectors) != 0)
				{
					// The FILL failed — which is not the same as the READ
					// failing: a full-line fetch can stray past a boundary
					// (GPT's backup header lives at the disk's LAST sector)
					// where the caller's exact span is perfectly readable.
					// Give the buffer back and serve the request uncached;
					// the caller learns nothing went wrong, because for
					// their bytes nothing did.
					spinlock_acquire(&sLock);
					sStats.bytes_cached -= BC_LINE_BYTES;
					sStats.bypass_edge++;
					spinlock_release(&sLock);
					kfree(mine->data);
					kfree(mine);
					if (d->orig_read(dev, cur, out, take) != 0)
						return (size_t)-1;
					out += take * ssz;
					cur += take;
					remaining -= take;
					continue;
				}
				sStats.fills++;

				spinlock_acquire(&sLock);
				bc_line_t *racer = bc_lookup(d, lineNo);
				if (racer != NULL)
				{
					// Someone filled it while we read. Their copy is as
					// fresh as ours (no write can have landed between —
					// writes invalidate under this same lock). Serve from
					// theirs, recycle ours to the free pool via LRU tail
					// position (owner NULL keeps it unfindable).
					memcpy(out, racer->data + offInLine * ssz, take * ssz);
					bc_lru_unlink(racer);
					bc_lru_push_front(racer);
					sStats.discarded_races++;
					// Park our orphan at the LRU tail as a ready-to-recycle
					// body: push front then let it age? Simplest: free it.
					sStats.bytes_cached -= BC_LINE_BYTES;
					spinlock_release(&sLock);
					kfree(mine->data);
					kfree(mine);
				}
				else
				{
					mine->owner = d;
					mine->lineNo = lineNo;
					uint32_t b = bc_bucket(d, lineNo);
					mine->hashNext = sHash[b];
					sHash[b] = mine;
					bc_lru_push_front(mine);
					memcpy(out, mine->data + offInLine * ssz, take * ssz);
					spinlock_release(&sLock);
				}
			}
		}

		out += take * ssz;
		cur += take;
		remaining -= take;
	}
	return 0;
}

static size_t bc_write_shim(void *dev, uint64_t sector, const void *buffer, uint64_t sectorCount)
{
	bc_device_t *d = bc_find_device(dev);
	if (d == NULL)
		return sDeviceCount > 0 ? sDevices[0].orig_write(dev, sector, buffer, sectorCount)
		                        : (size_t)-1;

	// Disk FIRST — write-through is the law of the land (and the stray-write
	// tripwire inside the driver keeps its post). Only a write the disk
	// accepted can make cached lines stale.
	size_t rc = d->orig_write(dev, sector, buffer, sectorCount);
	if (rc != 0 || !sEnabled)
		return rc;

	// UPDATE the lines the write touched, in place. This was born as
	// invalidate-only ("a killed line can't lie") and lasted four hours:
	// the first measured ext2 copy showed write-through rewriting the SAME
	// hot metadata blocks (inode table, bitmaps, superblock) every chunk —
	// each write killed a 64KB line, the next read resurrected it whole
	// from disk, and a 1.5MB copy read 1.2GB (the iostat confession,
	// 2026-08-06 late). Copying the written bytes INTO the line is safe
	// for exactly the reason write-through exists: the disk just accepted
	// these bytes, so cache and disk agree by construction.
	uint64_t firstLine = sector / d->line_sectors;
	uint64_t lastLine  = (sector + sectorCount - 1) / d->line_sectors;
	uint32_t ssz = d->dev->sectorSize;
	spinlock_acquire(&sLock);
	for (uint64_t ln = firstLine; ln <= lastLine; ln++)
	{
		bc_line_t *l = bc_lookup(d, ln);
		if (l != NULL)
		{
			// Overlap of the write [sector, sector+count) with this line.
			uint64_t lineStart = ln * d->line_sectors;
			uint64_t from = sector > lineStart ? sector : lineStart;
			uint64_t to   = sector + sectorCount < lineStart + d->line_sectors
			                    ? sector + sectorCount : lineStart + d->line_sectors;
			memcpy(l->data + (from - lineStart) * ssz,
			       (const uint8_t *)buffer + (from - sector) * ssz,
			       (to - from) * ssz);
			bc_lru_unlink(l);
			bc_lru_push_front(l);   // freshly written = obviously hot
			sStats.updates++;
		}
	}
	spinlock_release(&sLock);
	return rc;
}

// ── Init + attach ────────────────────────────────────────────────────────────

void block_cache_init(void)
{
	if (kBlockCacheDisabled || kBlockCacheCapMB <= 0)
	{
		sEnabled = false;
		printf("blockcache: disabled (%s)\n",
		       kBlockCacheDisabled ? "NOCACHE" : "CACHE=0");
		return;
	}
	sCapBytes = (uint64_t)kBlockCacheCapMB * 1024u * 1024u;
	memset(&sStats, 0, sizeof(sStats));
	sEnabled = true;
	printf("blockcache: %d MB budget, 64KB lines (CACHE=<MB> sizes, NOCACHE disables)\n",
	       kBlockCacheCapMB);
}

void block_cache_attach_all(void)
{
	if (!sEnabled)
		return;

	for (int i = 0; i < kBlockDeviceInfoCount && sDeviceCount < BC_MAX_DEVICES; i++)
	{
		block_device_info_t *info = &kBlockDeviceInfo[i];

		// Real disks only. The RAMDisk (memcpy-backed) is skipped on
		// principle: caching RAM in RAM is rent paid on a thing you own.
		if (info->bus != BUS_NVME && info->bus != BUS_SATA)
			continue;
		if (info->block_device == NULL || info->block_device->ops == NULL)
			continue;
		if (info->sectorSize == 0 || (BC_LINE_BYTES % info->sectorSize) != 0)
			continue;   // a sector size that doesn't divide the line — pass

		bc_device_t *d = &sDevices[sDeviceCount++];
		d->dev = info;
		d->orig_read  = info->block_device->ops->read;
		d->orig_write = info->block_device->ops->write;
		d->line_sectors = BC_LINE_BYTES / info->sectorSize;
		d->total_sectors = info->totalSectorCount != 0
		                       ? info->totalSectorCount
		                       : UINT64_MAX;   // unknown ≠ empty (see struct)

		// The interposition: swap the ops struct's FIELDS in place. Every
		// holder of this ops pointer — filesystems mounted past or future,
		// the partition scanner, anyone — now rides through the cache.
		// The write shim only goes in where a write op EXISTS: the AHCI
		// driver is read-only today (2026-08-08, the P5's SATA disk), and
		// shimming its NULL would dress a write-less device as writable —
		// with the first write jumping through orig_write to address zero.
		// A NULL left alone stays the honest NULL-slot refusal.
		info->block_device->ops->read  = bc_read_shim;
		if (d->orig_write != NULL)
			info->block_device->ops->write = bc_write_shim;

		printd(DEBUG_BOOT, "blockcache: attached %s (%u-byte sectors, %u/line)\n",
		       info->ATADeviceModel, info->sectorSize, d->line_sectors);
	}

	printd(DEBUG_BOOT, "blockcache: %d device(s) attached\n", sDeviceCount);
}

void block_cache_get_stats(block_cache_stats_t *out)
{
	spinlock_acquire(&sLock);
	*out = sStats;
	spinlock_release(&sLock);
}

bool block_cache_is_active(void)
{
	return sEnabled && sDeviceCount > 0;
}

bool block_cache_covers(void *block_device_info)
{
	// No lock needed: sDevices only grows, and only during attach — by the
	// time anyone asks, the registry is settled.
	return sEnabled && bc_find_device(block_device_info) != NULL;
}
