// ethernet.c — layer 2 in practice: filter, demux, frame, pad.
//
// This file is deliberately thin. Ethernet's job in a modern stack is two
// switch statements and a memcpy — the interesting state machines live one
// layer up (arp.c) and the interesting arithmetic two layers up (ipv4.c).
// What Layer 2 DOES own is the boundary discipline: nothing above this file
// ever sees a frame that isn't addressed to us, and nothing below it ever
// sees a payload without a proper header and wire-legal padding.

#include <stdint.h>
#include <stdbool.h>
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — the one boot config line
#include "memcpy.h"
#include "memset.h"
#include "CONFIG.h"
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/ethernet.h"
#include "driver/net/arp.h"
#include "driver/net/ipv4.h"

eth_stats_t kEthStats;

const uint8_t kEthBroadcastMAC[NET_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Receive: the demux ──────────────────────────────────────────────────────
// This is the net_device RX handler — the single doorway between every NIC
// driver and the whole protocol stack. CONTEXT CONTRACT (net_device.h): may
// be called from IRQ-ish context (today: the processSignals poll), so
// everything from here up must be quick, must not sleep, and takes only
// irqsave locks. The frame belongs to the DRIVER and dies when we return —
// layers that keep anything (ARP's cache) copy what they keep.
static void eth_input(net_device_t* dev, const void* frame, uint16_t length)
{
	const uint8_t* f = (const uint8_t*)frame;

	if (length < ETH_HDR_LEN)
	{
		kEthStats.rx_runt++;
		return;
	}

	// Address filter. Hardware and hypervisor have usually pre-filtered to
	// (ours | broadcast | multicast), but "usually" is not a contract, and
	// multicast chatter (IPv6 neighbor discovery, mDNS) arrives on any real
	// LAN — everything not explicitly for us drops on a named counter here
	// so the layers above can assume "addressed to this machine" as an
	// invariant instead of re-checking it.
	bool for_us = true;
	for (int i = 0; i < NET_MAC_LEN; i++)
		if (f[i] != dev->mac[i]) { for_us = false; break; }
	if (!for_us)
	{
		bool bcast = true;
		for (int i = 0; i < NET_MAC_LEN; i++)
			if (f[i] != 0xFF) { bcast = false; break; }
		if (!bcast)
		{
			kEthStats.rx_not_for_us++;
			return;
		}
	}

	uint16_t ethertype = net_read16(f + 12);
	switch (ethertype)
	{
		case ETH_TYPE_ARP:
			kEthStats.rx_delivered++;
			arp_input(dev, f + ETH_HDR_LEN, (uint16_t)(length - ETH_HDR_LEN));
			break;
		case ETH_TYPE_IPV4:
			kEthStats.rx_delivered++;
			ipv4_input(dev, f + ETH_HDR_LEN, (uint16_t)(length - ETH_HDR_LEN));
			break;
		default:
			// Not an error — a healthy LAN carries protocols we don't
			// speak. Counted so "why isn't X working" investigations can
			// see what we're declining, logged only at debug level.
			kEthStats.rx_unknown_type++;
			printd(DEBUG_NET | DEBUG_DETAILED, "eth: ignoring ethertype 0x%04x (%u bytes)\n",
			       ethertype, length);
			break;
	}
}

// ── Transmit: frame and pad ─────────────────────────────────────────────────
int32_t eth_send(net_device_t* dev, const uint8_t dst_mac[NET_MAC_LEN],
                 uint16_t ethertype, const void* payload, uint16_t length)
{
	// Two ceilings, both checked: the device's MTU bounds the PAYLOAD (the
	// contract with the far side), NET_FRAME_MAX bounds the built frame
	// (the contract with our own buffers). With a 1500 MTU the second can
	// never trip — it's here for the day a driver registers a bigger MTU.
	if (length > dev->mtu || (uint32_t)length + ETH_HDR_LEN > NET_FRAME_MAX)
	{
		kEthStats.tx_too_big++;
		return -1;
	}

	uint8_t frame[NET_FRAME_MAX];
	memcpy(frame, dst_mac, NET_MAC_LEN);
	memcpy(frame + NET_MAC_LEN, dev->mac, NET_MAC_LEN);
	net_write16(frame + 12, ethertype);
	memcpy(frame + ETH_HDR_LEN, payload, length);

	// Pad runts up to the wire minimum with zeros. QEMU's stack forgives
	// short frames (slice 1b's 42-byte ARP got answered), but real NICs
	// and real switches are within their rights to drop them — pad here,
	// once, and no layer above ever thinks about it.
	uint16_t wire_len = (uint16_t)(ETH_HDR_LEN + length);
	if (wire_len < ETH_MIN_FRAME)
	{
		memset(frame + wire_len, 0, ETH_MIN_FRAME - wire_len);
		wire_len = ETH_MIN_FRAME;
	}

	int32_t rc = dev->ops->transmit(dev, frame, wire_len);
	if (rc == 0)
		kEthStats.tx_sent++;
	else
		kEthStats.tx_errors++;
	return rc;
}

// ── Stack bring-up ──────────────────────────────────────────────────────────
void init_net_stack(void)
{
	// Order matters twice here. IPv4 config parses first so the boot line
	// below tells the truth; and ALL of this runs before init_virtio_net
	// (see the call site in kernel.c) so the handler is claimed before any
	// NIC can deliver — the rx_dropped_no_handler counter should only ever
	// move in a build where someone unhooked the stack on purpose.
	ipv4_config_init();
	net_set_rx_handler(eth_input);

	printf("net: ip %u.%u.%u.%u gw %u.%u.%u.%u mask %u.%u.%u.%u (static; IP=/GW=/MASK= to override)\n",
	       NET_IPV4_OCTETS(kNetIPv4Address),
	       NET_IPV4_OCTETS(kNetIPv4Gateway),
	       NET_IPV4_OCTETS(kNetIPv4Netmask));
}
