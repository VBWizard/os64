#ifndef UDP_H
#define UDP_H

// udp.h — User Datagram Protocol (RFC 768, August 1980 — three pages,
// including the title).
//
// UDP is IP with apartment numbers. IP delivers to a MACHINE; the two
// port fields here deliver to a PROGRAM on it — and that's the entire
// protocol. No connections, no ordering, no retransmission: a datagram
// arrives whole exactly once, or not at all, and finding out which is
// the application's job. That honesty is why it never aged: DNS, DHCP,
// NTP, and every game and video call alive ride UDP precisely because
// they'd rather handle loss themselves than inherit TCP's opinions
// about it. (David Reed fought to keep it this thin against proposals
// to make it "reliable"; being nothing but ports IS the feature.)
//
// The 8-byte header:
//
//   [0] src port (2)   who's asking — where replies go
//   [2] dst port (2)   the apartment number (67 = DHCP server, 53 = DNS...)
//   [4] length   (2)   header + payload
//   [6] checksum (2)   over a PSEUDO-HEADER + the datagram; 0 = "sender
//                      didn't compute one" (legal in IPv4, a fossil of
//                      1980s CPU budgets — we always compute ours, and
//                      verify arrivals whenever the sender did)
//
// KERNEL-INTERNAL for this slice: the bind table below is how in-kernel
// consumers (DHCP first) claim a port. The Phase 3 syscall surface —
// dial-a-peer handles per ruling #4 — will plug into this same table;
// programs never see this header.

#include <stdint.h>
#include "driver/net/net_device.h"

#define UDP_HDR_LEN 8

// Well-known ports this kernel speaks (registry: IANA, straight from the
// RFCs — DHCP's pair is asymmetric on purpose, so a broadcast reply can't
// be mistaken for a broadcast request).
#define UDP_PORT_DHCP_SERVER 67
#define UDP_PORT_DHCP_CLIENT 68

// How many ports can be claimed at once. The syscall era arrived (net_dial:
// every dialed UDP handle binds an ephemeral port through this same table),
// so the bound is no longer "enumerable in-kernel consumers" — it is DHCP
// plus every open conversation in every task. 32 covers a whole shell
// session of chatty tools; the day it doesn't, the counter that refuses
// bind #33 says so loudly.
#define UDP_MAX_BINDINGS 32

typedef struct udp_stats
{
	uint64_t rx_delivered;      // demuxed to a bound port's handler
	uint64_t rx_no_binding;     // arrived for a port nobody claimed
	uint64_t rx_bad_checksum;   // sender computed one and it's wrong
	uint64_t rx_truncated;      // length field disagrees with what arrived
	uint64_t tx_sent;
	uint64_t tx_errors;         // lower layer refused
} udp_stats_t;
extern udp_stats_t kUdpStats;

// A bound port's delivery callback. RX-handler context (the net_device.h
// contract cascades): quick, no sleeping, copy out what you keep.
typedef void (*udp_rx_handler_t)(net_device_t* dev, uint32_t src_ip,
                                 uint16_t src_port, const void* data,
                                 uint16_t length, void* ctx);

// Claim/release a local port. Returns 0, or negative if the port is
// already claimed / the table is full (a port is a mailbox — two owners
// would each get half the mail, so double-binding is refused loudly).
int32_t udp_bind(uint16_t local_port, udp_rx_handler_t handler, void* ctx);
void udp_unbind(uint16_t local_port);

// Inbound datagram (IP header stripped; addresses host-order, straight
// from the IP header because the checksum's pseudo-header needs them).
void udp_input(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
               const void* pkt, uint16_t length);

// Send one datagram. udp_send speaks from kNetIPv4Address like everyone;
// udp_send_from exists for DHCP's from-0.0.0.0 requirement (same story as
// ipv4_send_from — one honest customer). Returns ipv4_send's verdict,
// including -2 "ARP resolving, retry shortly".
int32_t udp_send(net_device_t* dev, uint32_t dst_ip, uint16_t src_port,
                 uint16_t dst_port, const void* payload, uint16_t length);
int32_t udp_send_from(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const void* payload, uint16_t length);

#endif // UDP_H
