#ifndef ICMP_CONN_H
#define ICMP_CONN_H

// icmp_conn.h — a dialed echo conversation: the object `ping` is built on.
//
// Same shape as udp_conn (dial → handle → blocking read), with one twist
// worth understanding: ICMP echo has no ports, so the thing that says
// "this reply is mine" is the ECHO IDENTIFIER — 16 bits whose only purpose
// is demultiplexing concurrent pings on one machine. os64 treats it as a
// port in every way that matters: the kernel assigns it at dial, filters
// arrivals by it, and never lets a program choose it (choosing it is how
// you'd steal someone else's replies — which is precisely why raw ICMP
// historically needed root, and why the identifier-owning kernel socket
// is what finally made unprivileged ping possible).
//
// The SEQUENCE number is ours too, incremented per write. Programs that
// need to match a particular reply put a marker in the PAYLOAD and read
// it back — the trick every ping has used since 1983, when Mike Muuss put
// a timestamp in the data field and invented the round-trip measurement
// everyone still uses.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thread.h"
#include "spinlock.h"
#include "driver/net/net_device.h"

// One MTU's worth of echo payload, and a short queue of replies. Deeper
// queueing would only matter for a program that fires many requests
// before reading any — booked as consumer-driven, like everything else.
#define ICMP_CONN_MAX_PAYLOAD  1472
#define ICMP_CONN_QUEUE_SLOTS  4

#define ICMP_CONN_ERR_INTERRUPTED (-3L)

typedef struct icmp_conn
{
	uint32_t peer_ip;
	uint16_t identifier;     // ours, kernel-assigned — the "port" of echo
	uint16_t next_sequence;  // incremented per write
	net_device_t* dev;

	spinlock_t lock;
	uint16_t head, count;
	uint16_t lens[ICMP_CONN_QUEUE_SLOTS];
	uint8_t (*slots)[ICMP_CONN_MAX_PAYLOAD];
	thread_t* volatile waiter;

	uint64_t requests_sent, replies_delivered;
	uint64_t dropped_full;   // replies arriving faster than they're read

	struct icmp_conn* next;
} icmp_conn_t;

// Dial: allocate an identifier and join the delivery list. NULL on no
// memory / no identifiers.
icmp_conn_t* icmp_conn_dial(net_device_t* dev, uint32_t peer_ip);

// Blocking read: one echoed payload, short if the buffer is smaller
// (a datagram is a unit — the tail drops, same contract as UDP).
long icmp_conn_read(icmp_conn_t* c, void* buf, size_t len);

// Send one echo request carrying `buf`. Returns the byte count, or
// negative (oversize, or the wire refused after ARP retries).
long icmp_conn_write(icmp_conn_t* c, const void* buf, size_t len);

void icmp_conn_close(icmp_conn_t* c);

// Called by icmp.c when an echo REPLY arrives: routes it to the handle
// whose identifier matches, or nowhere. RX context.
void icmp_conn_deliver(uint32_t src_ip, uint16_t identifier,
                       const void* payload, uint16_t length);

// Level-triggered wake sweep, beside its UDP and TCP siblings.
void icmp_conn_wake_if_ready(void);

#endif // ICMP_CONN_H
