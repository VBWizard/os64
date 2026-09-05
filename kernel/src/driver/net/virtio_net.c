// virtio_net.c — the paravirtual NIC: discovery, virtqueues, packet motion.
//
// SLICE HISTORY (NETWORK.md Phase 1):
//   1a: find the device on PCI, modern-capability walk, feature handshake,
//       read the MAC — the OS's first spoken word.
//   1b (this file now): the virtqueue rings. RX buffers posted, TX real,
//       DRIVER_OK earned, frames actually move. Interrupts NOT yet — we
//       poll from processSignals like the USB keyboard does; MSI-X is a
//       future slice (mind the house rule: AP-routed vectors must be ≥0x40).
//
// A one-paragraph virtio primer for the reader who's never met it:
// virtio is a CONTRACT between a hypervisor and a guest, standardized so
// one driver serves QEMU, VirtualBox, cloud hosts, anything. The device
// side is pure software in the hypervisor, so there's no register-poking
// archaeology like real silicon — instead there's a small, rational
// interface: capability structures found via standard PCI capabilities,
// a feature-bit negotiation (both sides say what they speak, the driver
// commits to the intersection), and shared-memory rings for the data.
// It is what hardware interfaces look like when software people design
// them — which is exactly why it's our first NIC and the template for
// reading the less rational ones later.
//
// ── HOW A VIRTQUEUE WORKS (the part worth understanding once, deeply) ──────
// A virtqueue is three arrays in ordinary RAM that both sides can see:
//
//   DESCRIPTOR TABLE  desc[N]  — N slots, each "here is a buffer: physical
//                                address, length, and whether YOU (device)
//                                may write it or only read it."
//   AVAIL RING        avail    — the driver's to-do list FOR the device:
//                                "descriptor #i is ready for you." Driver
//                                writes, device reads.
//   USED RING         used     — the device's done-list back: "finished
//                                with descriptor #i, and (for RX) I wrote
//                                this many bytes." Device writes, driver
//                                reads.
//
// Both rings carry free-running uint16 indices that only ever increment
// (mod 65536); ring slots are index % N. Since N divides 65536, wraparound
// is harmless — comparing "my cursor != their index" is always correct.
// The genius of the shape: NO shared locks, no read-modify-write races —
// each side only ever WRITES its own ring and READS the other's. Producer/
// consumer with the memory bus as the only meeting point. (This is the
// same discipline as the logd per-core queues, played hypervisor-scale.)
//
// TX is "here's a frame, read it, tell me when sent" (device-read buffer).
// RX is pre-payment: we post EMPTY device-writable buffers in advance, and
// arriving packets land in them — the used ring is how we learn which
// buffer filled and how full. Run out of posted RX buffers and the device
// simply drops packets (networks are lossy by design; TCP exists BECAUSE
// links drop things — the stack above will cope, by construction).
//
// Every frame in either direction is prefixed by a 12-byte virtio_net_hdr
// (offload metadata). We negotiated no offload features, so ours is always
// all-zeros on TX and skipped on RX — but it's ALWAYS there; forget it and
// every packet is 12 bytes of garbage followed by a misparse.

#include <stdint.h>
#include <stdbool.h>
#include "paging.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — framebuffer boot lines only
#include "strings/strcpy.h"
#include "memcpy.h"
#include "memset.h"
#include "spinlock.h"
#include "memory/allocator.h"
#include "CONFIG.h"
#include "driver/system/pci.h"
#include "driver/net/net_device.h"
#include "driver/net/virtio_net.h"

extern pci_device_t* kPCIDeviceHeaders;
extern pci_device_t* kPCIDeviceFunctions;
extern uint8_t kPCIDeviceCount, kPCIFunctionCount;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;

// ── PCI identity ────────────────────────────────────────────────────────────
// All virtio devices share one vendor ID. Two device IDs mean "net": 0x1000
// is a TRANSITIONAL device (speaks both the legacy pre-1.0 interface and the
// modern one — QEMU's default), 0x1041 is modern-only (QEMU with
// disable-legacy=on). We speak ONLY the modern interface either way; the
// legacy one (I/O ports, no feature word 1) is a fossil we refuse at the
// door rather than half-support.
#define VIRTIO_PCI_VENDOR        0x1AF4
#define VIRTIO_PCI_DEVICE_TRANS  0x1000
#define VIRTIO_PCI_DEVICE_MODERN 0x1041

// ── Virtio PCI capability structures (virtio spec 4.1.4) ────────────────────
// The modern interface lives inside PCI VENDOR-SPECIFIC capabilities (cap ID
// 0x09). Each one names a region: WHICH BAR, at WHAT OFFSET, HOW LONG, and a
// cfg_type saying what lives there. A device may scatter these across BARs
// however it likes — the driver follows the pointers and never assumes a
// layout (QEMU happens to put everything in one BAR; assuming that would be
// exactly the kind of works-on-QEMU landmine this project keeps a list of).
#define VIRTIO_CAP_COMMON  1   // virtio_pci_common_cfg — the control panel
#define VIRTIO_CAP_NOTIFY  2   // where the driver rings the doorbell (+ multiplier)
#define VIRTIO_CAP_ISR     3   // legacy-interrupt status byte
#define VIRTIO_CAP_DEVICE  4   // device-class config (for net: the MAC lives here)

// Offsets within the common config region (virtio spec 4.1.4.3). These are
// a struct in the spec; spelled as offsets here because we access them
// through a volatile MMIO pointer, and a packed-struct-over-MMIO invites
// the compiler to combine accesses in ways device registers can't tolerate.
#define VC_DEVICE_FEATURE_SEL  0x00   // le32 W: which 32-bit feature word to show
#define VC_DEVICE_FEATURE      0x04   // le32 R: the device's offer, selected word
#define VC_DRIVER_FEATURE_SEL  0x08   // le32 W: which word we're answering
#define VC_DRIVER_FEATURE      0x0C   // le32 W: our acceptance, selected word
#define VC_MSIX_CONFIG         0x10   // le16 W: config-change vector (NO_VECTOR: we poll)
#define VC_NUM_QUEUES          0x12   // le16 R: how many virtqueues exist
#define VC_DEVICE_STATUS       0x14   // u8 RW: the handshake state machine
#define VC_QUEUE_SELECT        0x16   // le16 W: which queue the fields below address
#define VC_QUEUE_SIZE          0x18   // le16 RW: ring size (device max; driver may shrink)
#define VC_QUEUE_MSIX_VECTOR   0x1A   // le16 W: per-queue vector (NO_VECTOR: we poll)
#define VC_QUEUE_ENABLE        0x1C   // le16 W: 1 = this queue is live
#define VC_QUEUE_NOTIFY_OFF    0x1E   // le16 R: doorbell slot (× multiplier = offset)
#define VC_QUEUE_DESC_LO       0x20   // le64 W as two le32: descriptor table phys
#define VC_QUEUE_DESC_HI       0x24
#define VC_QUEUE_DRIVER_LO     0x28   // le64: avail ring phys ("driver area")
#define VC_QUEUE_DRIVER_HI     0x2C
#define VC_QUEUE_DEVICE_LO     0x30   // le64: used ring phys ("device area")
#define VC_QUEUE_DEVICE_HI     0x34

#define VIRTIO_MSI_NO_VECTOR   0xFFFF

// Device-status bits (virtio spec 2.1): the handshake is a LADDER — each bit
// acknowledges one stage, and the order is the protocol. Writing 0 resets.
#define VSTATUS_ACKNOWLEDGE  1     // "a driver noticed you exist"
#define VSTATUS_DRIVER       2     // "and knows how to drive you"
#define VSTATUS_DRIVER_OK    4     // "rings are up — GO"
#define VSTATUS_FEATURES_OK  8     // "feature agreement is final"
#define VSTATUS_FAILED       0x80  // driver gave up (we set on any bail-out)

// Feature bits we care about. Feature space is 64 bits wide, windowed
// through the SEL/FEATURE register pair 32 bits at a time.
#define VNET_F_MAC        (1u << 5)   // word 0: device has a MAC to tell us
#define VIRTIO_F_VERSION_1 (1u << 0)  // word 1 bit 0 = spec bit 32: modern device

// Where the MAC sits in the net device's config region (virtio spec 5.1.4).
#define VNET_CFG_MAC  0x00   // 6 bytes

// ── Virtqueue layout (virtio spec 2.6) ──────────────────────────────────────
#define VQ_RX  0   // "receiveq1"  — virtio-net queue numbering, spec 5.1.2
#define VQ_TX  1   // "transmitq1"

#define VIRTQ_DESC_F_WRITE  2   // device may WRITE this buffer (RX); absent = read-only (TX)

typedef struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } virtq_desc_t;      // 16 bytes
typedef struct { uint16_t flags; uint16_t idx; uint16_t ring[]; } virtq_avail_t;
typedef struct { uint32_t id;    uint32_t len; } virtq_used_elem_t;
typedef struct { uint16_t flags; uint16_t idx; virtq_used_elem_t ring[]; } virtq_used_t;

// Ring size: the device offers up to some power of two (QEMU: 256); the
// driver may shrink it, and we do. 64 in-flight frames per direction is
// plenty for an OS whose entire network load is one test suite — and it
// caps the slab math below at 128KB per queue. Grows when a workload says so.
#define VQ_SIZE_CAP   64
// Per-frame buffer: 12-byte virtio header + NET_FRAME_MAX(1536) = 1548,
// rounded up so every slot starts cache-line- and sub-page-tidy.
#define VNET_HDR_LEN  12
#define VQ_BUF_SIZE   2048

// x86 is TSO (total store order): coherent DMA sees our stores in program
// order, so the only fence needed is against the COMPILER migrating ring
// writes past the index publish. On a weaker architecture this macro grows
// real teeth — it exists to mark every spot that would need them.
#define wmb() __asm__ volatile("" ::: "memory")

// One live virtqueue.
typedef struct
{
	uint16_t size;
	volatile virtq_desc_t*  desc;
	volatile virtq_avail_t* avail;
	volatile virtq_used_t*  used;
	volatile uint16_t* notify;      // this queue's doorbell register
	uint16_t last_used;             // our cursor chasing used->idx (free-running)
	uint8_t* buf;                   // buffer slab, HHDM view (slot i at i*VQ_BUF_SIZE)
	uint64_t buf_phys;              //   ...and its physical base (what descriptors carry)
	// TX bookkeeping: which descriptor/buffer slots are free. A stack, not
	// round-robin, because the spec does NOT promise in-order completion —
	// used-ring entries return IDs, and the stack honors whatever order
	// they come back in. (QEMU happens to complete in order. "Happens to"
	// is not a contract.)
	uint16_t free_ids[VQ_SIZE_CAP];
	uint16_t free_top;
} vq_t;

// ── Driver state ────────────────────────────────────────────────────────────
// ONE static instance: v1 drives the first virtio-net found, period. The
// day QEMU grows a second NIC in our tests, this becomes a small array —
// the xHCI driver walked the same road (single controller until the P5's
// five made it plural) and the net_device seam is already plural-ready.
typedef struct
{
	pci_device_t* pci;
	volatile uint8_t* common;      // control panel (VC_* offsets above)
	volatile uint8_t* device_cfg;  // net-class config (MAC)
	volatile uint8_t* notify_base; // doorbell region base
	uint32_t notify_off_multiplier;
	vq_t rx, tx;
	bool driver_ok;                // DRIVER_OK written — rings live, polling meaningful
	spinlock_t lock;               // guards both rings' driver-side state
	net_device_t netdev;
} virtio_net_t;

static virtio_net_t s_vnet;

// ── MMIO accessors ──────────────────────────────────────────────────────────
// volatile at the single choke point, so no call site can forget it. x86-64
// is little-endian and virtio-modern is little-endian by decree (that's what
// VERSION_1 *means*, wire-format-wise), so no swapping — noted because the
// protocol stack above WILL meet big-endian fields (network byte order) and
// the difference deserves to be conscious, not coincidental.
static inline uint8_t  vread8(volatile uint8_t* base, uint32_t off)  { return *(volatile uint8_t*)(base + off); }
static inline uint16_t vread16(volatile uint8_t* base, uint32_t off) { return *(volatile uint16_t*)(base + off); }
static inline uint32_t vread32(volatile uint8_t* base, uint32_t off) { return *(volatile uint32_t*)(base + off); }
static inline void vwrite8(volatile uint8_t* base, uint32_t off, uint8_t v)   { *(volatile uint8_t*)(base + off) = v; }
static inline void vwrite16(volatile uint8_t* base, uint32_t off, uint16_t v) { *(volatile uint16_t*)(base + off) = v; }
static inline void vwrite32(volatile uint8_t* base, uint32_t off, uint32_t v) { *(volatile uint32_t*)(base + off) = v; }

// ── BAR mapping ─────────────────────────────────────────────────────────────
// Resolve BAR n to a physical base, honoring the 64-bit encoding (bits 2:1
// == 10b → the high half lives in BAR n+1) and refusing I/O-space BARs
// (bit 0) — the modern capabilities only ever point at memory BARs, so an
// I/O BAR here means we misread something and should say so, not poke ports.
static uint64_t virtio_bar_phys(pci_device_t* dev, uint8_t bar)
{
	if (bar >= 6)
		return 0;
	uint32_t lo = dev->baseAdd[bar];
	if (lo & 0x1)   // I/O space
		return 0;
	uint64_t phys = lo & ~0xFULL;
	if ((lo & 0x6) == 0x4 && bar < 5)
		phys |= ((uint64_t)dev->baseAdd[bar + 1]) << 32;
	return phys;
}

// Map [phys+offset, phys+offset+length) at the kernel's HHDM alias and
// return the virtual pointer. Same idiom as the xHCI BAR map: upper-half VA
// so it's visible under EVERY task's CR3 (the poll runs from processSignals
// under whoever's page tables are live), and PAGE_PCD because device
// registers must never be cached — a cached read of a ring index would
// return stale truth, which in a producer/consumer protocol is
// indistinguishable from a lie.
static volatile uint8_t* virtio_map_region(uint64_t bar_phys, uint32_t offset, uint32_t length)
{
	uint64_t first_page = (bar_phys + offset) & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t last_byte  = bar_phys + offset + length - 1;
	uint64_t page_count = ((last_byte - first_page) / PAGE_SIZE) + 1;

	// Re-mapping a page that an earlier capability's region already mapped
	// just rewrites the same PTE with the same value — harmless, and far
	// simpler than tracking which of a BAR's pages we've visited.
	paging_map_pages((pt_entry_t*)kKernelPML4v, kHHDMOffset + first_page,
	                 first_page, page_count,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	return (volatile uint8_t*)(kHHDMOffset + bar_phys + offset);
}

// ── Transmit ────────────────────────────────────────────────────────────────
static int32_t virtio_net_transmit(net_device_t* dev, const void* frame, uint16_t length)
{
	virtio_net_t* vn = (virtio_net_t*)dev->driver_data;

	if (!vn->driver_ok || length == 0 || length > NET_FRAME_MAX)
	{
		dev->tx_errors++;
		return -1;
	}

	uint64_t irqflags = spinlock_acquire_irqsave(&vn->lock);
	vq_t* tx = &vn->tx;

	if (tx->free_top == 0)
	{
		// Ring full. The bound IS the flow control (the pipe doctrine,
		// wire edition): refuse loudly, let the caller retry. An
		// unbounded TX queue is a memory leak with a modem attached.
		spinlock_release_irqrestore(&vn->lock, irqflags);
		dev->tx_errors++;
		return -2;
	}

	uint16_t id = tx->free_ids[--tx->free_top];
	uint8_t* slot = tx->buf + (uint32_t)id * VQ_BUF_SIZE;

	// The 12-byte virtio header: all-zeros means "no offloads, no GSO,
	// just send it" — exactly the contract our declined features left us.
	// Zeroed EVERY time because slots are reused and yesterday's header
	// must not haunt today's frame.
	memset(slot, 0, VNET_HDR_LEN);
	memcpy(slot + VNET_HDR_LEN, frame, length);
	tx->desc[id].len = VNET_HDR_LEN + length;

	// Publish: slot into the avail ring, THEN the index. The wmb()s pin
	// the order — the device must never see the new idx before the entry
	// it points at is real.
	tx->avail->ring[tx->avail->idx % tx->size] = id;
	wmb();
	tx->avail->idx++;
	wmb();
	*tx->notify = VQ_TX;   // the doorbell: "queue 1 has work"

	dev->tx_frames++;
	dev->tx_bytes += length;
	spinlock_release_irqrestore(&vn->lock, irqflags);
	return 0;
}

static net_operations_t s_vnet_ops = {
	.transmit = virtio_net_transmit,
	.drain    = virtio_net_drain,
};

// ── Drain (the seam's verb; knet calls it, DOORBELL.md) ─────────────────────
// Drains both used rings: TX completions return their slots to the free
// stack; RX arrivals are delivered through the seam's choke point and the
// buffer goes straight back on the avail ring — recycled, never reallocated.
// Returns whether anything moved, so the drainer knows to come around again.
// virtio has no interrupt wired (DEBTS: its conversion is MSI-X work), so the
// tick's ring of knet's bell is the only thing that brings it here — the
// cadence it always had, out from under the scheduler lock.
bool virtio_net_drain(struct net_device* dev)
{
	(void)dev;   // one virtio-net per machine; the seam's handle and s_vnet are the same card
	virtio_net_t* vn = &s_vnet;
	if (!vn->driver_ok)
		return false;

	// Cheap reentry guard (the xhci_poll idiom): if another core is mid-
	// drain, skip — frames aren't lost, the next drain collects them.
	static volatile uint32_t busy = 0;
	if (__sync_lock_test_and_set(&busy, 1))
		return false;
	bool moved = false;

	uint64_t irqflags = spinlock_acquire_irqsave(&vn->lock);

	vq_t* tx = &vn->tx;
	while (tx->last_used != tx->used->idx)
	{
		volatile virtq_used_elem_t* e = &tx->used->ring[tx->last_used % tx->size];
		tx->free_ids[tx->free_top++] = (uint16_t)e->id;
		tx->last_used++;
		moved = true;
	}
	spinlock_release_irqrestore(&vn->lock, irqflags);

	// RX drain: one frame per lock hold, DELIVERED WITH THE LOCK RELEASED.
	// That shape is load-bearing, not stylistic. The protocol stack's RX
	// handler TRANSMITS in direct response to arrivals — an ARP request in
	// begets an ARP reply out, an echo request begets an echo reply — and
	// virtio_net_transmit takes this same lock; holding it across delivery
	// would deadlock the machine on its first answered ping. So each frame
	// is staged into a local buffer, its ring buffer recycled, the lock
	// dropped, and only THEN does the stack see it. The busy flag above
	// already guarantees a single drainer, so cycling the lock mid-drain
	// races nothing; the staging copy costs ≤1536 bytes of kernel stack
	// (20-page stacks — fine) and one memcpy per frame (v1-honest; the
	// zero-copy upgrade waits for a measured need, per the seam's charter).
	vq_t* rx = &vn->rx;
	uint8_t staged[NET_FRAME_MAX];
	for (;;)
	{
		irqflags = spinlock_acquire_irqsave(&vn->lock);
		if (rx->last_used == rx->used->idx)
		{
			spinlock_release_irqrestore(&vn->lock, irqflags);
			break;
		}
		volatile virtq_used_elem_t* e = &rx->used->ring[rx->last_used % rx->size];
		uint16_t id  = (uint16_t)e->id;
		uint32_t len = e->len;

		// Stage the frame MINUS the virtio header. A runt shorter than the
		// header itself is device nonsense; a frame too big for the staging
		// buffer is counted, never truncated (the counter lives here rather
		// than in net_device_rx because a frame we can't stage is a frame
		// the seam never gets to see).
		uint16_t flen = 0;
		if (len > VNET_HDR_LEN && (len - VNET_HDR_LEN) <= NET_FRAME_MAX)
		{
			flen = (uint16_t)(len - VNET_HDR_LEN);
			memcpy(staged, rx->buf + (uint32_t)id * VQ_BUF_SIZE + VNET_HDR_LEN, flen);
		}
		else if (len > VNET_HDR_LEN)
			vn->netdev.rx_dropped_too_big++;

		// Recycle: the same buffer goes straight back to the device. The
		// doorbell rings per-frame now — it's one MMIO write, and batching
		// it stopped being worth anything once the lock started cycling.
		rx->avail->ring[rx->avail->idx % rx->size] = id;
		wmb();
		rx->avail->idx++;
		rx->last_used++;
		spinlock_release_irqrestore(&vn->lock, irqflags);
		*rx->notify = VQ_RX;

		moved = true;   // a buffer came back, delivered or counted
		if (flen)
			net_device_rx(&vn->netdev, staged, flen);
	}

	__sync_lock_release(&busy);
	return moved;
}

// ── Virtqueue construction ──────────────────────────────────────────────────
static bool virtio_setup_queue(uint16_t qindex, vq_t* vq, bool is_rx)
{
	vwrite16(s_vnet.common, VC_QUEUE_SELECT, qindex);

	uint16_t dev_max = vread16(s_vnet.common, VC_QUEUE_SIZE);
	if (dev_max == 0)
	{
		printd(DEBUG_NET, "virtio-net: queue %u does not exist\n", qindex);
		return false;
	}
	// The driver may shrink the ring (spec 4.1.4.3.2) — we cap at our
	// slab budget. Both values are powers of two, so min() preserves that.
	uint16_t size = (dev_max < VQ_SIZE_CAP) ? dev_max : VQ_SIZE_CAP;
	vwrite16(s_vnet.common, VC_QUEUE_SIZE, size);
	vq->size = size;

	// One allocation for all three arrays. Spec alignment: desc 16, avail
	// 2, used 4 — a page-aligned start (allocate_memory_aligned) satisfies
	// desc, and the offsets below keep the other two. The allocator zeroes
	// every allocation at the choke point (house doctrine), and all-zero
	// happens to be EXACTLY a virtqueue's reset state (idx 0, flags 0) —
	// no init pass needed, and that's by design, not luck.
	uint32_t desc_bytes  = 16u * size;
	uint32_t avail_bytes = 6u + 2u * size;
	uint32_t used_off    = (desc_bytes + avail_bytes + 3u) & ~3u;
	uint32_t total_bytes = used_off + 6u + 8u * size;

	uint64_t ring_phys = allocate_memory_aligned(total_bytes);
	if (ring_phys == 0)
		return false;
	vq->desc  = (volatile virtq_desc_t*) (ring_phys | kHHDMOffset);
	vq->avail = (volatile virtq_avail_t*)((ring_phys + desc_bytes) | kHHDMOffset);
	vq->used  = (volatile virtq_used_t*) ((ring_phys + used_off)   | kHHDMOffset);

	// Tell the device where the rings live — physical addresses; the
	// device DMAs, it doesn't page-walk. 64-bit fields written as two
	// le32 halves (spec 4.1.3.1's access-width rules for common config).
	vwrite32(s_vnet.common, VC_QUEUE_DESC_LO,   (uint32_t)(ring_phys & 0xFFFFFFFF));
	vwrite32(s_vnet.common, VC_QUEUE_DESC_HI,   (uint32_t)(ring_phys >> 32));
	vwrite32(s_vnet.common, VC_QUEUE_DRIVER_LO, (uint32_t)(((ring_phys + desc_bytes)) & 0xFFFFFFFF));
	vwrite32(s_vnet.common, VC_QUEUE_DRIVER_HI, (uint32_t)((ring_phys + desc_bytes) >> 32));
	vwrite32(s_vnet.common, VC_QUEUE_DEVICE_LO, (uint32_t)(((ring_phys + used_off)) & 0xFFFFFFFF));
	vwrite32(s_vnet.common, VC_QUEUE_DEVICE_HI, (uint32_t)((ring_phys + used_off) >> 32));

	// We poll; no vector. Explicit NO_VECTOR so the intent is written
	// down, not defaulted into.
	vwrite16(s_vnet.common, VC_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

	// This queue's doorbell: a slot number times the multiplier gives the
	// offset into the notify region. (The indirection lets hardware
	// implementations give every queue its own page; QEMU collapses it.)
	uint16_t notify_off = vread16(s_vnet.common, VC_QUEUE_NOTIFY_OFF);
	vq->notify = (volatile uint16_t*)(s_vnet.notify_base +
	                                  (uint64_t)notify_off * s_vnet.notify_off_multiplier);

	// The buffer slab: one contiguous physical run, slot i at i*VQ_BUF_SIZE.
	vq->buf_phys = allocate_memory_aligned((uint64_t)size * VQ_BUF_SIZE);
	if (vq->buf_phys == 0)
		return false;
	vq->buf = (uint8_t*)(vq->buf_phys | kHHDMOffset);

	if (is_rx)
	{
		// RX pre-payment: every descriptor is a standing empty buffer the
		// device may fill (F_WRITE), and ALL of them go on the avail ring
		// before the device ever runs. From the device's view, we open
		// for business already holding out our hands.
		for (uint16_t i = 0; i < size; i++)
		{
			vq->desc[i].addr  = vq->buf_phys + (uint32_t)i * VQ_BUF_SIZE;
			vq->desc[i].len   = VQ_BUF_SIZE;
			vq->desc[i].flags = VIRTQ_DESC_F_WRITE;
			vq->desc[i].next  = 0;
			vq->avail->ring[i] = i;
		}
		wmb();
		vq->avail->idx = size;
	}
	else
	{
		// TX: descriptors pre-point at their slots (addr never changes —
		// only len does, per frame); every slot starts on the free stack.
		for (uint16_t i = 0; i < size; i++)
		{
			vq->desc[i].addr  = vq->buf_phys + (uint32_t)i * VQ_BUF_SIZE;
			vq->desc[i].len   = 0;
			vq->desc[i].flags = 0;   // device READS these
			vq->desc[i].next  = 0;
			vq->free_ids[i] = i;
		}
		vq->free_top = size;
	}

	vwrite16(s_vnet.common, VC_QUEUE_ENABLE, 1);
	printd(DEBUG_NET, "virtio-net: queue %u up, size %u (device offered %u)\n",
	       qindex, size, dev_max);
	return true;
}

// ── Capability walk ─────────────────────────────────────────────────────────
// Standard PCI capability list: config offset 0x34 holds the first pointer,
// each capability holds the next. We collect the virtio regions we need.
// Config space is read in 32-bit dwords (ECAM helper semantics), so fields
// are extracted by shifting — offsets in the virtio cap: +0 cap ID, +1 next,
// +3 cfg_type, +4 bar index, +8 region offset, +12 region length, and for
// the NOTIFY cap only, +16 the doorbell stride multiplier.
static bool virtio_discover_regions(pci_device_t* dev)
{
	bool have_common = false, have_device = false, have_notify = false;

	uint32_t dw = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 0x34);
	uint8_t cap_off = dw & 0xFC;

	while (cap_off != 0)
	{
		uint32_t cap_dw0 = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, cap_off);
		uint8_t cap_id   = cap_dw0 & 0xFF;
		uint8_t cap_next = (cap_dw0 >> 8) & 0xFC;

		if (cap_id == 0x09)   // vendor-specific = virtio's namespace
		{
			uint8_t cfg_type = (cap_dw0 >> 24) & 0xFF;
			uint8_t bar      = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, cap_off + 4) & 0xFF;
			uint32_t offset  = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, cap_off + 8);
			uint32_t length  = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, cap_off + 12);
			uint64_t bar_phys = virtio_bar_phys(dev, bar);

			printd(DEBUG_NET, "virtio-net: cap type %u -> BAR %u +0x%x len 0x%x (phys 0x%lx)\n",
			       cfg_type, bar, offset, length, bar_phys);

			if (bar_phys == 0)
			{
				cap_off = cap_next;
				continue;   // I/O BAR or unset — legacy furniture, skip
			}

			switch (cfg_type)
			{
				case VIRTIO_CAP_COMMON:
					s_vnet.common = virtio_map_region(bar_phys, offset, length);
					have_common = true;
					break;
				case VIRTIO_CAP_DEVICE:
					s_vnet.device_cfg = virtio_map_region(bar_phys, offset, length);
					have_device = true;
					break;
				case VIRTIO_CAP_NOTIFY:
					s_vnet.notify_base = virtio_map_region(bar_phys, offset, length);
					s_vnet.notify_off_multiplier =
						readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, cap_off + 16);
					have_notify = true;
					break;
				default:
					break;   // ISR (interrupt-era; we poll) and PCI_CFG (a
					         // keyhole for before BARs are mapped) — not needed
			}
		}
		cap_off = cap_next;
	}

	return have_common && have_device && have_notify;
}

// ── Bring-up ────────────────────────────────────────────────────────────────
static bool virtio_net_init_device(pci_device_t* dev)
{
	s_vnet.pci = dev;

	// Memory space + bus mastering on (command register, offset 4). Bus
	// mastering is what legalizes the rings' DMA.
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4, dev->command | 0x6);

	if (!virtio_discover_regions(dev))
	{
		printd(DEBUG_NET, "virtio-net: modern capabilities incomplete — legacy-only device? refusing\n");
		return false;
	}

	// The handshake ladder (spec 3.1.1). Write 0 = reset; the device
	// confirms by reading back 0. Bounded spin: a device that won't reset
	// is a device we won't drive.
	vwrite8(s_vnet.common, VC_DEVICE_STATUS, 0);
	for (int spin = 0; vread8(s_vnet.common, VC_DEVICE_STATUS) != 0; spin++)
	{
		if (spin > 1000000)
		{
			printd(DEBUG_NET, "virtio-net: device refuses reset\n");
			return false;
		}
	}
	vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_ACKNOWLEDGE);
	vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_ACKNOWLEDGE | VSTATUS_DRIVER);

	// Feature negotiation: read the offer (two 32-bit windows), accept the
	// intersection with what we implement. We implement exactly TWO
	// things: "you are a modern device" and "you'll tell me your MAC."
	// Everything else — checksum offload, TSO, multiqueue, control queue —
	// is deliberately declined; each is a contract with ring-level
	// consequences, and you accept contracts when you can honor them.
	// (MRG_RXBUF in particular would change the RX header semantics — the
	// ring code above is simpler because we refused it here.)
	vwrite32(s_vnet.common, VC_DEVICE_FEATURE_SEL, 0);
	uint32_t offer0 = vread32(s_vnet.common, VC_DEVICE_FEATURE);
	vwrite32(s_vnet.common, VC_DEVICE_FEATURE_SEL, 1);
	uint32_t offer1 = vread32(s_vnet.common, VC_DEVICE_FEATURE);
	printd(DEBUG_NET, "virtio-net: feature offer 0x%x:0x%x, %u queues\n",
	       offer1, offer0, vread16(s_vnet.common, VC_NUM_QUEUES));

	if (!(offer1 & VIRTIO_F_VERSION_1))
	{
		printd(DEBUG_NET, "virtio-net: device is legacy-only (no VERSION_1) — refusing\n");
		vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_FAILED);
		return false;
	}
	if (!(offer0 & VNET_F_MAC))
	{
		// Spec-legal but hypervisors always offer it. Inventing a random
		// MAC is what real OSes do here; os64 will too when something
		// other than curiosity demands it. Until then: honest refusal.
		printd(DEBUG_NET, "virtio-net: device offers no MAC — refusing (v1)\n");
		vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_FAILED);
		return false;
	}

	vwrite32(s_vnet.common, VC_DRIVER_FEATURE_SEL, 0);
	vwrite32(s_vnet.common, VC_DRIVER_FEATURE, VNET_F_MAC);
	vwrite32(s_vnet.common, VC_DRIVER_FEATURE_SEL, 1);
	vwrite32(s_vnet.common, VC_DRIVER_FEATURE, VIRTIO_F_VERSION_1);

	// FEATURES_OK is a question, not a statement: we set it, then read back
	// to see if the device kept it. A cleared bit means it rejected our
	// subset (spec 3.1.1 step 6) — can't happen with this minimal set from
	// a sane device, but "can't happen" is what tripwires are for.
	uint8_t status = vread8(s_vnet.common, VC_DEVICE_STATUS) | VSTATUS_FEATURES_OK;
	vwrite8(s_vnet.common, VC_DEVICE_STATUS, status);
	if (!(vread8(s_vnet.common, VC_DEVICE_STATUS) & VSTATUS_FEATURES_OK))
	{
		printd(DEBUG_NET, "virtio-net: device rejected feature selection\n");
		vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_FAILED);
		return false;
	}

	// The MAC, read from (virtual) silicon. (QEMU's default is
	// 52:54:00:12:34:56 — the 52:54:00 prefix is QEMU's assigned OUI,
	// the network world's way of saying "a virtual machine was here.")
	for (int i = 0; i < NET_MAC_LEN; i++)
		s_vnet.netdev.mac[i] = vread8(s_vnet.device_cfg, VNET_CFG_MAC + i);

	// Build the queues. Order is the spec's: features are FINAL (the ring
	// format depends on them), THEN rings, THEN the GO bit — DRIVER_OK is
	// the driver's signature on the whole arrangement, and the device may
	// DMA from the instant it's set.
	if (!virtio_setup_queue(VQ_RX, &s_vnet.rx, true) ||
	    !virtio_setup_queue(VQ_TX, &s_vnet.tx, false))
	{
		printd(DEBUG_NET, "virtio-net: virtqueue setup failed\n");
		vwrite8(s_vnet.common, VC_DEVICE_STATUS, VSTATUS_FAILED);
		return false;
	}

	status = vread8(s_vnet.common, VC_DEVICE_STATUS) | VSTATUS_DRIVER_OK;
	vwrite8(s_vnet.common, VC_DEVICE_STATUS, status);
	s_vnet.driver_ok = true;

	// One kick on the RX doorbell: "your empty buffers are waiting."
	// (A device needn't scan a queue it was never notified about.)
	*s_vnet.rx.notify = VQ_RX;

	strcpy(s_vnet.netdev.name, "virtio0");
	s_vnet.netdev.mtu = 1500;
	// virtio without the STATUS feature (declined above) defines the link
	// as always-up — this is truth by specification, not optimism.
	s_vnet.netdev.link_up = true;
	s_vnet.netdev.ops = &s_vnet_ops;
	s_vnet.netdev.driver_data = &s_vnet;

	return net_device_register(&s_vnet.netdev) == 0;
}

// ── Entry point (called from kernel_init, gated by NONET) ───────────────────
void init_virtio_net(void)
{
	// Scan both the device and function tables, the xHCI precedent — QEMU
	// puts virtio-net on a plain slot, but real topologies multi-function
	// everything and the P5 taught us not to assume (five controllers...).
	pci_device_t* found = NULL;
	for (int i = 0; i < kPCIDeviceCount && found == NULL; i++)
		if (kPCIDeviceHeaders[i].vendor == VIRTIO_PCI_VENDOR &&
		    (kPCIDeviceHeaders[i].device == VIRTIO_PCI_DEVICE_TRANS ||
		     kPCIDeviceHeaders[i].device == VIRTIO_PCI_DEVICE_MODERN))
			found = &kPCIDeviceHeaders[i];
	for (int i = 0; i < kPCIFunctionCount && found == NULL; i++)
		if (kPCIDeviceFunctions[i].vendor == VIRTIO_PCI_VENDOR &&
		    (kPCIDeviceFunctions[i].device == VIRTIO_PCI_DEVICE_TRANS ||
		     kPCIDeviceFunctions[i].device == VIRTIO_PCI_DEVICE_MODERN))
			found = &kPCIDeviceFunctions[i];

	if (found == NULL)
	{
		// No NIC attached is a configuration, not a failure — most boots
		// today have none. One quiet debug line so a "why no network?"
		// investigation starts with an answer instead of a search.
		printd(DEBUG_NET, "virtio-net: no device on PCI\n");
		return;
	}

	if (virtio_net_init_device(found))
		printf("virtio-net: %02x:%02x:%02x:%02x:%02x:%02x (rx/tx rings %u/%u)\n",
		       s_vnet.netdev.mac[0], s_vnet.netdev.mac[1], s_vnet.netdev.mac[2],
		       s_vnet.netdev.mac[3], s_vnet.netdev.mac[4], s_vnet.netdev.mac[5],
		       s_vnet.rx.size, s_vnet.tx.size);
	else
		printf("virtio-net: device found but init failed (DEBUG_NET for details)\n");
}
