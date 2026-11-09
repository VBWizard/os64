#ifndef LOOPBACK_H
#define LOOPBACK_H

// loopback.h — `lo`, the interface whose wire is a queue (REMOTE.md § 1).
//
// A conversation between two programs on this machine is ordinary TCP with
// both ends here. The frames go through the whole stack like any others —
// ethernet, IPv4, TCP, checksums and all — and the one difference is where
// transmit puts them: on a queue that knet drains back into the receive path,
// exactly as it drains a NIC's RX ring. Nothing above the seam knows.
//
// WHY TRANSMIT QUEUES INSTEAD OF DELIVERING: a sender reaches transmit while
// holding TCP's locks (a conn's, and sometimes the list lock too). Delivering
// there would re-enter tcp_input, which takes those same locks — a deadlock
// on the first SYN. The queue turns re-entry into later, on knet's thread,
// where nothing is held.
//
// NOT A NIC: lo is deliberately absent from kNetDevices[]. That table is the
// machine's network cards, its order is load-bearing ("kNetDevices[0] is the
// NIC the stack dials", kernel.c), DHCP runs on its first entry, and a boot
// with no card must still answer NO_NIC to a dial for the LAN. knet drains lo
// by name instead.

#include <stdbool.h>
#include <stdint.h>
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"

// 127.0.0.0/8, reserved for exactly this since RFC 990 (1986). RFC 1122 made
// it a MUST that such an address never appear on a wire — which is also the
// rule the whole remote-access design leans on: a service announced only on
// loopback can be reached only from this machine (ipv4_input drops a 127/8
// source or destination arriving on a real card).
#define NET_LOOPBACK_ADDR NET_IPV4(127, 0, 0, 1)

static inline bool net_ip_is_loopback(uint32_t ip)
{
	return (ip >> 24) == 127;
}

// The loopback device, or NULL when networking is disabled (NONET).
extern net_device_t* kNetLoopback;

// Create lo. Called from kernel_init with the network stack, before any NIC,
// in single-threaded boot context.
void init_loopback(void);

// What the queue has done, for /sys/net/knet. `depth_max` is the deepest the
// queue has been: a number near LOOPBACK_SLOTS says a sender outran knet, and
// dropped_full says what that cost.
typedef struct loopback_stats
{
	uint64_t queued;
	uint64_t delivered;
	uint64_t dropped_full;
	uint32_t depth;
	uint32_t depth_max;
	uint32_t slots;
} loopback_stats_t;

void loopback_get_stats(loopback_stats_t* out);

#endif // LOOPBACK_H
