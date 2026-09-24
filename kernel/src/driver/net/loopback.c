// loopback.c — lo, the interface whose wire is a queue. loopback.h is the
// contract and says why it queues; REMOTE.md § 1 is the design.

#include <stdint.h>
#include <stdbool.h>
#include "driver/net/loopback.h"
#include "driver/net/net_device.h"
#include "doorbell.h"
#include "knet.h"
#include "memcpy.h"
#include "memory/kmalloc.h"
#include "spinlock.h"
#include "serial_logging.h"
#include "strings/strings.h"
#include "CONFIG.h"

net_device_t* kNetLoopback;

// Sized to hold a full TCP window in flight: a receive ring is 1 MiB, which is
// about 720 full segments, and a sender with that much credit can put all of
// it on the queue before knet has run once. A queue smaller than the
// window drops segments that were never at risk, and each drop costs a
// retransmission timeout on a link with no loss at all. 1024 slots of one
// frame each is 1.5 MiB, once, for the life of the machine.
#define LOOPBACK_SLOTS 1024

// Frames delivered per drain call. knet calls every device's drain once per
// round, so a bounded batch is what keeps a busy loopback from starving a
// NIC's ring within a round; knet comes back for the rest.
#define LOOPBACK_DRAIN_BATCH 64

typedef struct
{
	uint16_t length;
	uint8_t  frame[NET_FRAME_MAX];
} lo_slot_t;

static lo_slot_t*       s_ring;
static uint32_t         s_head;    // oldest queued frame
static uint32_t         s_count;
static spinlock_t       s_lock;
static loopback_stats_t s_stats;   // guarded by s_lock, except `slots`

static int32_t lo_transmit(net_device_t* dev, const void* frame, uint16_t length)
{
	if (length > NET_FRAME_MAX)
	{
		dev->tx_errors++;
		return -1;
	}

	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	if (s_count == LOOPBACK_SLOTS)
	{
		// A full queue drops, as a NIC with a full ring does, and TCP
		// retransmits. Counted twice on purpose: the device's tx_errors is
		// what every card reports, dropped_full is the reason.
		s_stats.dropped_full++;
		dev->tx_errors++;
		spinlock_release_irqrestore(&s_lock, flags);
		return -1;
	}
	lo_slot_t* slot = &s_ring[(s_head + s_count) % LOOPBACK_SLOTS];
	slot->length = length;
	memcpy(slot->frame, (void*)frame, length);
	s_count++;
	s_stats.queued++;
	if (s_count > s_stats.depth_max)
		s_stats.depth_max = s_count;
	// Under the lock: every core is a sender on this device.
	dev->tx_frames++;
	dev->tx_bytes += length;
	spinlock_release_irqrestore(&s_lock, flags);

	// The bell is lock-free and callable from any context (doorbell.h), which
	// matters here: the sender usually holds a TCP lock with interrupts off.
	doorbell_ring(&kNetDoorbell);
	return 0;
}

static bool lo_drain(net_device_t* dev)
{
	// knet is the only caller, so there is one consumer; the frame is copied
	// out under the lock because there are many producers. Delivery happens
	// with the lock RELEASED — the receive path transmits (an ACK, a SYN-ACK),
	// and transmit takes this lock.
	uint8_t buf[NET_FRAME_MAX];
	bool moved = false;
	for (uint32_t n = 0; n < LOOPBACK_DRAIN_BATCH; n++)
	{
		uint64_t flags = spinlock_acquire_irqsave(&s_lock);
		if (s_count == 0)
		{
			spinlock_release_irqrestore(&s_lock, flags);
			break;
		}
		lo_slot_t* slot = &s_ring[s_head];
		uint16_t length = slot->length;
		memcpy(buf, slot->frame, length);
		s_head = (s_head + 1) % LOOPBACK_SLOTS;
		s_count--;
		s_stats.delivered++;
		spinlock_release_irqrestore(&s_lock, flags);

		net_device_rx(dev, buf, length);
		moved = true;
	}
	return moved;
}

static net_operations_t s_lo_ops = {
	.transmit = lo_transmit,
	.drain    = lo_drain,
};

void init_loopback(void)
{
	s_ring = kmalloc(sizeof(lo_slot_t) * LOOPBACK_SLOTS);
	net_device_t* dev = kmalloc(sizeof(net_device_t));
	strncpy(dev->name, "lo", sizeof(dev->name) - 1);
	// The MAC stays all zeros: frames on this wire are addressed to it and
	// from it, eth_input accepts them because they match, and ARP is never
	// asked (ipv4_send_from_ex knows lo's next hop is lo).
	dev->mtu = 1500;
	dev->link_up = true;
	dev->ops = &s_lo_ops;
	s_stats.slots = LOOPBACK_SLOTS;
	kNetLoopback = dev;
	printd(DEBUG_BOOT, "net: loopback lo, 127.0.0.0/8, %u-frame queue\n", LOOPBACK_SLOTS);
}

void loopback_get_stats(loopback_stats_t* out)
{
	uint64_t flags = spinlock_acquire_irqsave(&s_lock);
	*out = s_stats;
	out->depth = s_count;
	spinlock_release_irqrestore(&s_lock, flags);
}
