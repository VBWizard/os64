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
// dialed peer, os64_close hangs up. Negative = refused, and the CODE SAYS
// WHY — see the OS64_NET_ERR_* table in <os64/net.h>: the parser names the
// segment it rejected (BAD_STRING / BAD_ADDRESS / BAD_SERVICE), the kernel
// names its refusal (BAD_DEST / NO_NIC / NO_RESOURCES / REFUSED / TIMEOUT).
// A program can print something a human can act on without a debugger.

#include <stdint.h>
#include "os64/net.h"   // os64_netdest_t + OS64_NET_UDP/TCP — the abi contract

int64_t os64_net_dial(const os64_netdest_t *dest);
int64_t os64_dial(const char *dialstring);

// The inbound door (SERVERS.md; NETWORK.md ruling #3). os64_announce opens
// a port and returns a LISTENER handle; os64_accept reads one completed
// connection off it — the new stream's handle plus the peer's identity in
// `out`. accept blocks like any read; os64_read_for on the listener gives
// it a deadline (OS64_NET_ERR_TIMEOUT when nobody came).
//
//   int64_t d = os64_announce("tcp!*!23");    // or the struct call below
//   os64_netconn_t peer;
//   while (os64_accept((int32_t)d, &peer) == 0)
//       serve(peer.handle);                    // read/write/close like a dial
//
// os64_announce takes the same bang path as os64_dial, read as WHERE I AM:
// the address segment must be '*' ("every address this machine has"), the
// protocol must be tcp. A negative return is a code from the same table
// (os64_dial_reason renders it) plus OS64_NET_ERR_PORT_TAKEN.
int64_t os64_net_announce(const os64_netdest_t *local);
int64_t os64_announce(const char *dialstring);

// Read one connection off a listener handle. Returns 0 and fills `out` on a
// connection; a negative os64_read result otherwise (a signal, a deadline
// from os64_read_for, or the listener closing). A short read is impossible:
// the kernel writes the whole os64_netconn_t or refuses.
int64_t os64_accept(int32_t listener, os64_netconn_t *out);

// A negative dial result, in words a person can act on — one vocabulary
// for every program that dials, so a refusal and a timeout read the same
// on every glass. Never NULL; an unknown code says "refused".
const char *os64_dial_reason(int64_t err);

#endif // OS64_DIAL_H
