#ifndef UDP_CONN_H
#define UDP_CONN_H

// udp_conn.h — a dialed UDP conversation, shaped for the handle table.
//
// This is the layer NETWORK.md ruling #4 bought: "dial a peer once, then
// plain read/write." udp.c below it stays protocol-pure (ports, checksums,
// the bind table); THIS module owns everything conversational — the
// ephemeral local port, the peer filter, the queue of arrived datagrams,
// and the blocking read that parks a thread until one shows up. The
// syscall layer sees an opaque object behind HANDLE_NET_UDP and calls the
// four verbs below; nothing above this header knows a bind table exists.
//
// QUEUE DOCTRINE: arrivals land in a bounded ring of whole datagrams
// (UDP delivers packets, not bytes — a ring of bytes would glue datagrams
// together and invent TCP badly). When the ring is full, the NEWEST
// arrival drops on a counter — the bound IS the flow control (the pipe
// doctrine, datagram edition), and dropping is UDP-honest: the protocol's
// whole contract is "arrives once, or not at all."

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thread.h"
#include "spinlock.h"
#include "driver/net/net_device.h"

// Slot math: an MTU-1500 link carries at most 1500-20-8 = 1472 UDP payload
// bytes, so one slot holds any datagram that can physically arrive. Eight
// slots ≈ 12KB per conversation — generous for request/response protocols
// (DNS keeps 1 in flight), bumped when a real consumer queues deeper.
#define UDP_CONN_MAX_DGRAM     1472
#define UDP_CONN_QUEUE_SLOTS   8

// In-band sentinels for udp_conn_read (the pipe.c convention).
#define UDP_CONN_ERR_INTERRUPTED (-3L)   // terminate signal landed; caller dies at the boundary
#define UDP_CONN_ERR_TIMEOUT     (-4L)   // caller's read deadline expired with no datagram

typedef struct udp_conn
{
	// The conversation's identity (host order, like every address in the ABI).
	uint32_t peer_ip;
	uint16_t peer_port;
	uint16_t local_port;     // ephemeral, allocated at dial, freed at close
	net_device_t* dev;

	// The arrival ring: whole datagrams, head/count discipline.
	spinlock_t lock;         // guards ring + waiter; RX enqueues, task context drains
	uint16_t head, count;
	uint16_t lens[UDP_CONN_QUEUE_SLOTS];
	uint8_t (*slots)[UDP_CONN_MAX_DGRAM];   // kmalloc'd with the conn

	// The parked reader (pipe.c's single-waiter shape — one task owns a
	// handle, so one blocked reader is the whole population).
	thread_t* volatile waiter;

	// No silent anything.
	uint64_t rx_delivered;        // datagrams handed to read()
	uint64_t rx_dropped_full;     // ring full — newest arrival sacrificed
	uint64_t rx_dropped_stranger; // right port, wrong peer — connected-UDP filter

	struct udp_conn* next;   // kUdpConnList, for the wake sweep
} udp_conn_t;

// Dial: allocate an ephemeral port, bind it, join the sweep list.
// NULL = no ports / no memory / bind table full.
udp_conn_t* udp_conn_dial(net_device_t* dev, uint32_t peer_ip, uint16_t peer_port);

// Blocking read: returns one whole datagram (short if buf is smaller —
// the tail drops, the classic UDP truncation contract), or
// UDP_CONN_ERR_INTERRUPTED if a terminate signal landed. Task context
// ONLY (parks on SIGSLEEP).
// `deadline` = absolute kTicksSinceStart tick after which the wait gives
// up and returns UDP_CONN_ERR_TIMEOUT; 0 = wait forever (the eternal
// contract). A datagram already queued always wins over an expired
// deadline — data outranks the clock.
long udp_conn_read(udp_conn_t* c, void* buf, size_t len, uint64_t deadline);

// Send one datagram to the dialed peer. Returns byte count, or negative
// (oversize, or the wire refused after ARP retries). Task context ONLY
// (may sleep briefly waiting out ARP resolution).
long udp_conn_write(udp_conn_t* c, const void* buf, size_t len);

// Hang up: unbind the port, leave the sweep list, free everything.
void udp_conn_close(udp_conn_t* c);

// The level-triggered wake sweep — called from processSignals beside
// pipe_wake_if_ready, under the scheduler lock, AFTER the NIC poll (so a
// datagram delivered this pass wakes its reader this pass).
void udp_conn_wake_if_ready(void);

#endif // UDP_CONN_H
