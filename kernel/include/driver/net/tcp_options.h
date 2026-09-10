#ifndef TCP_OPTIONS_H
#define TCP_OPTIONS_H

// tcp_options.h — the SYN's option space, and the arithmetic of a scaled
// window.
//
// TCP negotiates exactly once, in the SYN exchange, and it does so in a
// list of typed options after the fixed header. Two of them matter to this
// stack: the maximum segment size (RFC 793, kind 2) and the window scale
// (Jacobson, Braden and Borman, RFC 1323, 1992 — RFC 7323 today; kind 3).
// The second exists because the window field is sixteen bits, so a
// connection whose window never grows is capped at 64KB per round trip
// whatever the link can carry; each side names a shift count in its SYN,
// and every window field after the handshake is read as the field shifted
// left by the OTHER side's count. Both sides must send it for either to
// scale, and the SYN's own window field is never scaled (§2.2, §2.3).
//
// PURE ON PURPOSE. This module has no kernel headers, no locks and no
// state, so tools/test_tcp_options_host.sh drives it with plain cc under
// ASan. That is the only proof of the parser short of a scaling peer, and
// QEMU's user networking is not one (libslirp does not negotiate the
// option), so the host test is where a wrong length or a missed NOP is
// caught before the P5 dials a real server.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2
#define TCP_OPT_WSCALE  3

// RFC 7323 §2.3: a shift past 14 is refused and 14 used, because 2^30 is
// the most a 32-bit sequence space can have in flight without ambiguity.
#define TCP_WSCALE_MAX  14

// What our SYN carries: MSS (4 bytes), a NOP, then the window scale (3
// bytes). The NOP is padding to a 32-bit boundary — the header's data
// offset counts in words — and it sits BEFORE the scale option, the order
// 4.4BSD sent and the one the parsers of the internet grew up on.
#define TCP_SYN_OPTIONS_LEN 8

typedef struct
{
	uint16_t mss;          // 0 = the peer said nothing
	bool     wscale_sent;  // the peer offered a shift at all — the rule
	                       // for scaling is that BOTH sides did
	uint8_t  wscale;       // the shift as sent, unclamped; the caller
	                       // applies TCP_WSCALE_MAX and says so
} tcp_syn_options_t;

// Read a SYN's option list. Stops at end-of-list, at a malformed length or
// at a length that runs past `len`, keeping whatever was read before the
// fault; every unknown kind is skipped by its own length. Kinds with the
// wrong length are ignored rather than trusted.
void tcp_syn_options_parse(const uint8_t* opts, size_t len, tcp_syn_options_t* out);

// Write our SYN's options into `out` (TCP_SYN_OPTIONS_LEN bytes) and
// return how many were written.
size_t tcp_syn_options_write(uint8_t* out, uint16_t mss, uint8_t wscale);

// The window field for a segment: the window shifted down by our own
// count, clamped to what sixteen bits can say. A shift of 0 is the
// unscaled field, which is what a SYN always carries.
uint16_t tcp_window_field(uint32_t window, uint8_t shift);

// The window a received field means: the field shifted up by the peer's
// count. A SYN's field is passed with shift 0.
uint32_t tcp_window_scaled(uint16_t field, uint8_t shift);

#endif
