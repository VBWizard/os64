#ifndef ETHERNET_H
#define ETHERNET_H

// ethernet.h — layer 2: framing and the ethertype demux (NETWORK.md Phase 2).
//
// Ethernet is the 1973 Xerox PARC design (Metcalfe & Boggs) that outlived
// every rival — token ring, FDDI, ATM — by being simple and cheap, and its
// FRAME format outlived even its own coax-and-collisions physics: modern
// "ethernet" is point-to-point switched links and WiFi, but the 14-byte
// header below is unchanged since the DIX Ethernet II spec of 1982. This is
// the header the net_device seam's "frame in, frame out" rule is about.
//
//   [0]  dst MAC (6)    who this is for (ff:ff:ff:ff:ff:ff = everyone)
//   [6]  src MAC (6)    who sent it
//   [12] ethertype (2)  WHICH PROTOCOL the payload is — the demux key
//   [14] payload...     (the FCS trailer is hardware's business, not ours)
//
// Ethertype is the whole reason this layer exists as code: one NIC carries
// many protocols, and these two bytes are how arrivals find their parser.
// Values below 0x0600 would instead be an 802.3 length (the 1983 IEEE
// standard split); nothing we speak uses that encoding, so it demuxes to
// the unknown-type counter like anything else we don't parse.

#include <stdint.h>
#include "driver/net/net_device.h"

#define ETH_HDR_LEN    14
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806

// Minimum frame length ON THE WIRE (sans FCS). Classic shared-coax ethernet
// needed 64 bytes (with FCS) so a collision could be detected before the
// sender finished transmitting — 51.2µs of round-trip physics at 10Mbit
// across 2.5km of cable. The physics are 40 years dead; the minimum is
// forever, and receivers may still discard "runts", so eth_send pads.
#define ETH_MIN_FRAME  60

// Counters, per the no-silent-drops doctrine: every frame that reaches this
// layer either demuxes or lands on a named number a debugger can read.
typedef struct eth_stats
{
	uint64_t rx_delivered;      // demuxed to ARP or IPv4
	uint64_t rx_not_for_us;     // dst MAC neither ours nor broadcast
	uint64_t rx_unknown_type;   // ethertype we don't parse (IPv6 says hello)
	uint64_t rx_runt;           // shorter than the 14-byte header
	uint64_t tx_sent;
	uint64_t tx_too_big;        // payload exceeded the device MTU
	uint64_t tx_errors;         // driver refused the frame
} eth_stats_t;
extern eth_stats_t kEthStats;

// ff:ff:ff:ff:ff:ff — "everyone on this link." Defined once in ethernet.c;
// ARP requests and IP broadcasts both address it.
extern const uint8_t kEthBroadcastMAC[NET_MAC_LEN];

// Bring up the protocol stack: parse the static IP configuration and claim
// the net_device RX handler slot. Called from kernel_init BEFORE the NIC
// drivers, so the stack is listening before the first frame can arrive —
// no boot-time window where real traffic lands on the no-handler counter.
void init_net_stack(void);

// Build a frame around `payload` and transmit it: dst from the caller, src
// from the device, ethertype as given, zero-padded up to ETH_MIN_FRAME.
// Returns the driver's verdict (0 = on the wire, negative = refused).
int32_t eth_send(net_device_t* dev, const uint8_t dst_mac[NET_MAC_LEN],
                 uint16_t ethertype, const void* payload, uint16_t length);

#endif // ETHERNET_H
