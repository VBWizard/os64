// sysfs.c — /sys: the machine as files.
//
// The second synthetic filesystem (procfs was the first; the shared guts —
// text buffer, snapshot handles, mount dance — live in synthfs, extracted
// the day this file was born). Where /proc renders the scheduler's view of
// the world and stays processes-ONLY, Plan 9 style, /sys renders the
// HARDWARE's — starting with what PCI discovery found at boot. The split is
// deliberate lineage: Linux built sysfs for its 2.6 driver model precisely
// because /proc had silted up with kernel state that was never a process;
// os64 gets to draw the line on day one instead of spending a decade
// dredging.
//
// The namespace (grown consumer-first, like everything else):
//
//   /sys/                        five entries: "bus", "cpu", "cache", "gui", "log"
//   /sys/bus/                    one entry: "pci"
//   /sys/bus/pci/                one file per discovered function, named
//                                bus:dev.fn in hex — "00:1f.3", lspci's
//                                spelling (no 0000: domain prefix; os64
//                                enumerates a single PCI segment, and the
//                                filename grows the day the scan does)
//   /sys/bus/pci/00:1f.3         the function, one "key: value" fact per line
//   /sys/cpu/                    the processors: "count", then one directory
//                                per core, named by core number
//   /sys/cpu/count               how many cores the scheduler runs, bare
//   /sys/cpu/<n>/time            the core's CPU-time ledger (was /proc/cores
//                                until 2026-08-12 — a machine fact wearing a
//                                process-tree address; top(1) followed it here)
//   /sys/cpu/<n>/state           the roster bits that can strike a core off
//                                the schedule — READ, side-effect free, no NMI
//   /sys/cpu/<n>/probe           WRITE "probe": fire a diagnostic NMI at the
//                                core.  READ: the last snapshot, rendered.
//
// Every file is TEXT. The PCI values are the enumeration's saved headers —
// kPCIDeviceHeaders / kPCIDeviceFunctions / kPCIBridgeHeaders, written once
// by init_PCI before the scheduler exists and never touched again, which is
// why every PCI read here is lock-free and safe: the snapshot machinery
// copies from arrays that cannot change. The cpu values are the scheduler's
// per-core globals — shared-upper-half, readable from any core under any
// CR3, each written only by its own core (worst case one slice stale).
//
// READS ARE SIDE-EFFECT FREE; THE GUN IS BEHIND A WRITE (ruled 2026-08-11,
// with Linux's own precedent — /sys/power/state suspends on write, cpu*/online
// offlines on write). `cat` must always be safe to aim at anything under
// /sys, because the person most likely to be cat-ing around in here is
// diagnosing a machine that is already half-broken. The single exception a
// purist might count: reading cpu/<n>/time settles the accounting first
// (a rate-limited IPI round /proc/cores always did) — bookkeeping, not
// state anyone can observe changing.
//
// The deliberately-deferred structure (ruled 2026-08-08): no devices/ or
// drivers/ subdirectories, no symlinks, no per-attribute files. One readable
// file per function is the v1; the Linux-shaped tree can grow LATER if a
// customer ever demands it, and the mount router makes that growth cheap.
//
// The raw file is intentionally humble — the pretty-printer is lspci(1),
// which is Chris's to write. That division IS the original one: Martin
// Mareš's lspci started life parsing Linux's raw /proc/bus/pci files and
// making them readable. Kernel emits ugly truth; userland makes it sing.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/filesystem/sys/sysfs.h"
#include "driver/filesystem/vfs/vfs.h"
#include "driver/filesystem/vfs/synthfs.h"
#include "driver/system/pci.h"
#include "driver/system/pci_lookup.h"
#include "driver/system/x86_64.h"   // rdtsc — cpu/<n>/state stamps its own read
#include "kmalloc.h"
#include "memset.h"
#include "strings/strings.h"
#include "sprintf.h"
#include "serial_logging.h"
#include "scheduler.h"              // mp_inScheduler, mp_lastIretqRIP
#include "smp.h"                    // core_local_storage_t, kMPCoreCount
#include "smp_core.h"               // get_core_local_storage_for_core, mpAcctSettleAll
#include "task.h"                   // task_t — kIdleTasks' element type
#include "nmi_probe.h"              // the probe trigger behind cpu/<n>/probe
#include "driver/block/block_cache.h"   // /sys/cache — the block cache's own numbers
#include "gui/compositor.h"             // /sys/gui — kEnableGUI, seat, census
#include "video.h"                      // kFrameBuffer — the resolution it reports
#include "CONFIG.h"
#include "logging/log.h"   // /sys/log — ring stats and sink state
#include "io.h"             // kSerialPresent

extern volatile uint64_t kTicksSinceStart;   // /sys/log stamps the sink heartbeat against it
extern task_t   *kIdleTasks[];         // per-core idle tasks — their runCycles IS idle time
extern uint64_t  kCPUCyclesPerSecond;  // boot-calibrated: the cycles→µs exchange rate
extern volatile bool mp_acctSettleAck[MAX_CPUS];
extern bool mp_schedulerEnabled[MAX_CPUS];

// ── The unified PCI view ────────────────────────────────────────────────────
// init_PCI files what it finds into three arrays: kPCIDeviceHeaders
// (function 0 of each non-bridge device), kPCIDeviceFunctions (the extra
// functions of multifunction devices), and kPCIBridgeHeaders (class 0x06,
// any function — a different struct with bus-routing fields). /sys/bus/pci
// is the UNION of all three; this view is the one place that knows the
// three-array split exists, so nothing else in this file has to.

typedef struct
{
	uint8_t  bus, dev, fn;
	bool     bridge;
	uint16_t vendor, device;
	uint8_t  class, subclass, prog, revision;
	uint16_t subvendor, subdevice;             // devices only (type-0 header)
	uint8_t  irq_line, irq_pin;
	uint8_t  primary, secondary, subordinate;  // bridges only (type-1 header)
} sys_pci_view_t;

static int sys_pci_total(void)
{
	return (int)kPCIDeviceCount + (int)kPCIFunctionCount + (int)kPCIBridgeCount;
}

// Fill the view from flat index 0..total-1 (devices, then functions, then
// bridges — the order is an implementation detail; listing sorts by address).
static bool sys_pci_at(int idx, sys_pci_view_t *v)
{
	memset(v, 0, sizeof(*v));

	if (idx < 0)
		return false;

	if (idx < (int)kPCIDeviceCount || idx < (int)kPCIDeviceCount + (int)kPCIFunctionCount)
	{
		bool is_fn = (idx >= (int)kPCIDeviceCount);
		pci_device_t *d = is_fn ? &kPCIDeviceFunctions[idx - kPCIDeviceCount]
		                        : &kPCIDeviceHeaders[idx];
		v->bus = d->busNo;  v->dev = d->deviceNo;  v->fn = d->funcNo;
		v->bridge = false;
		v->vendor = d->vendor;      v->device = d->device;
		v->class = (uint8_t)d->class;
		v->subclass = (uint8_t)d->subClass;
		// `prog` is the field init_PCI fills; progIF is a dead twin that has
		// already eaten one probe (SUCCESSION.md failure fingerprints).
		v->prog = d->prog;
		v->revision = d->revisionID;
		v->subvendor = d->subvendor;  v->subdevice = d->subdevice;
		v->irq_line = d->interrupt_line;  v->irq_pin = d->interrupt_pin;
		return true;
	}

	idx -= (int)kPCIDeviceCount + (int)kPCIFunctionCount;
	if (idx < (int)kPCIBridgeCount)
	{
		pci_bridge_t *b = &kPCIBridgeHeaders[idx];
		v->bus = b->busNo;  v->dev = b->deviceNo;  v->fn = b->funcNo;
		v->bridge = true;
		v->vendor = b->vendor;      v->device = b->device;
		v->class = (uint8_t)b->class;
		v->subclass = (uint8_t)b->subClass;
		v->prog = b->prog;
		v->revision = b->revisionID;
		v->irq_line = b->interrupt_line;  v->irq_pin = b->interrupt_pin;
		v->primary = b->primaryBusNum;
		v->secondary = b->secondaryBusNum;
		v->subordinate = b->subordinateBusNum;
		return true;
	}
	return false;
}

// bus:dev.fn packed into one sortable integer — the directory cursor's
// currency (same stability trick as /proc's task-ID cursor: strictly
// ascending keys survive any listing order, and dedupe for free).
static uint32_t sys_pci_key(const sys_pci_view_t *v)
{
	return ((uint32_t)v->bus << 8) | ((uint32_t)v->dev << 3) | v->fn;
}

static void sys_pci_name(const sys_pci_view_t *v, char *out, size_t outlen)
{
	snprintf(out, outlen, "%02x:%02x.%x", v->bus, v->dev, v->fn);
}

// Parse "bus:dev.fn" back — strict: exactly the shape sys_pci_name prints
// (lowercase hex, ':' then '.'), because these names are OURS; anything else
// is a file that does not exist.
static bool sys_pci_parse_name(const char *s, uint32_t *key)
{
	uint32_t bus = 0, dev = 0, fn = 0;
	size_t i = 0;

	int hexdigits = 0;
	while (s[i] != '\0' && s[i] != ':')
	{
		int h;
		if (s[i] >= '0' && s[i] <= '9')      h = s[i] - '0';
		else if (s[i] >= 'a' && s[i] <= 'f') h = s[i] - 'a' + 10;
		else return false;
		bus = bus * 16 + (uint32_t)h;
		i++; hexdigits++;
	}
	if (s[i] != ':' || hexdigits == 0 || hexdigits > 2)
		return false;
	i++;

	hexdigits = 0;
	while (s[i] != '\0' && s[i] != '.')
	{
		int h;
		if (s[i] >= '0' && s[i] <= '9')      h = s[i] - '0';
		else if (s[i] >= 'a' && s[i] <= 'f') h = s[i] - 'a' + 10;
		else return false;
		dev = dev * 16 + (uint32_t)h;
		i++; hexdigits++;
	}
	if (s[i] != '.' || hexdigits == 0 || hexdigits > 2)
		return false;
	i++;

	if (s[i] < '0' || s[i] > '7' || s[i + 1] != '\0')
		return false;   // a function is one octal-range digit, then the end
	fn = (uint32_t)(s[i] - '0');

	if (bus > 0xFF || dev > 0x1F)
		return false;
	*key = (bus << 8) | (dev << 3) | fn;
	return true;
}

static bool sys_pci_find(uint32_t key, sys_pci_view_t *v)
{
	for (int i = 0; i < sys_pci_total(); i++)
		if (sys_pci_at(i, v) && sys_pci_key(v) == key)
			return true;
	return false;
}

// ── Path parsing ────────────────────────────────────────────────────────────
// Paths arrive fs-local (mount prefix stripped): "/", "/bus", "/bus/pci",
// "/bus/pci/00:1f.3", "/cpu", "/cpu/count", "/cpu/2", "/cpu/2/probe".

typedef enum
{
	SYS_NODE_INVALID = 0,
	SYS_NODE_ROOT,       // /
	SYS_NODE_BUSDIR,     // /bus
	SYS_NODE_PCIDIR,     // /bus/pci
	SYS_NODE_PCIFILE,    // /bus/pci/<bus:dev.fn>
	SYS_NODE_CPUDIR,     // /cpu
	SYS_NODE_CPUCOUNT,   // /cpu/count
	SYS_NODE_CPUCORE,    // /cpu/<n>
	SYS_NODE_CPUFILE,    // /cpu/<n>/{time,state,probe}
	SYS_NODE_CACHEFILE,  // /cache — the block cache's own numbers (2026-08-16)
	SYS_NODE_LOGFILE,    // /log — the kernel log rings' own numbers (2026-08-18)
	SYS_NODE_GUIFILE,    // /gui — is there a desktop, and what does it cost
} sys_node_type_t;

#define SYS_NAME_MAX 32

// The files a core directory offers, in listing order (most-read first, the
// same doctrine as /proc's file table).
static const char *kSysCpuFiles[] = { "time", "state", "probe" };
#define SYS_CPU_FILE_COUNT (sizeof(kSysCpuFiles) / sizeof(kSysCpuFiles[0]))

typedef struct
{
	sys_node_type_t type;
	uint32_t pci_key;               // PCIFILE: which function
	uint32_t cpu;                   // CPUCORE/CPUFILE: which core
	char     name[SYS_NAME_MAX];    // *FILE: the leaf name as given
} sys_path_t;

static void sys_parse_path(const char *path, sys_path_t *out)
{
	char comp[SYS_NAME_MAX];
	size_t pos = 0;

	memset(out, 0, sizeof(*out));
	out->type = SYS_NODE_INVALID;

	if (path == NULL)
		return;

	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = SYS_NODE_ROOT;
		return;
	}

	if (strcmp(comp, "cpu") == 0)
	{
		if (!synth_next_component(path, &pos, comp, sizeof(comp)))
		{
			out->type = SYS_NODE_CPUDIR;
			return;
		}
		if (strcmp(comp, "count") == 0)
		{
			// Nothing lives inside a file.
			if (synth_next_component(path, &pos, comp, sizeof(comp)))
				return;
			out->type = SYS_NODE_CPUCOUNT;
			return;
		}
		// A core directory is a strict decimal core number, and only for a
		// core that exists — "/cpu/9" on an 8-core machine is a name that
		// does not exist, same as a task ID /proc never issued.
		uint64_t n;
		if (!synth_parse_u64(comp, &n) || n >= kMPCoreCount)
			return;
		out->cpu = (uint32_t)n;
		if (!synth_next_component(path, &pos, comp, sizeof(comp)))
		{
			out->type = SYS_NODE_CPUCORE;
			return;
		}
		if (!synth_name_in(comp, kSysCpuFiles, SYS_CPU_FILE_COUNT))
			return;
		strncpy(out->name, comp, SYS_NAME_MAX - 1);
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_CPUFILE;
		return;
	}

	if (strcmp(comp, "cache") == 0)
	{
		// Nothing lives inside a file.
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_CACHEFILE;
		return;
	}

	if (strcmp(comp, "log") == 0)
	{
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_LOGFILE;
		return;
	}

	if (strcmp(comp, "gui") == 0)
	{
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_GUIFILE;
		return;
	}

	if (strcmp(comp, "bus") != 0)
		return;

	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = SYS_NODE_BUSDIR;
		return;
	}
	if (strcmp(comp, "pci") != 0)
		return;

	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = SYS_NODE_PCIDIR;
		return;
	}
	if (!sys_pci_parse_name(comp, &out->pci_key))
		return;
	strncpy(out->name, comp, SYS_NAME_MAX - 1);
	// Nothing lives inside a file.
	if (synth_next_component(path, &pos, comp, sizeof(comp)))
		return;
	out->type = SYS_NODE_PCIFILE;
}

// ── The file generator ──────────────────────────────────────────────────────
// "key: value", one fact per line — the seam lspci(1) will parse. The hex
// IDs are the ground truth (they answer "what IS this NIC" even when the
// pci.ids module doesn't know it by name); the names are the courtesy.

static void sys_gen_pci_device(synth_text_t *t, const sys_pci_view_t *v)
{
	char name[SYS_NAME_MAX];
	sys_pci_name(v, name, sizeof(name));

	synth_text_addf(t, "address: %s\n", name);
	synth_text_addf(t, "type: %s\n", v->bridge ? "bridge" : "device");
	synth_text_addf(t, "vendor: 0x%04x\n", v->vendor);
	synth_text_addf(t, "device: 0x%04x\n", v->device);
	synth_text_addf(t, "class: 0x%02x\n", v->class);
	synth_text_addf(t, "subclass: 0x%02x\n", v->subclass);
	synth_text_addf(t, "progif: 0x%02x\n", v->prog);
	// The silicon stepping — unfilled (and therefore unreported) until the
	// enumeration learned to save it, the day the inherited bugs got paid.
	synth_text_addf(t, "revision: 0x%02x\n", v->revision);
	if (!v->bridge)
	{
		synth_text_addf(t, "subsystem_vendor: 0x%04x\n", v->subvendor);
		synth_text_addf(t, "subsystem_device: 0x%04x\n", v->subdevice);
	}
	else
	{
		// The routing triplet is what makes a bridge a bridge: it forwards
		// config cycles for buses secondary..subordinate.
		synth_text_addf(t, "primary_bus: 0x%02x\n", v->primary);
		synth_text_addf(t, "secondary_bus: 0x%02x\n", v->secondary);
		synth_text_addf(t, "subordinate_bus: 0x%02x\n", v->subordinate);
	}
	// The legacy interrupt pair straight from the header: pin 0 means "no
	// INTx", line is whatever firmware routed (255 = none/unknown).
	synth_text_addf(t, "irq_line: %u\n", v->irq_line);
	synth_text_addf(t, "irq_pin: %u\n", v->irq_pin);
	// Names last: the parseable facts above never move when a name is long,
	// absent, or grows a comma.
	synth_text_addf(t, "vendor_name: %s\n", pci_get_vendor_by_id(v->vendor));
	synth_text_addf(t, "device_name: %s\n",
	                pci_get_device_by_vendor_device_id(v->vendor, v->device));
}

// ── The cpu generators ──────────────────────────────────────────────────────
// Core numbers here are APIC IDs — the standing house convention
// (kCoreLocalStorage, kIdleTasks and the mp_* arrays are all indexed by it,
// and init_SMP brings cores up 0..kMPCoreCount-1 contiguous). If discontiguous
// APIC IDs ever arrive, every one of those arrays learns together.

// TSC cycles → microseconds, at the read boundary. Raw cycles never leave
// the kernel (the ABI speaks TIME — same doctrine as sleep's milliseconds):
// userland gets µs, and the TSC rate stays a kernel implementation detail.
static uint64_t sys_cycles_to_us(uint64_t cycles)
{
	uint64_t per_us = kCPUCyclesPerSecond / 1000000;
	return per_us ? cycles / per_us : 0;
}

static void sys_gen_cpu_count(synth_text_t *t)
{
	// Bare number, newline — the filename already says what it counts, and
	// a script wants the value, not a parse (Linux's cpu*/online files set
	// the precedent).
	synth_text_addf(t, "%u\n", (unsigned)kMPCoreCount);
}

// /sys/cache — the block cache confesses its size and its effectiveness
// (2026-08-16, born of a morning where 36MB of post-soak "missing" memory had
// to be INFERRED to be cache warm-up; a file beats an inference). Same
// doctrine as os64/memory.h: the interesting ratios are pre-computed, so
// nobody does column arithmetic on a report.
// /sys/log — what the kernel log rings are doing right now.
//
// The customer arrived before the file did (2026-08-18): the rings learned to
// overwrite their oldest entries instead of panicking, and a loss you cannot
// COUNT is a loss you cannot argue about. Then a debugging session spent an
// hour on "is something stuck in the buffers?" that this file answers in one
// `cat` — head, tail, and lost per core, plus whether anyone is draining them.
//
// READS ARE SIDE-EFFECT FREE (the /sys ruling): every value below is a plain
// load of a word another core owns. Worst case a head/tail pair is one entry
// stale, which is the honest amount of stale for a number describing a moving
// ring — and far better than taking the drain lock to read statistics, which
// would let `cat` stall a producer.
static void sys_gen_log(synth_text_t *t)
{
	// WHO IS DRAINING — the question every other number depends on.
	if (kLogSinkClaimed)
		synth_text_addf(t, "sink: userland (last read at tick %lu, now %lu)\n",
		                (uint64_t)kLogSinkLastRead, (uint64_t)kTicksSinceStart);
	else if (!kSerialPresent)
		synth_text_addf(t, "sink: NONE — no serial port, log RETAINED in memory\n");
	else
		synth_text_addf(t, "sink: kernel drainer -> serial\n");

	synth_text_addf(t, "serial: %s\n", kSerialPresent ? "present" : "absent");
	synth_text_addf(t, "format: %s\n", kLogFormat);
	synth_text_addf(t, "ticks_per_second: %d\n", TICKS_PER_SECOND);

	uint64_t used_total = 0, cap_total = 0, lost_total = 0;
	for (int c = 0; c < kMPCoreCount; c++)
	{
		log_buffer_t *b = &core_log_buffers[c];
		if (b->capacity == 0)
			continue;
		size_t head = b->head, tail = b->tail;   // one load each, in this order
		uint64_t used = (head >= tail) ? (head - tail)
		                               : (b->capacity - tail + head);
		used_total += used;
		cap_total  += b->capacity;
		lost_total += b->lost;
		synth_text_addf(t, "core.%d: used %lu of %lu (%lu%%), lost %lu\n",
		                c, used, (uint64_t)b->capacity,
		                b->capacity ? (used * 100) / b->capacity : 0,
		                b->lost);
	}
	synth_text_addf(t, "total: used %lu of %lu entries, lost %lu\n",
	                used_total, cap_total, lost_total);
	// The line that turns "hours and hours?" into a number: entries times the
	// per-entry size is the real memory this subsystem is holding.
	synth_text_addf(t, "bytes: %lu of %lu\n",
	                used_total * (uint64_t)sizeof(log_entry_t),
	                cap_total * (uint64_t)sizeof(log_entry_t));
	if (lost_total > 0)
		synth_text_addf(t, "NOTE: %lu entries were overwritten before anything "
		                   "could drain them\n", lost_total);
}

static void sys_gen_cache(synth_text_t *t)
{
	block_cache_stats_t s;
	block_cache_get_stats(&s);

	synth_text_addf(t, "state: %s\n",
	                kBlockCacheDisabled ? "disabled" : "enabled");

	// WHAT the cache fronts, named — NVMe/SATA only, RAMDisk deliberately
	// skipped (the 8/6 ruling: caching RAM in RAM is rent paid on a thing
	// you own). This line exists because of the day frozen stats sent a
	// perfectly healthy RAMDisk boot on a bug hunt (2026-08-16): a watch
	// loop loading executables off an uncached root leaves no tracks in the
	// numbers below, and the file itself should say why.
	if (block_cache_device_count() == 0)
		synth_text_addf(t, "devices: none (nothing cacheable found)\n");
	for (int i = 0; i < block_cache_device_count(); i++)
		synth_text_addf(t, "device.%d: %s\n", i, block_cache_device_model(i));

	synth_text_addf(t, "bytes: %lu\n", s.bytes_cached);
	synth_text_addf(t, "capacity: %lu\n",
	                (uint64_t)kBlockCacheCapMB * 1024u * 1024u);
	synth_text_addf(t, "hits: %lu\n", s.hits);
	synth_text_addf(t, "misses: %lu\n", s.misses);
	if (s.hits + s.misses > 0)
		synth_text_addf(t, "hit_pct: %lu\n",
		                (s.hits * 100) / (s.hits + s.misses));
	else
		synth_text_addf(t, "hit_pct: 0\n");
	synth_text_addf(t, "fills: %lu\n", s.fills);
	synth_text_addf(t, "evictions: %lu\n", s.evictions);
	synth_text_addf(t, "updates: %lu\n", s.updates);
	synth_text_addf(t, "bypass_edge: %lu\n", s.bypass_edge);
	synth_text_addf(t, "discarded_races: %lu\n", s.discarded_races);
}

// /sys/gui — is there a desktop on this boot, and what is it costing?
//
// Born 2026-08-19 from Chris's question: husk.rc unconditionally started
// gterm, and a text boot answered with a complaint. A startup file wants to
// ASK before it launches — X11 has published that fact in the environment
// since 1987 (DISPLAY), and os64 publishes it as a FILE instead, because a
// file tells the truth at read time while an inherited variable freezes at
// spawn. Today the GUI is a boot-time cmdline flag and cannot change under a
// running system, so both would work; the file is what stays honest if that
// ever stops being true.
//
// `running` comes first and is the whole answer for a script; everything
// below it is for a human with a question. The byte counts are here because
// a window is the one object in os64 that spends memory in TWO worlds — a
// task-mapped canvas and a kernel-side content surface — and /proc can only
// ever show the first (see wm_census_locked's comment for the afternoon that
// taught us).
static void sys_gen_gui(synth_text_t *t)
{
	synth_text_addf(t, "running: %s\n", kEnableGUI ? "yes" : "no");
	if (!kEnableGUI)
	{
		// Say WHY, and say how — a file that reports a "no" without naming
		// the switch that makes it "yes" sends its reader to the source.
		synth_text_addf(t, "reason: no GUI flag on the kernel commandline\n");
		return;
	}

	synth_text_addf(t, "resolution: %ux%u\n",
	                kFrameBuffer.width, kFrameBuffer.height);
	// The two halves of glass ownership, separately, because they fail
	// separately: seated says the compositor holds VT8's seat, owns_glass
	// says VT8 is also the terminal you are looking at. A desktop that is
	// seated but not showing is an Alt+F8 away, and that is a different
	// problem from one that never started.
	synth_text_addf(t, "seated: %s\n", gui_vt8_seated() ? "yes" : "no");
	synth_text_addf(t, "owns_glass: %s\n", gui_owns_glass() ? "yes" : "no");

	uint32_t windows = 0;
	uint64_t surface_bytes = 0;
	gui_census(&windows, &surface_bytes);
	synth_text_addf(t, "windows: %u\n", windows);
	synth_text_addf(t, "window_bytes: %lu\n", surface_bytes);
	// What one more window would cost, so the number above is predictive and
	// not merely historical: both stores are reserved at the screen's size
	// (GRAPHICS.md's capacity reservation), so this scales with resolution.
	synth_text_addf(t, "bytes_per_window: %lu\n",
	                2ull * kFrameBuffer.width * kFrameBuffer.height * 4);
}

// cpu/<n>/time — one core's slice of the CPU-time ledger (/proc/cores' whole
// table until 2026-08-12, split per-core when it moved to the machine's tree).
//
// busy is DERIVED (total - idle - sched): the accounting charges threads,
// the idle thread among them, and the scheduler's own passes; what the
// three don't explain is genuinely unaccounted (early boot, ISR time —
// documented v1 honesty). All values are written only by each core's own
// scheduler pass — reading them cross-core here is safe (worst case one
// slice stale); SUBTRACTING a remote TSC from a local rdtsc would not be,
// which is why total comes from the core's own two stamps, never from
// "now".
static void sys_gen_cpu_time(synth_text_t *t, uint32_t core)
{
	// Settle-on-read: every core charges its in-flight span (locally, own
	// TSC) before we render — so the books are never staler than this IPI
	// round-trip, and tickless mode's lumpy settlement can't staircase a
	// reader. Rate-limited inside (once per tick), so top's sweep across
	// all N core files pays once.
	mpAcctSettleAll();

	core_local_storage_t *cls = get_core_local_storage_for_core(core);
	if (cls == NULL)
		return;

	uint64_t total = (cls->acctLastDispatchTSC > cls->acctZeroTSC)
	                 ? cls->acctLastDispatchTSC - cls->acctZeroTSC : 0;
	uint64_t sched = cls->acctSchedCycles;
	uint64_t idle  = 0;
	if (kIdleTasks[core] != NULL && kIdleTasks[core]->threads != NULL)
		idle = kIdleTasks[core]->threads->runCycles;

	uint64_t accounted = idle + sched;
	uint64_t busy = (total > accounted) ? total - accounted : 0;

	synth_text_addf(t, "total_us: %lu\n", sys_cycles_to_us(total));
	synth_text_addf(t, "busy_us: %lu\n",  sys_cycles_to_us(busy));
	synth_text_addf(t, "idle_us: %lu\n",  sys_cycles_to_us(idle));
	synth_text_addf(t, "sched_us: %lu\n", sys_cycles_to_us(sched));
}

// cpu/<n>/state — the roster bits, raw. This file exists because of a core
// that spent thirteen hours struck off the P5's schedule while every
// instrument we owned needed that core's cooperation to say so (2026-08-11).
// These are plain globals in the shared upper half: reading them cross-core
// is free, always safe, and needs nothing from the core being asked — the
// NMI answers WHY a core is stuck; this file answers WHETHER, for the price
// of a cat.
//
// DELIBERATELY NO SETTLE, no IPI, no side effects of any kind: the machinery
// a settle rides is exactly the machinery this file exists to diagnose, and
// the reader most likely to be here is standing over a half-broken machine.
//
// NO LAPIC FIELDS, and that is physics, not laziness: LAPIC registers are
// core-local MMIO — the same address read here answers about the ASKING
// core (nmi_probe.c documents the rule). The target's LAPIC truth is only
// capturable standing on the target, which is the probe file's job.
static void sys_gen_cpu_state(synth_text_t *t, uint32_t core)
{
	core_local_storage_t *cls = get_core_local_storage_for_core(core);

	synth_text_addf(t, "core: %u\n", core);
	// The three bits that can each strike a core off the roster alone
	// (nmi_probe.h walks through the three disappearances in_scheduler
	// causes). in_scheduler STUCK at 1 was the leading hypothesis for the
	// P5 wedge — this line is that hypothesis testable in four keystrokes.
	synth_text_addf(t, "in_scheduler: %u\n", mp_inScheduler[core] ? 1 : 0);
	synth_text_addf(t, "settle_ack: %u\n", mp_acctSettleAck[core] ? 1 : 0);
	synth_text_addf(t, "scheduler_enabled: %u\n", mp_schedulerEnabled[core] ? 1 : 0);
	synth_text_addf(t, "last_iretq_rip: 0x%016lx\n", mp_lastIretqRIP[core]);
	if (cls != NULL)
	{
		synth_text_addf(t, "current_thread: 0x%016lx\n", (uint64_t)cls->currentThread);
		synth_text_addf(t, "current_task: 0x%016lx\n", (uint64_t)cls->task);
		// The core's own last bookkeeping stamp, raw TSC. Frozen across two
		// reads = the core has stopped keeping books ("parked" in top's
		// vocabulary). Compare successive reads of THIS field, not against
		// the asker's clock — remote-TSC arithmetic is the trap the time
		// generator's comment warns about.
		synth_text_addf(t, "last_settle_tsc: %lu\n", cls->acctLastDispatchTSC);
	}
}

// cpu/<n>/probe (READ) — the last NMI snapshot, rendered through the same
// formatter the wire report uses (nmi_probe_render — one formatter, zero
// drift). Reading never fires anything: no snapshot is an answer too.
static void sys_cpu_probe_line(void *ctx, const char *line)
{
	synth_text_addf((synth_text_t *)ctx, "%s", line);
}

static void sys_gen_cpu_probe(synth_text_t *t, uint32_t core)
{
	const nmi_probe_snapshot_t *s = nmi_probe_last(core);
	if (s == NULL)
	{
		synth_text_addf(t, "no snapshot for core %u — write \"probe\" here to fire a diagnostic NMI\n", core);
		return;
	}
	nmi_probe_render(s, sys_cpu_probe_line, t);
}

// ── File operations ─────────────────────────────────────────────────────────

// The open file object. Everything except the probe trigger is served by the
// bare snapshot; the probe file carries two more fields so a write knows its
// target (same embed-the-head pattern as procfs's ctl handle).
typedef struct
{
	synth_snapshot_t snap;   // MUST be first — the generic fops see only this
	                         // head, and close frees the whole struct by it
	bool     is_probe;       // writes to this handle fire the NMI
	uint32_t core;           // ...at this core
} sys_file_handle_t;

static int sys_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                    vfs_filesystem_t *vfs_fs)
{
	sys_path_t sp;

	sys_parse_path(path, &sp);

	// /sys is read-only EXCEPT cpu/<n>/probe, the one node that is a trigger
	// (reads side-effect free, the gun behind a write — the header's ruling).
	// Rejecting the write modes at the boundary beats a write that silently
	// goes nowhere.
	bool is_probe = (sp.type == SYS_NODE_CPUFILE && strcmp(sp.name, "probe") == 0);
	if (mode == NULL || mode[1] != '\0' || (mode[0] != 'r' && mode[0] != 'w'))
		return -1;
	if (mode[0] == 'w' && !is_probe)
		return -1;

	sys_pci_view_t v;
	if (sp.type == SYS_NODE_PCIFILE)
	{
		if (!sys_pci_find(sp.pci_key, &v))
			return -1;
	}
	else if (sp.type != SYS_NODE_CPUCOUNT && sp.type != SYS_NODE_CPUFILE
	         && sp.type != SYS_NODE_CACHEFILE && sp.type != SYS_NODE_LOGFILE
	         && sp.type != SYS_NODE_GUIFILE)
		return -1;   // directories go through dops; everything else is not a file

	synth_text_t text;
	if (!synth_text_init(&text, 512))
		return -1;

	if (sp.type == SYS_NODE_PCIFILE)
		sys_gen_pci_device(&text, &v);
	else if (sp.type == SYS_NODE_CACHEFILE)
		sys_gen_cache(&text);
	else if (sp.type == SYS_NODE_LOGFILE)
		sys_gen_log(&text);
	else if (sp.type == SYS_NODE_GUIFILE)
		sys_gen_gui(&text);
	else if (sp.type == SYS_NODE_CPUCOUNT)
		sys_gen_cpu_count(&text);
	else if (strcmp(sp.name, "time") == 0)
		sys_gen_cpu_time(&text, sp.cpu);
	else if (strcmp(sp.name, "state") == 0)
		sys_gen_cpu_state(&text, sp.cpu);
	else   // probe — a "w" open still renders the snapshot; harmless to read
		sys_gen_cpu_probe(&text, sp.cpu);

	sys_file_handle_t *h = synth_snapshot_publish(vfs_file, &text, path, vfs_fs,
	                                              sizeof(sys_file_handle_t),
	                                              FILETYPE_SYSFILE);
	if (h == NULL)
		return -1;

	h->is_probe = is_probe;
	h->core     = sp.cpu;
	return 0;
}

// Writing to /sys is a COMMAND aimed at the machine, accepted at exactly one
// node: cpu/<n>/probe. Same contract as /proc's ctl — first whitespace-
// delimited word is the command, the whole write reports as consumed on
// success (a partial write would make `echo` retry the tail, exactly wrong
// for a command). Everywhere else, -1 at the fops layer is louder than a
// write that "succeeds" into nothing.
static int sys_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	sys_file_handle_t *h = (sys_file_handle_t *)vfs_file->handle;
	char word[SYS_NAME_MAX];
	size_t i = 0, n = 0;

	if (h == NULL || !h->is_probe || buffer == NULL || size == 0)
		return -1;

	const char *b = (const char *)buffer;

	while (i < size && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r'))
		i++;
	while (i < size && b[i] != ' ' && b[i] != '\t' && b[i] != '\n' && b[i] != '\r')
	{
		if (n + 1 < sizeof(word))
			word[n++] = b[i];
		i++;
	}
	word[n] = '\0';

	if (n == 0)
		return (int)size;   // whitespace only — consumed, nothing commanded

	// A one-word vocabulary, checked anyway: `echo probe` is a person aiming
	// an NMI on purpose; a stray byte stream landing here by accident is not.
	if (strcmp(word, "probe") != 0)
		return -1;

	// Fire and wait — interrupts enabled, wall-clock bound, all of it
	// nmi_probe_core's own doctrine. False covers both the self-probe
	// refusal and the timeout verdict; each prints its own line, and -1
	// tells the shell the knock got no answer.
	if (!nmi_probe_core(h->core, 10))
		return -1;

	// Crash insurance, the moment the answer lands: the full report goes to
	// the WIRE now (permanent, greppable, survives the machine), and the
	// reader gets the same text from this file at leisure. The glass is
	// spared — whoever fired this is about to cat the answer anyway.
	nmi_probe_report_wire(h->core);
	return (int)size;
}

// ── Directory operations ────────────────────────────────────────────────────

typedef struct
{
	sys_path_t path;
	int      index;      // cursor for the fixed name tables
	uint32_t lastKey;    // cursor for the PCI listing (strictly ascending)
	bool     started;
} sys_dir_handle_t;

static int sys_open_dir(vfs_directory_t **vfs_dir, const char *path,
                        vfs_filesystem_t *vfs_fs)
{
	sys_path_t sp;
	sys_parse_path(path, &sp);

	if (sp.type != SYS_NODE_ROOT && sp.type != SYS_NODE_BUSDIR &&
	    sp.type != SYS_NODE_PCIDIR && sp.type != SYS_NODE_CPUDIR &&
	    sp.type != SYS_NODE_CPUCORE)
		return -1;

	sys_dir_handle_t *h = kmalloc(sizeof(sys_dir_handle_t));
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	if (h == NULL || *vfs_dir == NULL)
	{
		if (h) kfree(h);
		if (*vfs_dir) kfree(*vfs_dir);
		*vfs_dir = NULL;
		return -1;
	}

	h->path = sp;
	h->index = 0;
	h->lastKey = 0;
	h->started = false;

	(*vfs_dir)->handle = h;
	(*vfs_dir)->f_path = (char *)path;   // same lifetime contract as files
	(*vfs_dir)->dops   = vfs_fs->dops;
	(*vfs_dir)->owner  = vfs_fs;
	return 0;
}

static int sys_read_dir(vfs_directory_t *vfs_dir, os64_dirent_t *entry)
{
	sys_dir_handle_t *h = (sys_dir_handle_t *)vfs_dir->handle;

	memset(entry, 0, sizeof(*entry));

	switch (h->path.type)
	{
		case SYS_NODE_ROOT:
		{
			// Two directories, then the root files — a root that lists what
			// a path can reach, nothing hidden (the /proc/self lesson).
			static const char *kSysRootDirs[] = { "bus", "cpu" };
			static const char *kSysRootFiles[] = { "cache", "gui", "log" };
			if (h->index < 2)
			{
				entry->flags = OS64_DE_DIR;
				strncpy(entry->name, kSysRootDirs[h->index], OS64_DIRENT_NAME_MAX);
				h->index++;
				return 1;
			}
			if (h->index < 2 + (int)(sizeof(kSysRootFiles) / sizeof(kSysRootFiles[0])))
			{
				strncpy(entry->name, kSysRootFiles[h->index - 2], OS64_DIRENT_NAME_MAX);
				h->index++;
				return 1;
			}
			return 0;
		}

		case SYS_NODE_CPUDIR:
		{
			// "count" first (the file a script reads before iterating), then
			// one directory per core. index 0 = count, 1..N = core index+1.
			if (h->index == 0)
			{
				h->index = 1;
				strncpy(entry->name, "count", OS64_DIRENT_NAME_MAX);
				return 1;
			}
			int core = h->index - 1;
			if (core < (int)kMPCoreCount)
			{
				h->index++;
				entry->flags = OS64_DE_DIR;
				snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%u", (unsigned)core);
				return 1;
			}
			return 0;
		}

		case SYS_NODE_CPUCORE:
		{
			if (h->index < (int)SYS_CPU_FILE_COUNT)
			{
				strncpy(entry->name, kSysCpuFiles[h->index], OS64_DIRENT_NAME_MAX);
				h->index++;
				return 1;
			}
			return 0;
		}

		case SYS_NODE_BUSDIR:
		{
			if (h->index == 0)
			{
				h->index = 1;
				entry->flags = OS64_DE_DIR;
				strncpy(entry->name, "pci", OS64_DIRENT_NAME_MAX);
				return 1;
			}
			return 0;
		}

		case SYS_NODE_PCIDIR:
		{
			// The lowest address greater than the last one handed out —
			// sorted output and a stable cursor from the same trick, and it
			// dedupes any address the discovery arrays might hold twice.
			sys_pci_view_t v, best;
			bool have = false;
			for (int i = 0; i < sys_pci_total(); i++)
			{
				if (!sys_pci_at(i, &v))
					continue;
				uint32_t key = sys_pci_key(&v);
				if (h->started && key <= h->lastKey)
					continue;
				if (!have || key < sys_pci_key(&best))
				{
					best = v;
					have = true;
				}
			}
			if (!have)
				return 0;   // end of directory

			h->lastKey = sys_pci_key(&best);
			h->started = true;
			// Size 0 for every synthetic file: the content does not exist
			// until something opens it. `ls` prints 0; `cat` reads to EOF.
			sys_pci_name(&best, entry->name, OS64_DIRENT_NAME_MAX);
			return 1;
		}

		default:
			return -1;
	}
}

static int sys_close_dir(vfs_directory_t *vfs_dir)
{
	if (vfs_dir->handle != NULL)
		kfree(vfs_dir->handle);
	kfree(vfs_dir);
	return 0;
}

// stat is readdir for exactly one name (vfs.h): fill the same os64_dirent_t
// for whatever the path names, file or directory.
static int sys_stat(const char *path, os64_dirent_t *entry, vfs_filesystem_t *vfs_fs)
{
	sys_path_t sp;
	(void)vfs_fs;

	sys_parse_path(path, &sp);
	memset(entry, 0, sizeof(*entry));

	switch (sp.type)
	{
		case SYS_NODE_ROOT:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "sys", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_BUSDIR:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "bus", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_PCIDIR:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "pci", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_PCIFILE:
		{
			sys_pci_view_t v;
			if (!sys_pci_find(sp.pci_key, &v))
				return -1;
			strncpy(entry->name, sp.name, OS64_DIRENT_NAME_MAX);
			return 0;
		}

		case SYS_NODE_GUIFILE:
			strncpy(entry->name, "gui", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_CACHEFILE:
			strncpy(entry->name, "cache", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_LOGFILE:
			strncpy(entry->name, "log", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_CPUDIR:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "cpu", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_CPUCOUNT:
			strncpy(entry->name, "count", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_CPUCORE:
			entry->flags = OS64_DE_DIR;
			snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%u", (unsigned)sp.cpu);
			return 0;

		case SYS_NODE_CPUFILE:
			strncpy(entry->name, sp.name, OS64_DIRENT_NAME_MAX);
			return 0;

		default:
			return -1;
	}
}

vfs_file_operations_t sys_fops = {
	.open  = sys_open,
	.read  = synth_snapshot_read,    // the generic snapshot fops (synthfs.h)
	.write = sys_write,
	.seek  = synth_snapshot_seek,
	.tell  = synth_snapshot_tell,
	.close = synth_snapshot_close,
};

vfs_directory_operations_t sys_dops = {
	.open  = sys_open_dir,
	.read  = sys_read_dir,
	.close = sys_close_dir,
	.stat  = sys_stat,
};

// ── Mounting ────────────────────────────────────────────────────────────────

void sysfs_mount(void)
{
	synthfs_mount("/sys", &sys_fops, &sys_dops, "the machine as files");
}
