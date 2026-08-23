#ifndef DHCP_H
#define DHCP_H

// dhcp.h — Dynamic Host Configuration Protocol client (RFC 2131, 1997).
//
// DHCP is how a machine with no address gets one: shout DISCOVER from
// 0.0.0.0 to everybody, collect an OFFER, shout back a REQUEST for it
// (still broadcast — other servers that offered deserve to hear you chose
// someone else; that's the protocol's built-in rejection letter), and the
// server's ACK makes it yours. The protocol is BOOTP (1985) wearing a
// state machine: same 236-byte packet, same ports, with the options field
// promoted from afterthought to the actual negotiation. That heritage is
// why a 2026 kernel still pads packets for 1985 relay agents.
//
// This client is UDP's first customer (NETWORK.md Phase 3, as
// constitutionally scheduled) and the second protocol state machine in
// the tree — bigger than ARP's cache, still a dry run for TCP's eleven
// states. Four states, honest ones:
//
//   IDLE ──start──> SELECTING ──OFFER──> REQUESTING ──ACK──> BOUND
//                       │ (resend DISCOVER)   │ (resend REQUEST; NAK → SELECTING)
//                       └──── retries exhausted ────> GAVE_UP
//
// POLICY (stated for the reviewer): DHCP runs by default when a NIC
// exists and no IP= token was given; IP= opts out to pure static. Until
// BOUND, the static 10.0.2.x convention defaults REMAIN the live config —
// the ACK overwrites them. So a dead DHCP server degrades to yesterday's
// working behavior (hypervisor NAT still works, honestly logged), never
// to a dead stack; and there is no "unconfigured host" RX window to
// special-case. On a real LAN the placeholder is briefly wrong and then
// leased right — nothing transmits during the gap except DHCP itself,
// which speaks from 0.0.0.0 as RFC 2131 requires regardless.
//
// NOT in v1 (booked, not pretended): lease RENEWAL (T1/T2 timers —
// slirp/VBox leases are functionally eternal; a real LAN lease will want
// this before long uptimes), and DECLINE/RELEASE courtesies.

#include <stdint.h>
#include "driver/net/net_device.h"

typedef enum
{
	DHCP_IDLE = 0,      // never started (no NIC, or IP= chose static)
	DHCP_SELECTING,     // DISCOVER is out, listening for an OFFER
	DHCP_REQUESTING,    // REQUEST is out, listening for the ACK
	DHCP_BOUND,         // leased, configured, done
	DHCP_GAVE_UP,       // retries exhausted; running on the static defaults
} dhcp_state_t;

typedef struct dhcp_stats
{
	dhcp_state_t state;
	uint64_t discovers_sent;
	uint64_t offers_received;
	uint64_t requests_sent;
	uint64_t acks_received;
	uint64_t naks_received;     // server said no — back to SELECTING
	uint64_t ignored;           // wrong xid / wrong state / malformed

	// The lease, once BOUND (host order, like every address in the ABI).
	uint32_t lease_ip;
	uint32_t lease_mask;
	uint32_t lease_gateway;
	uint32_t lease_server;      // who to thank (and someday, renew with)
	uint32_t lease_seconds;     // option 51 — recorded honestly, not yet acted on
	// Option 6, the first name server offered (2026-08-22). The kernel does
	// NOT resolve names — that is libos64's job (resolve.c) — it only keeps
	// what the lease said and publishes it in /sys/net/dhcp, which is where
	// the resolver reads it when /etc/net.conf names no server of its own.
	// 0 = the lease offered none (a static boot, or a stingy server).
	uint32_t lease_dns;
} dhcp_stats_t;
extern dhcp_stats_t kDhcpStats;

// Begin the transaction on `dev`: binds UDP port 68 for the life of the
// system and sends the first DISCOVER. Called from kernel_init after NIC
// registration; replies and retries ride the processSignals poll.
void dhcp_start(net_device_t* dev);

// Retry timer, called from processSignals beside the other polls: resends
// DISCOVER/REQUEST if the answer is overdue (2s), gives up after 4 tries
// per phase. Costs one state compare when IDLE/BOUND.
void dhcp_poll(void);

#endif // DHCP_H
