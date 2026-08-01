#ifndef OS64_DIAL_H
#define OS64_DIAL_H

// os64/dial.h — how a program names a destination (NETWORK.md ruling #1).
//
// Two doors, one room:
//
//   os64_net_dial(&dest)          the struct call — what the kernel actually
//                                 speaks (a typed os64_netdest_t, host-order
//                                 fields, see <os64/net.h>)
//   os64_dial("udp!10.0.2.2!53")  the STRING call — Plan 9's dial() notation,
//                                 parsed HERE in the library and lowered onto
//                                 the struct call. The kernel never sees text.
//
// The bang path reads network!address!service. The '!' is honest lineage:
// UUCP bang-path routing (1976) by way of Plan 9's dial() (late 1980s) —
// ratified for os64 by a hippie who appreciated the provenance. The protocol
// segment is ALWAYS explicit (it's the verb of the call: a udp handle is
// datagram-shaped); there is no "net!" wildcard, os64 says what it means.
//
// When the DNS library lands, the middle segment grows names —
// "tcp!example.com!80" — resolved here, same shape, zero kernel changes.
//
// Both return a HANDLE (>= 0) you use with the verbs you already know:
// os64_write sends one datagram, os64_read blocks for one datagram from the
// dialed peer, os64_close hangs up. Negative = refused (bad string, bad
// struct, no NIC, no ports, TCP-before-Phase-4).

#include <stdint.h>
#include "os64/net.h"   // os64_netdest_t + OS64_NET_UDP/TCP — the abi contract

int64_t os64_net_dial(const os64_netdest_t *dest);
int64_t os64_dial(const char *dialstring);

#endif // OS64_DIAL_H
