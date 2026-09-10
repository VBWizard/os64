// test_tcp_options_host.c — drive tcp_options.c on the host, under ASan.
//
// The parser reads the one thing a peer negotiates, and QEMU's user
// networking cannot exercise the half that matters (libslirp does not
// send a window scale), so this is the only proof of it short of the P5
// dialing a real server. Every case names the behaviour it pins.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/net/tcp_options.h"

static int failures;

static void check(int ok, const char* why)
{
	if (!ok)
	{
		failures++;
		printf("FAIL %s\n", why);
	}
}

static void parse(const uint8_t* opts, size_t len, tcp_syn_options_t* out)
{
	// Copy into an exact-size heap block so ASan catches a read past `len`.
	uint8_t* copy = malloc(len ? len : 1);
	memcpy(copy, opts, len);
	tcp_syn_options_parse(copy, len, out);
	free(copy);
}

int main(void)
{
	tcp_syn_options_t o;

	// What slirp and every pre-1992 stack sends: MSS alone.
	{ uint8_t b[] = {2, 4, 0x05, 0xb4};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && !o.wscale_sent && o.wscale == 0, "MSS alone: 1460, no shift"); }

	// What we send: MSS, NOP, window scale — read back by our own parser.
	{ uint8_t b[TCP_SYN_OPTIONS_LEN];
	  size_t n = tcp_syn_options_write(b, 1460, 5);
	  check(n == 8, "write: 8 bytes");
	  check(b[0] == 2 && b[1] == 4 && b[2] == 0x05 && b[3] == 0xb4, "write: MSS option is kind 2 len 4 big-endian");
	  check(b[4] == 1, "write: a NOP pads the scale option to a word boundary");
	  check(b[5] == 3 && b[6] == 3 && b[7] == 5, "write: window scale is kind 3 len 3 shift");
	  parse(b, n, &o);
	  check(o.mss == 1460 && o.wscale_sent && o.wscale == 5, "our own SYN parses back"); }

	// What Linux sends: MSS, SACK-permitted, timestamps, NOP, window scale.
	{ uint8_t b[] = {2, 4, 0x05, 0xb4, 4, 2, 8, 10, 1, 2, 3, 4, 5, 6, 7, 8, 1, 3, 3, 7};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && o.wscale_sent && o.wscale == 7, "Linux's SYN-ACK: SACK-permitted and timestamps skipped, shift 7 found"); }

	// Windows puts the scale before the MSS.
	{ uint8_t b[] = {1, 3, 3, 8, 2, 4, 0x05, 0xb4};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && o.wscale_sent && o.wscale == 8, "scale before MSS: order does not matter"); }

	// A shift of 14 is the ceiling and is reported as sent; 15 is reported
	// raw, because clamping and saying so is the caller's job.
	{ uint8_t b[] = {3, 3, 14};
	  parse(b, sizeof b, &o);
	  check(o.wscale_sent && o.wscale == 14, "shift 14 reported"); }
	{ uint8_t b[] = {3, 3, 15};
	  parse(b, sizeof b, &o);
	  check(o.wscale_sent && o.wscale == 15, "shift 15 reported raw for the caller to clamp"); }

	// A scale option with the wrong length is not trusted.
	{ uint8_t b[] = {3, 4, 7, 0, 2, 4, 0x05, 0xb4};
	  parse(b, sizeof b, &o);
	  check(!o.wscale_sent && o.mss == 1460, "scale with length 4 ignored, MSS after it still read"); }
	{ uint8_t b[] = {2, 3, 0x05, 3, 3, 7};
	  parse(b, sizeof b, &o);
	  check(o.mss == 0, "MSS with length 3 ignored");
	  check(o.wscale_sent && o.wscale == 7, "and the parser stays in step by that length (bytes 3,3,7 read as the scale)"); }

	// End-of-list stops the walk; what came before it stands.
	{ uint8_t b[] = {2, 4, 0x05, 0xb4, 0, 3, 3, 7};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && !o.wscale_sent, "end-of-list: the scale after it is never read"); }

	// A length that runs past the header stops the walk without reading
	// past it (ASan proves the second half).
	{ uint8_t b[] = {2, 4, 0x05, 0xb4, 3, 3};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && !o.wscale_sent, "truncated scale option: MSS kept, nothing read past the end"); }
	{ uint8_t b[] = {2, 4, 0x05, 0xb4, 3};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460 && !o.wscale_sent, "a kind with no length byte stops the walk"); }
	{ uint8_t b[] = {2, 1, 0x05, 0xb4};
	  parse(b, sizeof b, &o);
	  check(o.mss == 0, "a length under 2 is malformed and stops the walk"); }
	{ uint8_t b[] = {2, 0};
	  parse(b, sizeof b, &o);
	  check(o.mss == 0, "a zero length cannot loop forever"); }

	// Nothing at all.
	parse((const uint8_t*)"", 0, &o);
	check(o.mss == 0 && !o.wscale_sent, "empty option space");

	// Unknown kinds are skipped by their own length, whatever it is.
	{ uint8_t b[] = {30, 6, 1, 2, 3, 4, 2, 4, 0x05, 0xb4};
	  parse(b, sizeof b, &o);
	  check(o.mss == 1460, "an unknown kind (30, the MPTCP one) is skipped by its length"); }

	// The window field: shifted down, clamped to sixteen bits, and a SYN's
	// (shift 0) clamps at 65535 whatever the ring holds.
	check(tcp_window_field(1024 * 1024, 5) == 32768, "1MB at shift 5 is 32768");
	check(tcp_window_field(1024 * 1024, 0) == 65535, "1MB unscaled clamps to 65535");
	check(tcp_window_field(65535, 0) == 65535, "65535 unscaled is itself");
	check(tcp_window_field(65536, 0) == 65535, "65536 unscaled clamps");
	check(tcp_window_field(0, 5) == 0, "zero stays zero at any shift");
	check(tcp_window_field(31, 5) == 0, "a window under one unit of shift reads as zero");
	check(tcp_window_field(1460, 5) == 45, "1460 at shift 5 is 45 units (1440 bytes told)");

	// What the peer will READ if we advertise a window: rounded down to the
	// unit, which is what a silly-window floor must judge.
	check(tcp_window_told(1465, 5) == 1440, "1465 free at shift 5 reads as 1440 on the wire, under one segment");
	check(tcp_window_told(1472, 5) == 1472, "1472 at shift 5 is exact");
	check(tcp_window_told(1024 * 1024, 5) == 1024 * 1024, "1MB at shift 5 is exact");
	check(tcp_window_told(1024 * 1024, 0) == 65535, "1MB unscaled reads as 65535");
	check(tcp_window_told(31, 5) == 0, "31 bytes at shift 5 reads as nothing");

	// The most a peer can be told at its shift: the accept bound for a
	// peer that did not scale is the 64KB its field can carry.
	check(tcp_window_ceiling(0) == 65535, "unscaled ceiling is 65535");
	check(tcp_window_ceiling(5) == 65535u << 5, "shift 5 ceiling is 2MB minus a unit");
	check(tcp_window_ceiling(14) == 65535u << 14, "shift 14 ceiling fits 32 bits");

	// And back: the field the peer sent, times its shift.
	check(tcp_window_scaled(65535, 0) == 65535, "unscaled field is itself");
	check(tcp_window_scaled(65535, 14) == 65535u << 14, "shift 14 reaches 1GB without overflow");
	check(tcp_window_scaled(32768, 5) == 1024 * 1024, "32768 at shift 5 is 1MB");

	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("tcp_options host test: PASS\n");
	return 0;
}
