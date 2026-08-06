// net_device.c — the NIC registry and the inbound choke point.
//
// Deliberately tiny: the seam's value is its SHAPE (net_device.h), not its
// machinery. A fixed array instead of a dlist because NICs are enumerated
// once at boot and never leave (block devices made the same static-fleet
// assumption for years before RAMDisk arrived — when hotplug or unregister
// becomes real, this file grows a lock and the header's comments change in
// the same commit).

#include "driver/net/net_device.h"
#include "serial_logging.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "strings/strings.h"

net_device_t* kNetDevices[NET_MAX_DEVICES];
uint32_t kNetDeviceCount = 0;

// One handler for the whole machine, not one per device: inbound demux
// (which protocol? which connection?) is the STACK's job — the ethertype
// field does the routing, not the NIC identity. Per-device handlers would
// just be N copies of the same pointer. (The device still rides along as
// the first argument of every delivery, because ARP replies and DHCP
// answers must go back out the interface they arrived on.)
static net_rx_handler_t kNetRxHandler = NULL;

int32_t net_device_register(net_device_t* dev)
{
	// A NIC without transmit is not a NIC; a NIC with no ops table is a
	// bug in its driver's init order. Refuse loudly at the boundary —
	// the alternative is a NULL call through ops->transmit at the first
	// ARP request, minutes away from the actual mistake.
	if (dev == NULL || dev->ops == NULL || dev->ops->transmit == NULL)
		return -1;

	if (kNetDeviceCount >= NET_MAX_DEVICES)
	{
		// Counted refusal, not silent truncation (house rule): if a
		// machine ever presents a fifth NIC, the log says exactly what
		// was ignored and why.
		printd(DEBUG_BOOT, "net: device table full (%u), refusing '%s'\n",
		       kNetDeviceCount, dev->name);
		return -2;
	}

	// Boot-time registration is single-threaded (drivers init from
	// kernel_init before the scheduler owns the world), so no lock —
	// the day a NIC registers from anywhere else, this needs one.
	kNetDevices[kNetDeviceCount++] = dev;

	printd(DEBUG_BOOT, "net: registered %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, mtu %u\n",
	       dev->name,
	       dev->mac[0], dev->mac[1], dev->mac[2],
	       dev->mac[3], dev->mac[4], dev->mac[5],
	       dev->mtu);
	return 0;
}

void net_set_rx_handler(net_rx_handler_t handler)
{
	// Single word-sized store — atomic on x86-64, so a frame arriving
	// mid-claim sees either the old handler or the new one, never a torn
	// pointer. NULL is a legal argument (release: frames go back to being
	// counted drops).
	kNetRxHandler = handler;
}

void net_device_rx(net_device_t* dev, const void* frame, uint16_t length)
{
	// Every frame that dies, dies ON A COUNTER. These two branches are
	// the seam's whole no-silent-drops promise:
	if (length > NET_FRAME_MAX)
	{
		// A frame bigger than our ceiling means jumbo frames or a driver
		// bug — either way, truncating it and parsing half a packet
		// would be worse than losing it. Count and drop whole.
		dev->rx_dropped_too_big++;
		return;
	}

	net_rx_handler_t handler = kNetRxHandler;  // one read: stable for this frame
	if (handler == NULL)
	{
		// The stack hasn't claimed inbound yet (Phase 1 has drivers but
		// no protocols). Broadcast chatter WILL land here on any real
		// network — ARP asks about everyone. Counted, not logged: a busy
		// LAN would turn per-frame logging into a boot-log flood.
		dev->rx_dropped_no_handler++;
		return;
	}

	dev->rx_frames++;
	dev->rx_bytes += length;
	handler(dev, frame, length);
}
