#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

// block_cache — the buffer cache (read caching, phase 1 of the caching arc,
// 2026-08-06).
//
// Thompson's 1975 buffer cache was the first thing Unix grew because disks
// were the first thing that hurt; os64 re-derived the need the same way
// (the grep autopsy: a cold pass over the 46MB log cost ~138,000 NVMe round
// trips, two-thirds of them re-reading the same few hundred ext2 indirect
// blocks). This cache sits where Chris named the pattern: BETWEEN the
// filesystems and the disk drivers — it interposes on each block device's
// block_operations at attach time, so FAT, ext2, GPT scanning, and every
// future caller ride through it without changing a line.
//
// Shape: 64KB cache lines keyed (device, line#), filled with ONE disk
// command each (the run-coalescing fix fused in as the fill mechanism),
// LRU-evicted against a fixed byte budget. Writes are NOT cached (phase 1
// ruling): the shim passes them straight to the driver — write-through
// stands, the stray-write tripwire keeps its post — and then copies the
// written bytes INTO any overlapping resident line, because a stale cache is
// worse than no cache and the disk has just agreed to these exact bytes. (It
// INVALIDATED those lines for the first four hours of its life; see the note
// four paragraphs down for the iostat confession that changed it.)
//
// KEYED BY SECTOR, WHICH IS WHY IT CANNOT SERVE A STALE FILE. A cache line
// names a place on a device, never a name in a directory — so a file replaced
// by rename (a new inode on new blocks) has nothing cached to hit, and blocks
// recycled from a deleted file are correct by the update-in-place rule above.
// The one cache in os64 that IS keyed by name is the shared-object page cache,
// and that is exactly why it needed an identity check (shared_object.h).
//
// PHASE 2, deliberately absent (rulings 2026-08-06): write caching (no
// consumers), memory-pressure eviction + the os64_memory_t `reclaimable`
// seat (counting cache bytes reclaimable before drop-on-demand exists
// would make `available` LIE — cache memory reports as plain `used` until
// eviction-under-pressure is real), async readahead beyond the line
// (needs completion infrastructure), and concurrent-miss merging (two
// tasks missing one line both read disk; the loser's fill is discarded).
// (Update-in-place on write STARTED on this list and was promoted the
// same night — the first measured copy showed invalidate-and-refill
// amplifying reads 800× under write-through's metadata churn.)
//
// Cmdline: CACHE=<MB> sizes the budget (default 64), NOCACHE disables
// entirely — the diagnostic flashlight, same doctrine as SCHED=periodic.

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint64_t hits;            // request bytes served from a line
	uint64_t misses;          // request segments that required a fill
	uint64_t fills;           // lines read from disk (one command each)
	uint64_t evictions;       // LRU lines recycled to make room
	uint64_t updates;         // lines patched in place by an overlapping write
	                          // (was `invalidations` for its first four hours —
	                          // the 1.2GB-to-copy-1.5MB iostat confession
	                          // retired kill-and-refill the same night)
	uint64_t bypass_edge;     // segments served uncached (device-end edge)
	uint64_t discarded_races; // concurrent same-line fills that lost
	uint64_t bytes_cached;    // current resident line bytes
} block_cache_stats_t;

// Parse-time state is read from the cmdline by kernel_commandline.c into
// these; block_cache_init consumes them.
extern int  kBlockCacheCapMB;     // CACHE=<MB>; 0 disables
extern bool kBlockCacheDisabled;  // NOCACHE

// Read the cmdline verdict, size the budget, announce on the glass.
// Call after the cmdline is parsed and kmalloc lives.
void block_cache_init(void);

// Walk kBlockDeviceInfo and interpose the cache's read/write shims on every
// REAL disk's block_operations (NVMe/SATA). The RAMDisk is deliberately
// skipped: caching a memcpy-backed device is paying rent on RAM to store
// copies of RAM. Call after storage drivers have registered; the swap
// mutates each ops struct's fields in place, so filesystems mounted before
// OR after both ride through (they all share the driver's one ops struct).
void block_cache_attach_all(void);

// Live counters, for tests and the curious.
void block_cache_get_stats(block_cache_stats_t *out);

// What the cache actually fronts, for /sys/cache's `device` lines — NVMe and
// SATA only; a RAMDisk is deliberately uncached (caching RAM in RAM is rent
// paid on a thing you own). Exposed so the report can EXPLAIN a quiet cache:
// a watch loop loading executables off an uncached root leaves no tracks
// here, and that must be readable from the file, not deduced from source.
int block_cache_device_count(void);
const char *block_cache_device_model(int i);   // NULL when i is out of range

// True when the cache is enabled AND interposed on at least one device —
// the tests' SKIP question (a NOCACHE boot is a configuration, not a
// failure).
bool block_cache_is_active(void);

// True when the cache is enabled AND interposed on THIS device (pass the
// block_device_info_t* — the universal arg0, typed void* here because the
// struct is vfs.h's and anonymous). Active alone answers the wrong question
// on a RAMDisk-root boot: the machine's internal disk is attached, the root
// the caller reads from is not (first seen: P5, 2026-08-07).
bool block_cache_covers(void *block_device_info);

#endif // BLOCK_CACHE_H
