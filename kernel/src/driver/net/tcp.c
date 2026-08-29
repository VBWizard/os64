// tcp.c — the state machine, the arithmetic, and the timer.
//
// Read this file in four passes, in this order:
//   1. the seq_* helpers — everything else is built on them
//   2. tcp_send_segment / tcp_ack — how a segment is put on the wire
//   3. tcp_input — the state machine: every arriving segment, judged
//   4. tcp_poll — the clock: retransmission, connect timeout, TIME_WAIT
// The blocking read/write/dial at the bottom are ordinary os64 handle
// plumbing (pipe.c's park-and-sweep recipe again); the protocol is above.

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"          // kTicksSinceStart — every TCP timer rides it
#include "serial_logging.h"
#include "memcpy.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "smp_core.h"
#include "signals.h"
#include "scheduler.h"
#include "time.h"
#include "os64/net.h"        // OS64_NET_ERR_* — dial's why-it-failed vocabulary (abi)
#include "driver/net/net_device.h"
#include "driver/net/net_wire.h"
#include "driver/net/net_checksum.h"
#include "driver/net/ethernet.h"
#include "driver/net/ipv4.h"
#include "driver/net/tcp.h"

tcp_stats_t kTcpStats;

static tcp_conn_t* kTcpConnList = NULL;
static spinlock_t s_list_lock;
static uint16_t s_next_ephemeral = 49152;   // same dynamic range as UDP (RFC 6335)

// ── 1. SEQUENCE ARITHMETIC ──────────────────────────────────────────────────
// TCP sequence numbers are 32 bits and they WRAP — after 4GB the stream's
// numbering returns to where it started. So "is a newer than b" can never
// be a plain comparison: at the wrap boundary, 0xFFFFFFFF and 0x00000001
// are two apart, but `>` says one is four billion larger.
//
// The fix is the one everybody uses and few write down: subtract, then
// read the result as SIGNED. Unsigned subtraction wraps in exactly the way
// that makes the difference correct modulo 2^32, and the signed cast asks
// "which way is shorter?" — valid as long as the two numbers are within
// 2^31 of each other, which for a real connection they always are. Every
// comparison in this file goes through these four functions; a bare `<`
// on a sequence number is a bug, and this comment exists so a future
// reader knows that on sight.
static inline bool seq_lt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) <  0; }
static inline bool seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) >  0; }
static inline bool seq_geq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

// The initial sequence number. RFC 793 specified a clock-based ISN, and
// Robert Morris showed in 1985 that a PREDICTABLE ISN lets an attacker
// forge a connection blind (the technique behind the 1994 Mitnick attack);
// RFC 6528 now requires unpredictability. os64 has no entropy pool yet, so
// this mixes the tick counter with the connection's own addressing and a
// scramble — better than a counter, honestly weaker than a real PRNG, and
// booked as a DEBT rather than dressed up.
static uint32_t tcp_initial_seq(uint32_t peer_ip, uint16_t peer_port, uint16_t local_port)
{
	uint32_t t = (uint32_t)kTicksSinceStart;
	uint32_t mix = t * 2654435761u;              // Knuth's multiplicative hash constant
	mix ^= peer_ip + ((uint32_t)peer_port << 16) + local_port;
	mix ^= mix >> 13;
	return mix * 2246822519u;
}

// ── 2. SEGMENT CONSTRUCTION ─────────────────────────────────────────────────
// How much room we still have for their bytes — this IS the flow control
// the peer obeys. Advertising 0 stops them; we owe an update when drained.
static uint16_t tcp_window(tcp_conn_t* c)
{
	uint32_t free_space = TCP_RCV_BUF - c->rcv_count;

	// SILLY WINDOW SYNDROME, RECEIVER SIDE (RFC 1122 4.2.3.3), and the
	// single most expensive line in this file until 2026-08-16.
	//
	// A window smaller than one segment is worse than no window at all. A
	// correct sender will not split a segment to squeeze into it (that is
	// its half of the same rule), so it waits — and we, seeing a NON-ZERO
	// window, never considered ourselves stalled and never sent the update
	// that would have freed it. Both sides waited politely until the
	// sender's persist timer fired.
	//
	// The P5 measured the cost exactly (with the 8KB buffer of the time):
	// six segments would fill it, the window would land on 873 bytes, and
	// then FIVE SECONDS of silence
	// before the sender probed again. ~7.3KB per 5s = the 1.7 KB/s that
	// made a 100BASE-TX link perform like a 1993 modem.
	//
	// Rounding a useless window down to zero is what makes it honest: zero
	// means "stop, I will tell you when", which is a promise the code below
	// actually keeps. 873 meant "go ahead" while nothing could.
	if (free_space < TCP_MSS)
		return 0;

	return (uint16_t)(free_space > 0xFFFF ? 0xFFFF : free_space);
}

// Build and transmit one segment. `with_mss` adds the MSS option, which
// only ever rides a SYN (that is the one moment both sides are allowed to
// state their limits).
static void tcp_send_segment(tcp_conn_t* c, uint32_t seq, uint8_t flags,
                             const void* data, uint16_t data_len, bool with_mss)
{
	uint8_t seg[TCP_HDR_MIN + 4 + TCP_MSS];
	uint16_t hdr_len = TCP_HDR_MIN + (with_mss ? 4 : 0);

	net_write16(seg + 0, c->local_port);
	net_write16(seg + 2, c->peer_port);
	net_write32(seg + 4, seq);
	net_write32(seg + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0);
	// Data offset is in 32-bit WORDS, in the high nibble — the header can
	// be 20 to 60 bytes, and this is how the receiver finds the payload.
	seg[12] = (uint8_t)((hdr_len / 4) << 4);
	seg[13] = flags;
	uint16_t win = tcp_window(c);
	net_write16(seg + 14, win);
	c->zero_window = (win == 0);
	net_write16(seg + 16, 0);   // checksum, computed below
	net_write16(seg + 18, 0);   // urgent pointer: never used here (URG is a
	                            // 1981 idea that modern stacks treat as a
	                            // security footgun and applications ignore)

	if (with_mss)
	{
		seg[20] = 2; seg[21] = 4;                    // option kind 2, length 4
		net_write16(seg + 22, TCP_MSS);
	}
	if (data_len)
		memcpy(seg + hdr_len, (void*)data, data_len);

	uint16_t total = (uint16_t)(hdr_len + data_len);

	// The checksum covers a PSEUDO-HEADER (addresses, protocol, length)
	// plus the whole segment — same anti-misdelivery insurance as UDP's,
	// except here it is MANDATORY: TCP has no "sender skipped it" option.
	uint8_t pseudo[12];
	net_write32(pseudo + 0, kNetIPv4Address);
	net_write32(pseudo + 4, c->peer_ip);
	pseudo[8] = 0;
	pseudo[9] = IPV4_PROTO_TCP;
	net_write16(pseudo + 10, total);
	uint32_t sum = net_checksum_add(0, pseudo, sizeof(pseudo));
	sum = net_checksum_add(sum, seg, total);
	net_write16(seg + 16, net_checksum_fold(sum));

	ipv4_send(c->dev, c->peer_ip, IPV4_PROTO_TCP, seg, total);
	kTcpStats.segments_out++;
	if (data_len)
		c->tx_bytes += data_len;
}

// A bare acknowledgement: no data, no state change, just "I'm up to here"
// (and, implicitly, "here is my current window").
static void tcp_ack(tcp_conn_t* c)
{
	tcp_send_segment(c, c->snd_nxt, TCP_ACK, NULL, 0, false);
}

// Arm the retransmit slot with a segment and send it. Everything that must
// survive loss — SYN, data, FIN — goes out through here.
static void tcp_send_reliable(tcp_conn_t* c, const void* data, uint16_t len, uint8_t flags)
{
	if (len)
		memcpy(c->snd_buf, (void*)data, len);
	c->snd_len = len;
	c->snd_flags = flags;
	c->snd_seq = c->snd_nxt;
	c->rto_ticks = TCP_RTO_TICKS;
	c->rto_deadline = kTicksSinceStart + c->rto_ticks;
	c->retries = 0;

	tcp_send_segment(c, c->snd_seq, flags, data, len, (flags & TCP_SYN) != 0);
	// SYN and FIN each consume one sequence number even though they carry
	// no data — that is what makes them acknowledgeable, and it is why the
	// handshake's numbers are always off by one from what you first expect.
	c->snd_nxt += len + (((flags & TCP_SYN) || (flags & TCP_FIN)) ? 1 : 0);
}

// A refusal to a segment we have no connection for: RST, the protocol's
// "you have the wrong number." Built by hand because there is no conn.
static void tcp_send_rst(net_device_t* dev, uint32_t peer_ip, uint16_t local_port,
                         uint16_t peer_port, uint32_t seq, uint32_t ack, bool ack_valid)
{
	uint8_t seg[TCP_HDR_MIN];
	net_write16(seg + 0, local_port);
	net_write16(seg + 2, peer_port);
	// RFC 793 §3.4: if their segment carried an ACK, our RST takes ITS
	// number as our sequence; otherwise we send seq 0 and acknowledge what
	// they sent. Getting this backwards makes an RST the peer ignores.
	net_write32(seg + 4, ack_valid ? ack : 0);
	net_write32(seg + 8, seq);
	seg[12] = (TCP_HDR_MIN / 4) << 4;
	seg[13] = ack_valid ? TCP_RST : (TCP_RST | TCP_ACK);
	net_write16(seg + 14, 0);
	net_write16(seg + 16, 0);
	net_write16(seg + 18, 0);

	uint8_t pseudo[12];
	net_write32(pseudo + 0, kNetIPv4Address);
	net_write32(pseudo + 4, peer_ip);
	pseudo[8] = 0;
	pseudo[9] = IPV4_PROTO_TCP;
	net_write16(pseudo + 10, TCP_HDR_MIN);
	uint32_t sum = net_checksum_add(0, pseudo, sizeof(pseudo));
	sum = net_checksum_add(sum, seg, TCP_HDR_MIN);
	net_write16(seg + 16, net_checksum_fold(sum));

	ipv4_send(dev, peer_ip, IPV4_PROTO_TCP, seg, TCP_HDR_MIN);
	kTcpStats.segments_out++;
}

// ── The receive ring (a byte STREAM, unlike UDP's datagram slots) ───────────
static void tcp_rcv_store(tcp_conn_t* c, const uint8_t* data, uint16_t len)
{
	for (uint16_t i = 0; i < len && c->rcv_count < TCP_RCV_BUF; i++)
	{
		uint32_t slot = (c->rcv_head + c->rcv_count) % TCP_RCV_BUF;
		c->rcv_buf[slot] = data[i];
		c->rcv_count++;
	}
	c->rx_bytes += len;
}

// ── 3. THE STATE MACHINE ────────────────────────────────────────────────────
void tcp_input(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
               const void* seg, uint16_t length)
{
	const uint8_t* p = (const uint8_t*)seg;
	if (length < TCP_HDR_MIN)
		return;

	// Verify the checksum before believing ONE field of this segment.
	uint8_t pseudo[12];
	net_write32(pseudo + 0, src_ip);
	net_write32(pseudo + 4, dst_ip);
	pseudo[8] = 0;
	pseudo[9] = IPV4_PROTO_TCP;
	net_write16(pseudo + 10, length);
	uint32_t sum = net_checksum_add(0, pseudo, sizeof(pseudo));
	sum = net_checksum_add(sum, p, length);
	if (net_checksum_fold(sum) != 0)
	{
		kTcpStats.bad_checksum++;
		return;
	}
	kTcpStats.segments_in++;

	uint16_t src_port = net_read16(p + 0);
	uint16_t dst_port = net_read16(p + 2);
	uint32_t seq      = net_read32(p + 4);
	uint32_t ack      = net_read32(p + 8);
	uint16_t hdr_len  = (uint16_t)((p[12] >> 4) * 4);
	uint8_t  flags    = p[13];
	uint16_t win      = net_read16(p + 14);
	if (hdr_len < TCP_HDR_MIN || hdr_len > length)
		return;
	const uint8_t* data = p + hdr_len;
	uint16_t data_len = (uint16_t)(length - hdr_len);

	// Demux on the FULL four-tuple (their address, their port, our port) —
	// which is what lets two programs on this machine talk to the same
	// server, and the same program talk to two servers, without confusion.
	tcp_conn_t* c = NULL;
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (tcp_conn_t* t = kTcpConnList; t != NULL; t = t->next)
		if (t->local_port == dst_port && t->peer_port == src_port && t->peer_ip == src_ip)
		{
			c = t;
			break;
		}
	spinlock_release_irqrestore(&s_list_lock, lf);

	if (c == NULL)
	{
		// Nobody home. A segment for an unknown connection gets an RST —
		// that is how the far side learns immediately instead of
		// retransmitting into silence (and it is exactly what a closed
		// port answers, which is why "connection refused" is instant).
		kTcpStats.no_connection++;
		if (!(flags & TCP_RST))
			tcp_send_rst(dev, src_ip, dst_port, src_port, seq + data_len +
			             (((flags & TCP_SYN) || (flags & TCP_FIN)) ? 1 : 0),
			             ack, (flags & TCP_ACK) != 0);
		return;
	}

	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);

	// RST: the conversation is over, right now, no reply. (An RST that
	// arrives for a connection we are still opening means "refused".)
	if (flags & TCP_RST)
	{
		kTcpStats.resets_received++;
		if (c->state == TCP_SYN_SENT)
			kTcpStats.connections_refused++;
		c->reset = true;
		c->state = TCP_CLOSED;
		c->snd_len = 0;
		spinlock_release_irqrestore(&c->lock, irqflags);
		printd(DEBUG_NET, "tcp: connection to %u.%u.%u.%u:%u reset by some jerk :-P\n",
		       NET_IPV4_OCTETS(c->peer_ip), c->peer_port);
		return;
	}

	c->snd_wnd = win;

	switch (c->state)
	{
		case TCP_SYN_SENT:
			// The handshake's middle packet. Their SYN carries THEIR
			// starting sequence number; acknowledging it (rcv_nxt = their
			// seq + 1, because SYN counts as a byte) completes the
			// three-way handshake and opens the stream.
			if ((flags & TCP_SYN) && (flags & TCP_ACK) && seq_geq(ack, c->snd_nxt))
			{
				c->rcv_nxt = seq + 1;
				c->snd_una = ack;
				c->snd_len = 0;          // our SYN is acknowledged; disarm the timer
				c->state = TCP_ESTABLISHED;
				kTcpStats.connections_opened++;

				// Parse their MSS option if present — the one negotiation
				// TCP does, and it lives in the SYN's option space.
				c->snd_mss = 536;        // RFC 879's floor when nobody says otherwise
				for (uint16_t o = TCP_HDR_MIN; o + 1 < hdr_len; )
				{
					uint8_t kind = p[o];
					if (kind == 0) break;              // end of options
					if (kind == 1) { o++; continue; }  // NOP padding
					uint8_t olen = p[o + 1];
					if (olen < 2 || o + olen > hdr_len) break;
					if (kind == 2 && olen == 4)
						c->snd_mss = net_read16(p + o + 2);
					o += olen;
				}
				if (c->snd_mss > TCP_MSS)
					c->snd_mss = TCP_MSS;   // never send more than WE can build

				tcp_ack(c);
				printd(DEBUG_NET, "tcp: established with %u.%u.%u.%u:%u (their mss %u, win %u)\n",
				       NET_IPV4_OCTETS(c->peer_ip), c->peer_port, c->snd_mss, win);
			}
			break;

		case TCP_ESTABLISHED:
		case TCP_FIN_WAIT_1:
		case TCP_FIN_WAIT_2:
		case TCP_CLOSE_WAIT:
		case TCP_CLOSING:
		case TCP_LAST_ACK:
		{
			// (a) Their acknowledgement retires our unacknowledged segment.
			if ((flags & TCP_ACK) && seq_gt(ack, c->snd_una))
			{
				c->snd_una = ack;
				if (c->snd_len && seq_geq(ack, c->snd_seq + c->snd_len))
				{
					c->snd_len = 0;      // fully acknowledged: disarm the RTO
					c->snd_flags = 0;
				}
				// Our FIN being acknowledged is a state transition, not
				// just bookkeeping.
				if (c->state == TCP_FIN_WAIT_1 && seq_geq(ack, c->snd_nxt))
					c->state = TCP_FIN_WAIT_2;
				else if (c->state == TCP_CLOSING && seq_geq(ack, c->snd_nxt))
				{
					c->state = TCP_TIME_WAIT;
					c->time_wait_until = kTicksSinceStart + 2 * TCP_MSL_TICKS;
				}
				else if (c->state == TCP_LAST_ACK && seq_geq(ack, c->snd_nxt))
					c->state = TCP_CLOSED;   // fully closed; poll reaps it
			}

			// (b) Their data — but ONLY if it is the next thing we expect.
			// Out-of-order arrivals are dropped (v1, a booked DEBT): the
			// sender's own retransmission will bring them back in order,
			// which is slow but never wrong.
			if (data_len)
			{
				if (seq == c->rcv_nxt)
				{
					uint32_t room = TCP_RCV_BUF - c->rcv_count;   // up to the whole buffer, past 16 bits
					uint16_t take = data_len < room ? data_len : room;
					tcp_rcv_store(c, data, take);
					c->rcv_nxt += take;
					// The receive-path trace, and the reason it is
					// permanent rather than a debugging leftover: this
					// stack moves ~1.7 KB/s on a 100BASE-TX link (measured
					// on the P5, 2026-08-16, with ZERO retransmits — so
					// nothing is being lost, the sender is simply not
					// being let go). Throughput on a healthy connection is
					// window over round-trip, and every term of that is on
					// this line. One boot with DEBUG_NET and a transfer
					// answers "where did the window go" without anybody
					// having to theorize about it first.
					//
					// took < len says our buffer was too small for the
					// segment. win == 0 says we just told the sender to
					// stop, and the next TCPWIN line says how long it took
					// us to take that back.
					printd(DEBUG_NET, "TCPRX t=%lu len=%u room=%u took=%u buf=%u win=%u\n",
					       kTicksSinceStart, data_len, room, take,
					       c->rcv_count, tcp_window(c));
					tcp_ack(c);          // acknowledge immediately: simple,
					                     // and the peer's window depends on it
				}
				else
				{
					c->out_of_order_dropped++;
					tcp_ack(c);          // re-announce where we actually are
				}
			}

			// (c) Their FIN: they will send no more. The stream stays
			// readable until the buffer drains — THEN read() returns 0.
			if ((flags & TCP_FIN) && seq_leq(seq, c->rcv_nxt))
			{
				c->rcv_nxt++;            // FIN occupies one sequence number
				c->rcv_fin = true;
				tcp_ack(c);
				if (c->state == TCP_ESTABLISHED)
					c->state = TCP_CLOSE_WAIT;
				else if (c->state == TCP_FIN_WAIT_1)
					c->state = TCP_CLOSING;      // simultaneous close
				else if (c->state == TCP_FIN_WAIT_2)
				{
					// The normal end of a client connection: both sides
					// have said goodbye. TIME_WAIT now guards the port —
					// see tcp_poll for why that wait is not paranoia.
					c->state = TCP_TIME_WAIT;
					c->time_wait_until = kTicksSinceStart + 2 * TCP_MSL_TICKS;
				}
				printd(DEBUG_NET, "tcp: peer %u.%u.%u.%u:%u closed its half\n",
				       NET_IPV4_OCTETS(c->peer_ip), c->peer_port);
			}
			break;
		}

		case TCP_TIME_WAIT:
			// A duplicate FIN from a peer that missed our ACK. Answer it
			// and restart the clock — that is precisely the job TIME_WAIT
			// exists to do.
			if (flags & TCP_FIN)
			{
				tcp_ack(c);
				c->time_wait_until = kTicksSinceStart + 2 * TCP_MSL_TICKS;
			}
			break;

		default:
			break;
	}

	spinlock_release_irqrestore(&c->lock, irqflags);
}

// ── 4. THE CLOCK ────────────────────────────────────────────────────────────
void tcp_poll(void)
{
	uint64_t now = kTicksSinceStart;

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	tcp_conn_t** pp = &kTcpConnList;
	while (*pp != NULL)
	{
		tcp_conn_t* c = *pp;
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);

		// Retransmission: the one mechanism that makes the stream
		// reliable. Back off exponentially — the network may be congested
		// BECAUSE everyone is retransmitting (October 1986, LBL to
		// Berkeley: 32 kbit/s across a 400-yard link, and Van Jacobson's
		// answer is why the modern internet works at all).
		if (c->snd_len > 0 && c->state != TCP_TIME_WAIT && now >= c->rto_deadline)
		{
			if (++c->retries > TCP_MAX_RETRIES)
			{
				c->reset = true;         // the peer is gone; give up honestly
				c->state = TCP_CLOSED;
				c->snd_len = 0;
				if (c->state == TCP_SYN_SENT)
					kTcpStats.connect_timeouts++;
			}
			else
			{
				c->rto_ticks *= 2;
				if (c->rto_ticks > TCP_RTO_MAX_TICKS)
					c->rto_ticks = TCP_RTO_MAX_TICKS;
				c->rto_deadline = now + c->rto_ticks;
				tcp_send_segment(c, c->snd_seq, c->snd_flags | TCP_ACK,
				                 c->snd_buf, (uint16_t)c->snd_len,
				                 (c->snd_flags & TCP_SYN) != 0);
				c->retransmits++;
				kTcpStats.retransmits++;
				printd(DEBUG_NET, "tcp: retransmit #%u to %u.%u.%u.%u:%u (rto %u ticks)\n",
				       (uint32_t)c->retries, NET_IPV4_OCTETS(c->peer_ip),
				       c->peer_port, c->rto_ticks);
			}
		}

		// The window update we owe a peer we stalled: once the reader
		// drained our buffer, tell them they may speak again. Without
		// this a zero-window connection deadlocks until their probe.
		if (c->zero_window && c->rcv_count < TCP_RCV_BUF / 2 &&
		    (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
		     c->state == TCP_FIN_WAIT_2))
		{
			// Paired with TCPRX above: the gap between the tick that
			// advertised zero and the tick that reopens is the stall, in
			// ticks, measured rather than guessed.
			printd(DEBUG_NET, "TCPWIN t=%lu reopening buf=%u win=%u\n",
			       kTicksSinceStart, c->rcv_count, tcp_window(c));
			tcp_ack(c);
		}

		// TIME_WAIT: why a closed connection lingers. Two reasons, both
		// about the NEXT connection that reuses this port pair: a delayed
		// duplicate from this conversation must expire before it can be
		// mistaken for that one's data, and our final ACK must be
		// retransmittable if the peer's FIN comes again. 2×MSL is the
		// price of not corrupting a future stranger's stream.
		bool reap = false;
		if (c->state == TCP_TIME_WAIT && now >= c->time_wait_until)
			reap = c->detached;
		else if (c->state == TCP_CLOSED && c->detached)
			reap = true;

		spinlock_release_irqrestore(&c->lock, irqflags);

		if (reap)
		{
			*pp = c->next;
			printd(DEBUG_NET, "tcp: reaped %u.%u.%u.%u:%u (local port %u free)\n",
			       NET_IPV4_OCTETS(c->peer_ip), c->peer_port, c->local_port);
			kfree(c->rcv_buf);
			kfree(c->snd_buf);
			kfree(c);
			continue;
		}
		pp = &c->next;
	}
	spinlock_release_irqrestore(&s_list_lock, lf);
}

void tcp_wake_if_ready(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (tcp_conn_t* c = kTcpConnList; c != NULL; c = c->next)
	{
		thread_t *r = NULL, *w = NULL;
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		// Level-triggered, always re-evaluating the CONDITION: a reader
		// wakes for bytes, for EOF, or for death; a writer wakes when its
		// segment is acknowledged, when the handshake completes, or for
		// death. (Same doctrine as pipes — a wake is a hint, never a
		// promise, and both sleepers re-test after waking.)
		// The ISLEEP test is part of the condition (see the long note in
		// icmp_conn.c): a waiter that registered but has not yet parked
		// must stay registered, or its wake is silently dropped and it
		// sleeps the whole backstop second.
		if (c->reader != NULL && c->reader->threadState == THREAD_STATE_ISLEEP &&
		    (c->rcv_count > 0 || c->rcv_fin || c->reset || c->state == TCP_CLOSED))
		{
			r = c->reader;
			c->reader = NULL;
		}
		if (c->writer != NULL && c->writer->threadState == THREAD_STATE_ISLEEP &&
		    (c->snd_len == 0 || c->reset || c->state == TCP_CLOSED))
		{
			w = c->writer;
			c->writer = NULL;
		}
		spinlock_release_irqrestore(&c->lock, irqflags);

		if (r) scheduler_wake_isleep_thread_locked(r);
		if (w) scheduler_wake_isleep_thread_locked(w);
	}
	spinlock_release_irqrestore(&s_list_lock, lf);
}

// ── Handle plumbing: dial, read, write, close ───────────────────────────────
tcp_conn_t* tcp_conn_dial(net_device_t* dev, uint32_t peer_ip, uint16_t peer_port,
                          int64_t* why)
{
	// Pessimism as default: every early exit below is a resource problem
	// unless the handshake itself says otherwise (reset/timeout at the end).
	if (why) *why = OS64_NET_ERR_NO_RESOURCES;

	tcp_conn_t* c = kmalloc(sizeof(*c));
	if (c == NULL)
		return NULL;
	c->rcv_buf = kmalloc(TCP_RCV_BUF);
	c->snd_buf = kmalloc(TCP_MSS);
	if (c->rcv_buf == NULL || c->snd_buf == NULL)
	{
		if (c->rcv_buf) kfree(c->rcv_buf);
		if (c->snd_buf) kfree(c->snd_buf);
		kfree(c);
		return NULL;
	}
	c->dev = dev;
	c->peer_ip = peer_ip;
	c->peer_port = peer_port;
	c->local_port = s_next_ephemeral++;
	if (s_next_ephemeral < 49152)
		s_next_ephemeral = 49152;
	c->snd_mss = 536;
	c->snd_wnd = TCP_MSS;

	uint32_t iss = tcp_initial_seq(peer_ip, peer_port, c->local_port);
	c->snd_una = iss;
	c->snd_nxt = iss;
	c->state = TCP_SYN_SENT;

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	c->next = kTcpConnList;
	kTcpConnList = c;
	spinlock_release_irqrestore(&s_list_lock, lf);

	printd(DEBUG_NET, "tcp: dialing %u.%u.%u.%u:%u from port %u (iss 0x%x)\n",
	       NET_IPV4_OCTETS(peer_ip), peer_port, c->local_port, iss);

	// The opening SYN. It rides the reliable path, so a lost SYN is
	// retransmitted by the same timer that protects data.
	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	tcp_send_reliable(c, NULL, 0, TCP_SYN);
	spinlock_release_irqrestore(&c->lock, irqflags);

	// Wait out the handshake. Polling the state with short naps rather
	// than parking: a connect is a one-time cost, this runs in task
	// context, and it keeps the wake sweep's conditions about DATA only.
	uint64_t deadline = kTicksSinceStart + TCP_CONNECT_TIMEOUT;
	while (kTicksSinceStart < deadline)
	{
		if (c->state == TCP_ESTABLISHED)
			return c;
		if (c->reset || c->state == TCP_CLOSED)
			break;
		wait(10);
	}

	if (c->state != TCP_ESTABLISHED)
	{
		if (!c->reset)
			kTcpStats.connect_timeouts++;
		// The two failures a caller can actually act on: REFUSED means the
		// machine answered and said no (wrong port? service down?); TIMEOUT
		// means silence (wrong address? unplugged? filtered?). This used to
		// live only in a printd — the caller deserves it more than the log.
		if (why) *why = c->reset ? OS64_NET_ERR_REFUSED : OS64_NET_ERR_TIMEOUT;
		printd(DEBUG_NET, "tcp: dial to %u.%u.%u.%u:%u failed (%s)\n",
		       NET_IPV4_OCTETS(peer_ip), peer_port,
		       c->reset ? "refused" : "timed out");
		c->state = TCP_CLOSED;
		c->detached = true;   // let the poll reap it
		return NULL;
	}
	return c;
}

long tcp_conn_read(tcp_conn_t* c, void* buf, size_t len, uint64_t deadline)
{
	core_local_storage_t* cls = get_core_local_storage();
	thread_t* self = cls->currentThread;

	for (;;)
	{
		// Awake at the top: a registration left by the park we just came
		// out of is void, cleared before any return below can leave the
		// slot naming a thread that has moved on (a spurious wake out of a
		// later sleep, or freed memory once it has exited).
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->reader == self)
			c->reader = NULL;
		spinlock_release_irqrestore(&c->lock, irqflags);

		if (signal_park_must_end(self))   // a terminate, or a signal a handler is waiting for
			return TCP_ERR_INTERRUPTED;

		irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->rcv_count > 0)
		{
			// Short reads are the contract (every os64 read returns what
			// it has): a stream reader loops, which is why `cat` works on
			// a pipe, a file, and now a socket with the same code.
			uint32_t n = (len < c->rcv_count) ? (uint32_t)len : c->rcv_count;
			for (uint32_t i = 0; i < n; i++)
				((uint8_t*)buf)[i] = c->rcv_buf[(c->rcv_head + i) % TCP_RCV_BUF];
			c->rcv_head = (c->rcv_head + n) % TCP_RCV_BUF;
			c->rcv_count -= n;

			// AND TELL THEM, NOW. Draining the buffer is the event that
			// makes the sender's data welcome again; nobody else is going
			// to mention it on our behalf. Before this, the only window
			// update lived in tcp_poll, so the best case was a scheduler
			// pass of silence and the WORST case was forever — because
			// that path only ran when we had advertised exactly zero, and
			// a window of 873 is not zero.
			//
			// Gated on actually having been stalled, so a reader keeping
			// up with a slow sender does not put an extra ACK on the wire
			// for every read it performs.
			if (c->zero_window && tcp_window(c) >= TCP_MSS)
				tcp_ack(c);

			spinlock_release_irqrestore(&c->lock, irqflags);
			return (long)n;
		}
		if (c->rcv_fin)
		{
			// Their FIN, and the buffer is drained: THIS is EOF, and it is
			// the same 0 a pipe returns when its last writer leaves.
			spinlock_release_irqrestore(&c->lock, irqflags);
			return 0;
		}
		if (c->reset || c->state == TCP_CLOSED)
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			return TCP_ERR_RESET;
		}
		// Deadline LAST: bytes, EOF, and death all outrank the clock —
		// only pure silence times out. No deregistration needed here: the
		// loop top already voided it, and nothing has re-registered since.
		if (deadline != 0 && kTicksSinceStart >= deadline)
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			return TCP_ERR_TIMEOUT;
		}

		c->reader = self;
		spinlock_release_irqrestore(&c->lock, irqflags);
		// Park until the backstop or the caller's deadline, first to land.
		uint64_t wake = kTicksSinceStart + TICKS_PER_SECOND;
		if (deadline != 0 && deadline < wake)
			wake = deadline;
		signal_raise(SIGSLEEP, wake, self);
	}
}

long tcp_conn_write(tcp_conn_t* c, const void* buf, size_t len)
{
	core_local_storage_t* cls = get_core_local_storage();
	thread_t* self = cls->currentThread;
	const uint8_t* src = (const uint8_t*)buf;
	size_t sent = 0;

	while (sent < len)
	{
		// Same discipline as the reader: awake means our registration is
		// void, and it goes before any return can leave it behind.
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->writer == self)
			c->writer = NULL;
		spinlock_release_irqrestore(&c->lock, irqflags);

		if (signal_park_must_end(self))   // a terminate, or a signal a handler is waiting for
			return sent ? (long)sent : TCP_ERR_INTERRUPTED;

		irqflags = spinlock_acquire_irqsave(&c->lock);
		if (c->reset || c->state == TCP_CLOSED || c->state == TCP_LAST_ACK ||
		    c->state == TCP_TIME_WAIT)
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			return sent ? (long)sent : TCP_ERR_RESET;
		}

		if (c->snd_len == 0)
		{
			// Stop-and-wait (v1): one segment in flight, bounded by both
			// the peer's MSS and the peer's advertised window — the window
			// is the peer's flow control and ignoring it is how you get an
			// RST from a server that meant to be helpful.
			size_t chunk = len - sent;
			if (chunk > c->snd_mss)  chunk = c->snd_mss;
			if (chunk > c->snd_wnd)  chunk = c->snd_wnd;
			if (chunk > 0)
			{
				tcp_send_reliable(c, src + sent, (uint16_t)chunk, TCP_ACK);
				sent += chunk;
				spinlock_release_irqrestore(&c->lock, irqflags);
				continue;
			}
		}

		c->writer = self;
		spinlock_release_irqrestore(&c->lock, irqflags);
		signal_raise(SIGSLEEP, kTicksSinceStart + TICKS_PER_SECOND, self);
	}

	// Wait for the last segment's acknowledgement, so a write() that
	// returns means the bytes are genuinely on their way (not merely
	// queued). Bounded by the RTO machinery, which gives up honestly.
	for (;;)
	{
		uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
		// Awake: last pass's registration is void (see the read loop).
		if (c->writer == self)
			c->writer = NULL;
		bool done = (c->snd_len == 0) || c->reset || c->state == TCP_CLOSED;
		if (done)
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			break;
		}
		// Ask BEFORE registering (rd2): a break after `c->writer = self`
		// left the slot naming a thread that had already returned. And ask
		// UNDER THE SAME LOCK as the completion check and the registration
		// (rd5): dropping the lock between "not done" and "registered" let
		// the final ACK land in the gap — tcp_wake_if_ready found no waiter,
		// and the park ran to its one-second backstop for a write that was
		// already complete (the lost-registration case its comment names).
		// signal_park_must_end takes no lock of its own, so it can be asked
		// here.
		if (signal_park_must_end(self))   // a terminate, or a signal a handler is waiting for
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			break;
		}
		c->writer = self;
		spinlock_release_irqrestore(&c->lock, irqflags);
		signal_raise(SIGSLEEP, kTicksSinceStart + TICKS_PER_SECOND, self);
	}
	return (long)sent;
}

void tcp_conn_close(tcp_conn_t* c)
{
	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);

	// Send our FIN if the connection is still alive. Which state we land
	// in depends on who spoke last — the asymmetry that gives TCP its
	// four-packet close (each direction shuts down on its own schedule).
	if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
	{
		bool they_finished = (c->state == TCP_CLOSE_WAIT);
		// The FIN may have to wait behind an unacknowledged segment; v1
		// takes the simple road and sends it now, since stop-and-wait
		// means at most one thing was outstanding and the retransmit slot
		// belongs to whatever is newest.
		tcp_send_reliable(c, NULL, 0, TCP_FIN | TCP_ACK);
		c->state = they_finished ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
	}

	// Detach, never block: close() returns immediately and the poll
	// finishes the closing dance and the TIME_WAIT nap in the background.
	// A program should not wait 30 seconds for protocol politeness.
	c->detached = true;
	c->reader = NULL;
	c->writer = NULL;
	spinlock_release_irqrestore(&c->lock, irqflags);
}
