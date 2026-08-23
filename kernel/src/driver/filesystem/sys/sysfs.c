// sysfs.c — /sys: the machine as files.
//
// The second synthetic filesystem (procfs was the first; the shared guts —
// text buffer, snapshot handles, mount dance — live in synthfs, extracted
// the day this file was born). Where /proc renders the scheduler's view of
// the world and stays processes-ONLY, Plan 9 style, /sys renders MACHINE
// STATE THAT IS NOT A PROCESS — starting with what PCI discovery found at
// boot. The split is deliberate lineage: Linux built sysfs for its 2.6
// driver model precisely because /proc had silted up with kernel state that
// was never a process; os64 gets to draw the line on day one instead of
// spending a decade dredging.
//
// That sentence used to say "renders the HARDWARE's view", and it was true
// until 2026-08-21, when /sys/clipboard arrived and it stopped being true.
// Amended rather than stretched: a clipboard is system state and belongs
// under /sys, but it is not hardware, and the alternative homes were worse.
// /proc is processes only (ruling #7 of SUCCESSION.md). /dev — where Plan 9
// actually kept snarf — is os64's NARROWER Unix-shaped /dev, whose own
// devfs.c says "there is no position, no snapshot, and no buffer: a device
// answers from its nature, not from stored bytes"; the clipboard is stored
// bytes with a length and a position, so filing it there would have made
// that comment a lie on arrival. The day /dev grows into a Plan 9-style
// SERVICE namespace (mouse, draw), snarf's cousins live there and this gets
// revisited. See CLIPBOARD.md.
//
// The namespace (grown consumer-first, like everything else):
//
//   /sys/                        seven entries: "bus", "cpu", "net", "cache",
//                                "gui", "log", "clipboard"
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
//   /sys/net/                    the networking view: "ip", "dhcp", then one
//                                file per REGISTERED NIC, named as the driver
//                                registered it ("r8125_0") — never by index,
//                                which would renumber if probe order changed
//   /sys/net/ip                  the machine's address. MACHINE-wide and it
//                                says so: os64 is single-homed (one
//                                kNetIPv4Address, up to NET_MAX_DEVICES cards)
//   /sys/net/dhcp                how that address was come by — state, lease,
//                                and the conversation's counters
//   /sys/net/<card>              one NIC: model/location (both optional),
//                                mac, mtu, link, whether it carries the
//                                address, and the traffic counters
//   /sys/clipboard               THE system clipboard, read AND write:
//                                `... > /sys/clipboard` copies, `cat` pastes.
//                                Two firsts for /sys, both worth naming: it
//                                is the only node with DURABLE content (every
//                                other file is generated fresh at open), and
//                                the only write that STORES instead of
//                                COMMANDS (cpu/<n>/probe fires a gun; this
//                                keeps your bytes). The standing rule is
//                                untouched — reading it changes nothing.
//                                Store and rulings: clipboard.c, CLIPBOARD.md
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
#include "driver/net/net_device.h"      // /sys/net — kNetDevices, the registered NICs
#include "driver/net/ipv4.h"            // kNetIPv4Address/Gateway/Netmask
#include "driver/net/net_wire.h"        // NET_IPV4_OCTETS — the a.b.c.d splitter
#include "driver/net/dhcp.h"            // kDhcpStats — the lease, and how it was got
#include "gui/compositor.h"             // /sys/gui — kEnableGUI, seat, census
#include "video.h"                      // kFrameBuffer — the resolution it reports
#include "CONFIG.h"
#include "logging/log.h"   // /sys/log — ring stats and sink state
#include "io.h"             // kSerialPresent
#include "clipboard.h"      // /sys/clipboard — the snarf store behind the file
#include "shared_object.h"  // /sys/shlib — the loaded-shared-object registry

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
	SYS_NODE_NETDIR,     // /net — the networking view (2026-08-20)
	SYS_NODE_NETIP,      // /net/ip — the machine's address, and it is ONE
	SYS_NODE_NETDHCP,    // /net/dhcp — how that address was come by
	SYS_NODE_NETCARD,    // /net/<name> — one registered NIC
	SYS_NODE_CLIPFILE,   // /clipboard — the system clipboard (2026-08-21)
	SYS_NODE_SHLIBFILE,  // /shlib — every loaded shared object (2026-08-22)
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
	uint32_t net;                   // NETCARD: index into kNetDevices
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

	if (strcmp(comp, "shlib") == 0)
	{
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_SHLIBFILE;
		return;
	}

	if (strcmp(comp, "clipboard") == 0)
	{
		// A file, and a file it stays: history, when it comes, arrives BESIDE
		// this name and never inside it (CLIPBOARD.md). Consumers must never
		// find a directory where a file used to be.
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = SYS_NODE_CLIPFILE;
		return;
	}

	if (strcmp(comp, "net") == 0)
	{
		if (!synth_next_component(path, &pos, comp, sizeof(comp)))
		{
			out->type = SYS_NODE_NETDIR;
			return;
		}
		bool is_ip   = (strcmp(comp, "ip") == 0);
		bool is_dhcp = (strcmp(comp, "dhcp") == 0);
		if (is_ip || is_dhcp)
		{
			// Decide BEFORE the lookahead: synth_next_component writes into
			// `comp`, so testing it afterwards would be reading the wrong
			// component (or a leftover one).
			if (synth_next_component(path, &pos, comp, sizeof(comp)))
				return;   // nothing lives inside a file
			out->type = is_ip ? SYS_NODE_NETIP : SYS_NODE_NETDHCP;
			return;
		}
		// A card is addressed by the name the DRIVER registered ("r8125_0"),
		// not by an index — the index is an implementation detail of
		// kNetDevices and would renumber if probe order ever changed. Only a
		// name that is actually registered exists, same rule as /cpu/<n>.
		for (uint32_t i = 0; i < NET_MAX_DEVICES; i++)
		{
			if (kNetDevices[i] == NULL || strcmp(kNetDevices[i]->name, comp) != 0)
				continue;
			if (synth_next_component(path, &pos, comp, sizeof(comp)))
				return;
			out->net  = i;
			out->type = SYS_NODE_NETCARD;
			return;
		}
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

// /sys/shlib — every shared object the system has loaded, and what each one
// is actually costing.
//
// Born 2026-08-22, the day libos64 became a .so and the whole userland
// started sharing it. The claim the feature makes is "one copy, shared by
// everybody" — and until this file existed there was no way to CHECK that
// claim on a running machine, only a kernel regression test asserting it
// about a two-task fixture. `resident` is the honest measurement: how many
// pages of this object have actually been read, relocated and cached, which
// is the memory the object costs the whole system no matter how many programs
// are running it. Multiply by nothing; that is the total.
//
// This is also the seam an `ldd`-alike reads (the labor division: the kernel
// grows the view, the utility that presents it is Chris's). Everything a
// dependency lister needs is here — the objects, their dependency edges, and
// where each one landed.
//
// Deliberately NOT a directory of per-object files: the interesting question
// is almost always comparative ("what is loaded, what does it cost"), and a
// single readable table answers it in one `cat`. The clipboard's ruling
// applies in reverse — a file that wants to stay a file.
static void sys_gen_shlib(synth_text_t *t)
{
	synth_text_addf(t, "window: 0x%016lx-0x%016lx\n",
	                (uint64_t)TASK_SHLIB_VIRT_BASE, (uint64_t)TASK_SHLIB_VIRT_END);

	if (kLoadedSharedObjects == NULL || kLoadedSharedObjects->head == NULL)
	{
		// Not an error and worth saying plainly: a machine booted entirely
		// from static binaries is a legitimate state, and an empty table
		// should not read as a broken one.
		synth_text_addf(t, "objects: 0 (nothing dynamically linked has been loaded)\n");
		return;
	}

	uint64_t total_objects = 0, total_resident_pages = 0;
	for (dlist_node_t *n = kLoadedSharedObjects->head; n != NULL; n = n->next)
	{
		shared_object_t *so = (shared_object_t *)n->data;
		if (so == NULL)
			continue;
		total_objects++;

		size_t resident = 0;
		if (so->page_phys != NULL)
			for (size_t i = 0; i < so->total_pages; i++)
			{
				uintptr_t p = so->page_phys[i];
				// Skip the RESOLVING sentinel: a page some core is reading
				// right now is not resident yet, and counting it would make
				// this file's numbers flicker with the fault traffic.
				if (p != 0 && p != SHARED_OBJECT_PAGE_RESOLVING)
					resident++;
			}
		total_resident_pages += resident;

		// One line per object, then its dependency edges indented under it.
		// `kind` distinguishes the two things that live in this registry: a
		// LIBRARY placed in the shared window, and a dynamically-linked
		// EXECUTABLE, which is here for exactly the same reason (its pages
		// are shared between concurrent runs of the same program) but sits
		// at its own link address with no bias at all.
		synth_text_addf(t, "object: %s\n", so->path);
		synth_text_addf(t, "  kind: %s%s\n",
		                so->is_executable ? "executable" : "library",
		                // A PIE executable is both things at once — ET_DYN, so
		                // it takes a slot in the shared window like a library,
		                // while being somebody's program. Worth saying out
		                // loud rather than making the reader infer it from an
		                // address that looks surprising for a program.
		                (so->is_executable && so->load_bias != 0) ? " (position-independent)" : "");
		synth_text_addf(t, "  base: 0x%016lx\n", (uint64_t)(so->load_bias + so->vaddr_base));
		synth_text_addf(t, "  pages: %lu resident of %lu (%lu bytes)\n",
		                (uint64_t)resident, (uint64_t)so->total_pages,
		                (uint64_t)resident * PAGE_SIZE);
		synth_text_addf(t, "  refs: %u\n", so->refcount);
		for (size_t i = 0; i < so->dep_count; i++)
			synth_text_addf(t, "  needs: %s\n",
			                so->deps[i] != NULL ? so->deps[i]->path : "?");
	}

	synth_text_addf(t, "objects: %lu\n", total_objects);
	synth_text_addf(t, "resident_bytes: %lu\n", total_resident_pages * PAGE_SIZE);
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

// ── /sys/net (2026-08-20) ───────────────────────────────────────────────────
//
// Chris asked for "a sys entry for what any currently enabled NIC's network
// settings are", and the interesting part turned out to be the shape rather
// than the data. os64 registers up to NET_MAX_DEVICES cards but has exactly
// ONE address: kNetIPv4Address/Gateway/Netmask are globals and s_dhcp is a
// single client bound to a single device. So a per-card `ip` file would be a
// LIE IN THE FILESYSTEM — four cards each appearing to own an address that
// only one of them carries.
//
// Hence the split. `<card>` holds what is genuinely per-card (identity, link,
// counters); `ip` holds the machine's addressing and says out loud that it is
// machine-wide; `dhcp` holds how that address was come by. The day addressing
// becomes per-interface, `ip` grows a column or moves into the card files and
// nothing else has to change.
//
// THE TWO TRADITIONS, since this had to pick one. Linux splits the same facts
// across /sys/class/net/<if> (link only — MAC, MTU, operstate) and rtnetlink
// (the addresses, reachable by ioctl and `ip addr` but NOT by cat), with DHCP
// nowhere in the kernel at all. That split is chronology, not design:
// rtnetlink predates sysfs, so the addresses never moved. Plan 9 put the whole
// stack in the filesystem — /net/ipifc/<n>/status hands you device, MTU and
// every address in one read. os64 already follows Plan 9 for /proc, and this
// follows it here: everything about the network is readable with cat.
//
// One place per QUESTION, not per object: a NIC also appears under
// /sys/bus/pci, because "what is on the bus" and "what does networking look
// like" are different questions. That is why `location` is printed in lspci's
// spelling — so the two entries can be read against each other. It is
// deliberately NOT a second device tree; the header's 2026-08-08 ruling (no
// devices/, no symlinks) is what keeps os64 out of sysfs's three-views-of-one-
// device maze.

// net/ip — the machine's addressing. Machine-wide, and it says so.
static void sys_gen_net_ip(synth_text_t *t)
{
	synth_text_addf(t, "address: %u.%u.%u.%u\n", NET_IPV4_OCTETS(kNetIPv4Address));
	synth_text_addf(t, "netmask: %u.%u.%u.%u\n", NET_IPV4_OCTETS(kNetIPv4Netmask));
	synth_text_addf(t, "gateway: %u.%u.%u.%u\n", NET_IPV4_OCTETS(kNetIPv4Gateway));

	// Where it came from, in the words the boot line uses. DHCP_BOUND is the
	// only state that means "the network gave us this"; everything else means
	// the cmdline's IP=/GW=/MASK= (or their built-in defaults) are in force.
	synth_text_addf(t, "source: %s\n",
	                kDhcpStats.state == DHCP_BOUND ? "dhcp" : "static");

	// SINGLE-HOMED, stated rather than implied. A reader with two cards
	// installed will otherwise reasonably wonder which one this belongs to —
	// and the honest answer is "the machine", until addressing goes
	// per-interface. Naming the card that carries it costs one line and saves
	// that question.
	synth_text_addf(t, "scope: machine (os64 is single-homed: one address, %u NIC slot(s))\n",
	                (unsigned)NET_MAX_DEVICES);
}

// net/dhcp — how the address above was come by, and whether it is still a
// conversation. Renewal is a booked debt (dhcp.h), so a BOUND lease here is
// held until reboot no matter what lease_seconds says; the file reports the
// number honestly rather than implying a timer that does not exist.
static void sys_gen_net_dhcp(synth_text_t *t)
{
	static const char *kStateNames[] = {
		"IDLE", "SELECTING", "REQUESTING", "BOUND", "GAVE_UP"
	};
	unsigned s = (unsigned)kDhcpStats.state;
	synth_text_addf(t, "state: %s\n",
	                s < (sizeof(kStateNames) / sizeof(kStateNames[0]))
	                    ? kStateNames[s] : "?");

	if (kDhcpStats.state == DHCP_IDLE)
	{
		// Same courtesy /sys/gui pays: a "no" that names the reason instead
		// of sending its reader to the source.
		synth_text_addf(t, "reason: never started (no NIC, or IP= chose static)\n");
	}

	if (kDhcpStats.state == DHCP_BOUND)
	{
		synth_text_addf(t, "lease: %u.%u.%u.%u/%u.%u.%u.%u\n",
		                NET_IPV4_OCTETS(kDhcpStats.lease_ip),
		                NET_IPV4_OCTETS(kDhcpStats.lease_mask));
		synth_text_addf(t, "gateway: %u.%u.%u.%u\n",
		                NET_IPV4_OCTETS(kDhcpStats.lease_gateway));
		// The server is the address to dial on the P5's segment: under ICS
		// the machine sharing its connection IS the DHCP server and the
		// gateway, which is why the lifeboat entry tells you to read it.
		synth_text_addf(t, "server: %u.%u.%u.%u\n",
		                NET_IPV4_OCTETS(kDhcpStats.lease_server));
		// The name server the lease offered (option 6). READ BY SOFTWARE,
		// not just by people: libos64's resolver takes its server from this
		// line when /etc/net.conf names none — so the key and the shape are
		// a contract. "none" when the lease was silent.
		if (kDhcpStats.lease_dns != 0)
			synth_text_addf(t, "dns: %u.%u.%u.%u\n",
			                NET_IPV4_OCTETS(kDhcpStats.lease_dns));
		else
			synth_text_addf(t, "dns: none\n");
		synth_text_addf(t, "lease_seconds: %u (recorded; renewal is a DEBT)\n",
		                kDhcpStats.lease_seconds);
	}

	// The conversation itself, which is what you read when the state is not
	// the one you wanted: sent vs received tells you whether the wire is
	// carrying, and `ignored` is the tell for a second DHCP server or a
	// stale transaction answering late.
	synth_text_addf(t, "discovers_sent: %lu\n", kDhcpStats.discovers_sent);
	synth_text_addf(t, "offers_received: %lu\n", kDhcpStats.offers_received);
	synth_text_addf(t, "requests_sent: %lu\n", kDhcpStats.requests_sent);
	synth_text_addf(t, "acks_received: %lu\n", kDhcpStats.acks_received);
	synth_text_addf(t, "naks_received: %lu\n", kDhcpStats.naks_received);
	synth_text_addf(t, "ignored: %lu\n", kDhcpStats.ignored);
}

// net/<card> — one registered NIC: what it is, whether the wire is good, and
// what has actually moved through it.
static void sys_gen_net_card(synth_text_t *t, uint32_t index)
{
	net_device_t *d = kNetDevices[index];
	if (d == NULL)
		return;

	synth_text_addf(t, "name: %s\n", d->name);
	// Both optional by contract (net_device.h): a device with no part number
	// omits the line rather than inventing one. virtio-net is a contract, not
	// a chip, and saying so by silence is more honest than "unknown".
	if (d->model != NULL)
		synth_text_addf(t, "model: %s\n", d->model);
	if (d->location[0] != '\0')
		synth_text_addf(t, "location: %s\n", d->location);

	synth_text_addf(t, "mac: %02x:%02x:%02x:%02x:%02x:%02x\n",
	                d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
	synth_text_addf(t, "mtu: %u\n", d->mtu);
	// Speed/duplex when the card knows them (link_mbps 0 = unknown: virtio has
	// no wire, and an 8125 at 2.5GbE reports through a bit its driver does not
	// decode). Same silence-over-guessing contract as model and location — and
	// the whole reason those fields moved onto the seam, since until 2026-08-20
	// the e1000 knew its speed and had nowhere to say it.
	if (d->link_up && d->link_mbps != 0)
		synth_text_addf(t, "link: up, %u/%s\n", d->link_mbps,
		                d->full_duplex ? "full" : "half");
	else
		synth_text_addf(t, "link: %s\n", d->link_up ? "up" : "down");

	// Does this card carry the machine's one address? kNetDevices[0] does:
	// syscall dial hands it to tcp/udp/icmp_conn_dial, and kernel_init hands
	// it to dhcp_start — "kNetDevices[0] is the NIC the stack dials", in
	// kernel.c's own words. That makes registration ORDER load-bearing, which
	// is exactly the sort of thing a person deserves to read rather than
	// deduce, especially on a machine with two cards in it.
	synth_text_addf(t, "carries_address: %s\n", index == 0 ? "yes" : "no");

	synth_text_addf(t, "tx_frames: %lu\n", d->tx_frames);
	synth_text_addf(t, "tx_bytes: %lu\n", d->tx_bytes);
	synth_text_addf(t, "tx_errors: %lu\n", d->tx_errors);
	synth_text_addf(t, "rx_frames: %lu\n", d->rx_frames);
	synth_text_addf(t, "rx_bytes: %lu\n", d->rx_bytes);
	synth_text_addf(t, "rx_errors: %lu\n", d->rx_errors);
	// TWO drop reasons, kept apart because they accuse different things:
	// no-handler means frames arrived before the stack claimed RX (a boot
	// ordering story), too-big means the wire carried something longer than
	// NET_FRAME_MAX (counted, never truncated — net_device.h's rule).
	synth_text_addf(t, "rx_dropped_no_handler: %lu\n", d->rx_dropped_no_handler);
	synth_text_addf(t, "rx_dropped_too_big: %lu\n", d->rx_dropped_too_big);
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

// What an open handle IS, when it is not just a rendered snapshot. Everything
// except these two is served by the bare snapshot; the probe file carries its
// target, and the clipboard carries the entry it is reading or the copy it is
// accumulating (same embed-the-head pattern as procfs's ctl handle).
typedef enum
{
	SYS_HANDLE_SNAPSHOT = 0,   // the ordinary case: text generated at open
	SYS_HANDLE_CLIPREAD,       // /sys/clipboard opened "r" — holds a ref
	SYS_HANDLE_CLIPWRITE,      // /sys/clipboard opened "w" — holds a pending
} sys_handle_kind_t;

typedef struct
{
	synth_snapshot_t snap;   // MUST be first — the generic fops see only this
	                         // head, and close frees the whole struct by it
	bool     is_probe;       // writes to this handle fire the NMI
	uint32_t core;           // ...at this core

	// The clipboard's private state. A CLIPREAD handle points snap.data
	// straight AT the entry's immutable bytes — zero copy, even at 16MB — and
	// the generic read/seek/tell then work unmodified, because an immutable
	// ref-held entry is exactly what a snapshot is. Only close differs: it
	// must release the reference instead of kfree'ing the bytes out from
	// under the store (see sys_close).
	sys_handle_kind_t kind;
	snarf_entry_t    *entry;     // CLIPREAD: the entry this handle is reading
	snarf_pending_t  *pending;   // CLIPWRITE: the copy being accumulated
} sys_file_handle_t;

static int sys_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                    vfs_filesystem_t *vfs_fs)
{
	sys_path_t sp;

	sys_parse_path(path, &sp);

	// /sys is read-only EXCEPT two nodes: cpu/<n>/probe, the trigger (reads
	// side-effect free, the gun behind a write — the header's ruling), and
	// clipboard, the one node that STORES what you write. Rejecting the write
	// modes at the boundary beats a write that silently goes nowhere.
	//
	// Mode "a" is REFUSED here along with everything else that is not exactly
	// "r" or "w", and for the clipboard that refusal is deliberate rather than
	// incidental: entries are immutable, and appending to one needs a consumer
	// to justify it (CLIPBOARD.md). The single-character test does the work.
	bool is_probe = (sp.type == SYS_NODE_CPUFILE && strcmp(sp.name, "probe") == 0);
	bool is_clip  = (sp.type == SYS_NODE_CLIPFILE);
	if (mode == NULL || mode[1] != '\0' || (mode[0] != 'r' && mode[0] != 'w'))
		return -1;
	if (mode[0] == 'w' && !is_probe && !is_clip)
		return -1;

	// The clipboard is served from the store, not from generated text, so it
	// leaves the generator ladder below entirely. Both directions publish a
	// handle with NO snapshot buffer of their own: the reader borrows the
	// entry's bytes, the writer has nothing to read yet.
	if (is_clip)
	{
		synth_text_t empty = { 0 };   // no allocation: publish takes it as-is

		snarf_entry_t   *entry   = NULL;
		snarf_pending_t *pending = NULL;

		if (mode[0] == 'w')
		{
			pending = clipboard_begin();
			if (pending == NULL)
				return -1;
		}
		else
		{
			// NULL is not a failure — it is an empty clipboard, which reads
			// as an empty file. `cat /sys/clipboard` before anyone has ever
			// copied prints nothing and exits 0, exactly like an empty file.
			entry = clipboard_acquire();
		}

		sys_file_handle_t *ch = synth_snapshot_publish(vfs_file, &empty, path, vfs_fs,
		                                              sizeof(sys_file_handle_t),
		                                              FILETYPE_SYSFILE);
		if (ch == NULL)
		{
			// Publish failed after we had already taken the store's word for
			// it — hand both back rather than leaking an entry reference.
			//
			// DISCARD, not seal. The open is about to fail, so the caller
			// never gets a handle and never writes a byte; sealing here would
			// publish the empty pending as the newest snarf and throw away
			// whatever the user had copied — an allocation failure inside a
			// failed `open("/sys/clipboard","w")` erasing the clipboard is a
			// bad trade for anyone. (Codex review, 2026-08-22.)
			clipboard_discard(pending);
			clipboard_release(entry);
			return -1;
		}

		ch->kind    = (mode[0] == 'w') ? SYS_HANDLE_CLIPWRITE : SYS_HANDLE_CLIPREAD;
		ch->entry   = entry;
		ch->pending = pending;
		if (entry != NULL)
		{
			// Point the snapshot head at the entry's immutable bytes. Nothing
			// is copied; the reference taken above is what keeps them alive.
			ch->snap.data = (char *)entry->bytes;
			ch->snap.size = entry->length;
		}
		return 0;
	}

	sys_pci_view_t v;
	if (sp.type == SYS_NODE_PCIFILE)
	{
		if (!sys_pci_find(sp.pci_key, &v))
			return -1;
	}
	else if (sp.type != SYS_NODE_CPUCOUNT && sp.type != SYS_NODE_CPUFILE
	         && sp.type != SYS_NODE_CACHEFILE && sp.type != SYS_NODE_LOGFILE
	         && sp.type != SYS_NODE_GUIFILE && sp.type != SYS_NODE_NETIP
	         && sp.type != SYS_NODE_NETDHCP && sp.type != SYS_NODE_NETCARD
	         && sp.type != SYS_NODE_SHLIBFILE)
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
	else if (sp.type == SYS_NODE_SHLIBFILE)
		sys_gen_shlib(&text);
	else if (sp.type == SYS_NODE_NETIP)
		sys_gen_net_ip(&text);
	else if (sp.type == SYS_NODE_NETDHCP)
		sys_gen_net_dhcp(&text);
	else if (sp.type == SYS_NODE_NETCARD)
		sys_gen_net_card(&text, sp.net);
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

// Writing to /sys means one of exactly two things, and they are different in
// kind. cpu/<n>/probe is a COMMAND aimed at the machine — same contract as
// /proc's ctl, first whitespace-delimited word is the verb, the whole write
// reports as consumed on success (a partial write would make `echo` retry the
// tail, exactly wrong for a command). clipboard is CONTENT — every byte is
// kept, and the accumulated copy is sealed at close. Everywhere else, -1 at
// the fops layer is louder than a write that "succeeds" into nothing.
static int sys_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	sys_file_handle_t *h = (sys_file_handle_t *)vfs_file->handle;
	char word[SYS_NAME_MAX];
	size_t i = 0, n = 0;

	if (h == NULL || buffer == NULL)
		return -1;

	if (h->kind == SYS_HANDLE_CLIPWRITE)
	{
		if (size == 0)
			return 0;   // nothing offered, nothing refused
		// A refusal (over the ceiling, or out of memory) has already poisoned
		// the copy and said so on the glass; -1 here is for the tools that do
		// check, and the poison is for the ones that don't.
		if (clipboard_append(h->pending, buffer, size) < 0)
			return -1;
		return (int)size;
	}

	if (!h->is_probe || size == 0)
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

// Close is the ONE file op the clipboard has to have its own version of. Read,
// seek and tell are the generic snapshot ones unchanged, because a ref-held
// immutable entry IS a snapshot — that is the whole reason the entries are
// refcounted instead of copied. But the generic close frees snap.data, and
// those bytes belong to the store, not to this handle.
//
// It is also where a copy becomes visible to everyone else: the seal happens
// HERE, which is what makes "a multi-write copy is one snarf" true.
static int sys_close(vfs_file_t *vfs_file)
{
	sys_file_handle_t *h = (sys_file_handle_t *)vfs_file->handle;

	if (h != NULL)
	{
		if (h->kind == SYS_HANDLE_CLIPWRITE)
		{
			// Publishes, or discards a poisoned copy. Either way the pending
			// is freed and the handle owns nothing afterwards.
			clipboard_seal(h->pending);
			h->pending = NULL;
		}
		else if (h->kind == SYS_HANDLE_CLIPREAD)
		{
			// Disown the borrowed bytes BEFORE the generic close sees them,
			// then drop the reference that was keeping them alive.
			h->snap.data = NULL;
			h->snap.size = 0;
			clipboard_release(h->entry);
			h->entry = NULL;
		}
	}

	return synth_snapshot_close(vfs_file);   // frees the handle and the vfs_file
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
	    sp.type != SYS_NODE_CPUCORE && sp.type != SYS_NODE_NETDIR)
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
			// Directories first, then the root files — a root that lists what
			// a path can reach, nothing hidden (the /proc/self lesson).
			// COUNTED, never a literal: "net" joined this table 2026-08-20
			// and the hardcoded `2` that used to sit in both tests below was
			// how a new entry silently fails to list. (The same class of bug
			// as the "log" entry that went missing from a merge in August —
			// a listing that drops a name reads exactly like a name that
			// does not exist.)
			static const char *kSysRootDirs[] = { "bus", "cpu", "net" };
			static const char *kSysRootFiles[] = { "cache", "gui", "log", "shlib", "clipboard" };
			const int kDirCount  = (int)(sizeof(kSysRootDirs) / sizeof(kSysRootDirs[0]));
			const int kFileCount = (int)(sizeof(kSysRootFiles) / sizeof(kSysRootFiles[0]));
			if (h->index < kDirCount)
			{
				entry->flags = OS64_DE_DIR;
				strncpy(entry->name, kSysRootDirs[h->index], OS64_DIRENT_NAME_MAX);
				h->index++;
				return 1;
			}
			if (h->index < kDirCount + kFileCount)
			{
				const char *name = kSysRootFiles[h->index - kDirCount];
				strncpy(entry->name, name, OS64_DIRENT_NAME_MAX);
				// Size 0 for the generated files — their content does not
				// exist until something opens them. The clipboard is the
				// exception BECAUSE its content is durable: `ls -l /sys` can
				// honestly say how much is on the clipboard right now.
				if (strcmp(name, "clipboard") == 0)
					entry->size = clipboard_length();
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

		case SYS_NODE_NETDIR:
		{
			// The machine's two facts first — a reader asking "what are my
			// network settings" wants `ip` before a card roster — then one
			// file per REGISTERED card, in registration order, which is the
			// order that decides who carries the address.
			static const char *kSysNetFiles[] = { "ip", "dhcp" };
			const int kNetFileCount = (int)(sizeof(kSysNetFiles) / sizeof(kSysNetFiles[0]));
			if (h->index < kNetFileCount)
			{
				strncpy(entry->name, kSysNetFiles[h->index], OS64_DIRENT_NAME_MAX);
				h->index++;
				return 1;
			}
			// Skip empty slots rather than stopping at the first one: the
			// table is a fixed array and nothing promises it is packed.
			for (int i = h->index - kNetFileCount; i < NET_MAX_DEVICES; i++)
			{
				if (kNetDevices[i] == NULL)
					continue;
				h->index = kNetFileCount + i + 1;
				strncpy(entry->name, kNetDevices[i]->name, OS64_DIRENT_NAME_MAX);
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

		case SYS_NODE_SHLIBFILE:
			strncpy(entry->name, "shlib", OS64_DIRENT_NAME_MAX);
			return 0;

		// The clipboard reports its REAL length here (the only /sys node that
		// can), which is how userland sizes a paste before reading one — and
		// how `ls -l /sys` stops lying about the one file that has content.
		case SYS_NODE_CLIPFILE:
			strncpy(entry->name, "clipboard", OS64_DIRENT_NAME_MAX);
			entry->size = clipboard_length();
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

		// The net nodes. MISSING THESE IS WHY `ls /sys/net` answered "could
		// not stat" on its first outing (2026-08-20, found by Chris on VBox):
		// the parser, opendir and readdir all knew about /sys/net, but stat
		// did not, and ls stats a path before it lists it. Four doors, and a
		// node has to be let through EVERY one — the `default: return -1`
		// below is a closed door by design, which is right, and is exactly
		// why a new node type has to be walked through this switch too.
		case SYS_NODE_NETDIR:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "net", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_NETIP:
			strncpy(entry->name, "ip", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_NETDHCP:
			strncpy(entry->name, "dhcp", OS64_DIRENT_NAME_MAX);
			return 0;

		case SYS_NODE_NETCARD:
			// The parser already proved this index names a registered card;
			// re-read it rather than trusting a stale copy of the name.
			if (kNetDevices[sp.net] == NULL)
				return -1;
			strncpy(entry->name, kNetDevices[sp.net]->name, OS64_DIRENT_NAME_MAX);
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
	.close = sys_close,              // snapshot close + the clipboard's seal
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
