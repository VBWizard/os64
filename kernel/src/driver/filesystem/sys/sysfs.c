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
//   /sys/                        one entry: "bus"
//   /sys/bus/                    one entry: "pci"
//   /sys/bus/pci/                one file per discovered function, named
//                                bus:dev.fn in hex — "00:1f.3", lspci's
//                                spelling (no 0000: domain prefix; os64
//                                enumerates a single PCI segment, and the
//                                filename grows the day the scan does)
//   /sys/bus/pci/00:1f.3         the function, one "key: value" fact per line
//
// Every file is TEXT. The values are the enumeration's saved headers —
// kPCIDeviceHeaders / kPCIDeviceFunctions / kPCIBridgeHeaders, written once
// by init_PCI before the scheduler exists and never touched again, which is
// why every read here is lock-free and safe: the snapshot machinery copies
// from arrays that cannot change.
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
#include "kmalloc.h"
#include "memset.h"
#include "strings/strings.h"
#include "sprintf.h"
#include "serial_logging.h"
#include "CONFIG.h"

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
// "/bus/pci/00:1f.3".

typedef enum
{
	SYS_NODE_INVALID = 0,
	SYS_NODE_ROOT,       // /
	SYS_NODE_BUSDIR,     // /bus
	SYS_NODE_PCIDIR,     // /bus/pci
	SYS_NODE_PCIFILE,    // /bus/pci/<bus:dev.fn>
} sys_node_type_t;

#define SYS_NAME_MAX 32

typedef struct
{
	sys_node_type_t type;
	uint32_t pci_key;               // PCIFILE: which function
	char     name[SYS_NAME_MAX];    // PCIFILE: the leaf name as given
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

// ── File operations ─────────────────────────────────────────────────────────

static int sys_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                    vfs_filesystem_t *vfs_fs)
{
	sys_path_t sp;

	// /sys is read-only, whole. Rejecting the write modes at the boundary
	// beats a write that silently goes nowhere.
	if (mode == NULL || mode[0] != 'r' || mode[1] != '\0')
		return -1;

	sys_parse_path(path, &sp);
	if (sp.type != SYS_NODE_PCIFILE)
		return -1;   // directories go through dops; everything else is not a file

	sys_pci_view_t v;
	if (!sys_pci_find(sp.pci_key, &v))
		return -1;

	synth_text_t text;
	if (!synth_text_init(&text, 512))
		return -1;

	sys_gen_pci_device(&text, &v);

	if (synth_snapshot_publish(vfs_file, &text, path, vfs_fs,
	                           sizeof(synth_snapshot_t), FILETYPE_SYSFILE) == NULL)
		return -1;
	return 0;
}

// /sys takes no commands — a write here is always a caller's mistake, and
// -1 at the fops layer is louder than a write that "succeeds" into nothing.
static int sys_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	(void)vfs_file; (void)buffer; (void)size;
	return -1;
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
	    sp.type != SYS_NODE_PCIDIR)
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
			if (h->index == 0)
			{
				h->index = 1;
				entry->flags = OS64_DE_DIR;
				strncpy(entry->name, "bus", OS64_DIRENT_NAME_MAX);
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
