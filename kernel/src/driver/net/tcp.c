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
#include "panic.h"          // the tombstone queue's invariant, when it breaks
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

// Exported (tcp.h) so /sys/net/tcp can walk what tcp_poll walks.
tcp_conn_t* kTcpConnList = NULL;
spinlock_t kTcpListLock;
// The dynamic range, 49152..65535 — same as UDP's (RFC 6335).
#define TCP_EPHEMERAL_BASE   49152u
#define TCP_EPHEMERAL_COUNT  16384u

static uint16_t s_next_ephemeral = TCP_EPHEMERAL_BASE;

// One bit per ephemeral port (2KB): which of them a listed connection holds.
// Guarded by kTcpListLock like the list itself — set when a dial's draw
// claims the port, cleared when the reaper unlists the connection — so the
// draw asks "is this port spoken for?" in one bit test instead of walking
// the whole connection list per candidate.
static uint64_t s_ephemeral_used[TCP_EPHEMERAL_COUNT / 64];

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
//
// The per-dial serial is the part that is not about attackers: it is what
// makes two incarnations of one four-tuple DISTINGUISHABLE. A refused
// dial gives its port back at once, so a redial can wear the same tuple
// within the same 10ms tick, and the acceptance rules in tcp_input judge
// a straggler from the first attempt by whether it acknowledges this
// one's ISS. Tick alone gave both the same ISS (Codex, PR #46); RFC 793's
// 4µs clock and RFC 6528's M term exist for exactly this, and a serial
// that moves on every dial is the same guarantee without a finer clock.
// Read under kTcpListLock, which every dial holds here.
static uint32_t s_dial_serial;
static uint32_t tcp_initial_seq(uint32_t peer_ip, uint16_t peer_port, uint16_t local_port)
{
	uint32_t t = (uint32_t)kTicksSinceStart;
	uint32_t mix = t * 2654435761u;              // Knuth's multiplicative hash constant
	mix ^= peer_ip + ((uint32_t)peer_port << 16) + local_port;
	mix ^= mix >> 13;
	mix *= 2246822519u;
	return mix + (++s_dial_serial) * 2654435761u; // distinct per dial, whatever the tick
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
	// A tombstone is not a party to anything: its port is back in the
	// draw, so the tuple it shows may belong to a live dial, and the
	// answer to a segment aimed at its dead conversation is "nobody
	// home" below.
	//
	// THE CONN IS PINNED BEFORE THE LIST LOCK DROPS: its lock is taken
	// under the list lock, and the list lock is released without
	// restoring interrupts, so `irqflags` walks across to the conn lock
	// and interrupts stay off for the whole handoff. Every unlink-and-free
	// runs under the list lock (tcp_poll's reap, the tombstone cap's
	// eviction), so a conn found here is either locked by us before the
	// freer takes the list lock — and the freer waits for us — or already
	// gone before we look. Found-then-lock, with a gap between, was a
	// pointer to a conn another core could free (Codex, PR #46).
	tcp_conn_t* c = NULL;
	uint64_t irqflags = spinlock_acquire_irqsave(&kTcpListLock);
	for (tcp_conn_t* t = kTcpConnList; t != NULL; t = t->next)
		if (!t->tombstone &&
		    t->local_port == dst_port && t->peer_port == src_port && t->peer_ip == src_ip)
		{
			c = t;
			spinlock_acquire(&c->lock);
			break;
		}

	if (c == NULL)
	{
		spinlock_release_irqrestore(&kTcpListLock, irqflags);
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
	spinlock_release(&kTcpListLock);   // irqflags now belongs to c->lock

	// RST: the conversation is over, right now, no reply. (An RST that
	// arrives for a connection we are still opening means "refused" —
	// dial sees `reset` and counts it when it entombs the attempt.)
	//
	// In SYN_SENT the RST must acknowledge OUR SYN — RFC 793 §3.9 — or it
	// is not ours: a failed dial gives its port back at once, and after
	// enough dials the draw wraps, so a new dial to the same peer can wear
	// the four-tuple of an earlier one whose replies are still on the
	// wire. What tells the incarnations apart is the sequence space —
	// every dial mints a distinct ISS (tcp_initial_seq's serial) — so a
	// stale answer acknowledges a number this connection never sent, and
	// is dropped (Codex, PR #46).
	//
	// Once SYNCHRONIZED the same stranger is judged by RFC 5961 (2010,
	// the blind-reset fix): an RST is honored only at exactly the next
	// sequence number we expect; one merely inside our window earns a
	// "challenge" ACK that tells the real peer where we are and would
	// draw a genuine RST back; anything else is dropped. A SYN in a
	// synchronized state is challenged the same way, never obeyed — a
	// stale SYN+ACK is precisely that — and an ACK for bytes we have not
	// sent is answered and dropped (RFC 793 §3.9). Guarding SYN_SENT
	// alone protected the replacement only until it established; the
	// stragglers do not know the handshake is over.
	bool synchronized = (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
	                     c->state == TCP_FIN_WAIT_2 || c->state == TCP_CLOSE_WAIT ||
	                     c->state == TCP_CLOSING || c->state == TCP_LAST_ACK ||
	                     c->state == TCP_TIME_WAIT);
	if (flags & TCP_RST)
	{
		if (c->state == TCP_SYN_SENT && (!(flags & TCP_ACK) || ack != c->snd_nxt))
		{
			printd(DEBUG_NET, "tcp: RST for a SYN this connection did not send (ack 0x%x, ours 0x%x) — dropped\n",
			       ack, c->snd_nxt);
			spinlock_release_irqrestore(&c->lock, irqflags);
			return;
		}
		if (synchronized && seq != c->rcv_nxt)
		{
			bool in_window = seq_geq(seq, c->rcv_nxt) && seq_lt(seq, c->rcv_nxt + tcp_window(c));
			printd(DEBUG_NET, "tcp: RST at seq 0x%x, expected 0x%x — %s\n",
			       seq, c->rcv_nxt, in_window ? "challenged" : "dropped");
			if (in_window)
				tcp_ack(c);
			spinlock_release_irqrestore(&c->lock, irqflags);
			return;
		}
		kTcpStats.resets_received++;
		c->reset = true;
		c->state = TCP_CLOSED;
		c->snd_len = 0;
		// Logged under the lock: past the release the conn is the
		// reaper's to free, and nothing here may read it again.
		printd(DEBUG_NET, "tcp: connection to %u.%u.%u.%u:%u reset by some jerk :-P\n",
		       NET_IPV4_OCTETS(c->peer_ip), c->peer_port);
		spinlock_release_irqrestore(&c->lock, irqflags);
		return;
	}

	if (synchronized && (flags & TCP_SYN))
	{
		printd(DEBUG_NET, "tcp: SYN in a synchronized state from %u.%u.%u.%u:%u — challenged\n",
		       NET_IPV4_OCTETS(c->peer_ip), c->peer_port);
		tcp_ack(c);
		spinlock_release_irqrestore(&c->lock, irqflags);
		return;
	}
	if (synchronized && (flags & TCP_ACK) && seq_gt(ack, c->snd_nxt))
	{
		printd(DEBUG_NET, "tcp: ACK 0x%x for bytes we have not sent (snd_nxt 0x%x) — dropped\n",
		       ack, c->snd_nxt);
		tcp_ack(c);
		spinlock_release_irqrestore(&c->lock, irqflags);
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
			//
			// The ACK must be EXACTLY our SYN's successor: only the SYN is
			// outstanding, and anything else acknowledges a segment this
			// connection never sent — a previous incarnation's, when the
			// port draw has wrapped onto a tuple whose replies are still
			// in flight (the RST rule above says why that is reachable).
			// RFC 793 §3.9: an unacceptable ACK in SYN_SENT is answered
			// with RST at its own number and dropped, and we stay put;
			// our own SYN's answer is still coming. `>=` here let a stale
			// SYN+ACK complete a handshake it was never part of.
			if ((flags & TCP_ACK) && ack != c->snd_nxt)
			{
				printd(DEBUG_NET, "tcp: SYN_SENT answer acks 0x%x, ours is 0x%x — a stale incarnation, RST\n",
				       ack, c->snd_nxt);
				spinlock_release_irqrestore(&c->lock, irqflags);
				tcp_send_rst(dev, src_ip, dst_port, src_port, 0, ack, true);
				return;
			}
			if ((flags & TCP_SYN) && (flags & TCP_ACK))
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
				else if (seq_gt(seq, c->rcv_nxt))
				{
					// Ahead of us: something between here and the peer
					// reordered or dropped. THIS is the count the
					// reassembly debt is judged by.
					c->out_of_order_dropped++;
					kTcpStats.out_of_order_dropped++;
					tcp_ack(c);          // re-announce where we actually are
				}
				else
				{
					// Behind us: bytes we already took, sent again because
					// our ack never reached them (or not in time). The
					// same drop, a different weather report — and the
					// re-announce is the cure, which is why it is not the
					// reordering count (Codex, PR #46).
					kTcpStats.duplicates_dropped++;
					tcp_ack(c);
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

		case TCP_CLOSED:
			// The morgue keeps this conn LISTED for the observer, but the
			// protocol is over: RFC 793 answers anything sent to a CLOSED
			// connection with RST, exactly as if no connection existed.
			// Without this, a peer answering our long-dead SYN late would
			// retransmit into the morgue's silence instead of learning
			// immediately. (RST segments never reach here — judged above.)
			spinlock_release_irqrestore(&c->lock, irqflags);
			tcp_send_rst(dev, src_ip, dst_port, src_port, seq + data_len +
			             (((flags & TCP_SYN) || (flags & TCP_FIN)) ? 1 : 0),
			             ack, (flags & TCP_ACK) != 0);
			return;

		default:
			break;
	}

	spinlock_release_irqrestore(&c->lock, irqflags);
}

// Give a port back to the draw. Caller holds kTcpListLock. Guarded because
// only drawn ports live in the bitmap: a local port below the ephemeral
// range (a listener's service port, when LISTEN arrives) was never claimed
// there, and subtracting the base from it would underflow into a wild bit
// index.
static void tcp_port_release_locked(uint16_t port)
{
	if (port >= TCP_EPHEMERAL_BASE)
	{
		uint32_t bit = port - TCP_EPHEMERAL_BASE;
		s_ephemeral_used[bit / 64] &= ~(1ULL << (bit % 64));
	}
}

// ── The list, and the tombstone queue threaded through it ──────────────────
// Both under kTcpListLock. The queue is FIFO, oldest at the head: a push
// is O(1), the cap's eviction is O(1), and only the reaper's removal of a
// tombstone from the middle walks it — bounded by the cap, never by the
// number of connections.
static tcp_conn_t* s_tomb_head;
static tcp_conn_t* s_tomb_tail;
static uint32_t    s_tomb_count;

static void tcp_list_unlink_locked(tcp_conn_t* c)
{
	if (c->prev != NULL)
		c->prev->next = c->next;
	else
		kTcpConnList = c->next;
	if (c->next != NULL)
		c->next->prev = c->prev;
	c->next = NULL;
	c->prev = NULL;
}

static void tcp_tomb_push_locked(tcp_conn_t* c)
{
	c->tomb_next = NULL;
	if (s_tomb_tail != NULL)
		s_tomb_tail->tomb_next = c;
	else
		s_tomb_head = c;
	s_tomb_tail = c;
	s_tomb_count++;
}

static void tcp_tomb_remove_locked(tcp_conn_t* c)
{
	tcp_conn_t* prev = NULL;
	tcp_conn_t** pp = &s_tomb_head;
	while (*pp != NULL && *pp != c)
	{
		prev = *pp;
		pp = &(*pp)->tomb_next;
	}
	if (*pp == NULL)
		panic("tcp: tombstone %p is listed but not queued\n", c);
	*pp = c->tomb_next;
	if (s_tomb_tail == c)
		s_tomb_tail = prev;
	c->tomb_next = NULL;
	s_tomb_count--;
}

// ── The heap invariant (tcp.h heap_moves) ──────────────────────────────────
// TCP's heap moves ONLY through these two, ONLY under kTcpListLock, and
// each move ticks heap_moves in the same critical section. The tripwire
// is the lock word itself: a caller that does not hold it is the next
// uncounted edge, and it panics by name instead of waiting for a review
// to find it. (The allocator never returns NULL — exhaustion panics —
// so there is no failure path to handle.)
static void* tcp_heap_alloc_locked(size_t bytes)
{
	if (kTcpListLock == 0)
		panic("tcp: heap allocation outside kTcpListLock\n");
	kTcpStats.heap_moves++;
	return kmalloc(bytes);
}

static void tcp_heap_free_locked(void* p)
{
	if (kTcpListLock == 0)
		panic("tcp: heap free outside kTcpListLock\n");
	kfree(p);
	kTcpStats.heap_moves++;
}

// ── Stripping, entombing, evicting (tcp.h § the morgue) ────────────────────
// All three run under the list lock; strip and entomb also under c->lock.
// Once `stripped` is set nothing touches the buffers: the demux skips a
// tombstone outright, the retransmit excludes CLOSED and TIME_WAIT, the
// receive path never stores in TIME_WAIT, and a segment already past the
// demux finds the state under the conn lock and answers with headers only.
static void tcp_conn_strip_locked(tcp_conn_t* c)
{
	tcp_heap_free_locked(c->rcv_buf);
	tcp_heap_free_locked(c->snd_buf);
	c->rcv_buf = NULL;
	c->snd_buf = NULL;
	c->rcv_count = 0;
	c->snd_len = 0;
	c->stripped = true;
}

// CLOSED and detached: strip, give the port back, join the morgue queue,
// start the morgue clock.
static void tcp_conn_entomb_locked(tcp_conn_t* c, uint64_t now)
{
	tcp_conn_strip_locked(c);
	tcp_port_release_locked(c->local_port);
	c->tombstone = true;
	c->closed_at = now;
	tcp_tomb_push_locked(c);
}

// The cap: past TCP_MORGUE_TOMBSTONES the queue's head — the oldest — is
// evicted. Caller holds the list lock and NO conn lock, because THE
// EVICTED CONN'S LOCK IS DRAINED BEFORE THE FREE: a receiver that found
// it in the demux before it was a tombstone pinned it by taking its lock
// under the list lock, so it holds that lock now or never will, and
// acquiring and releasing it here, with the list lock held, waits out the
// one holder that can exist. Never called from inside a list walk — an
// eviction unlinks an arbitrary node, and a walker's cursor may be
// pointing through it.
static void tcp_tomb_evict_over_cap_locked(void)
{
	while (s_tomb_count > TCP_MORGUE_TOMBSTONES)
	{
		tcp_conn_t* evicted = s_tomb_head;
		tcp_tomb_remove_locked(evicted);
		tcp_list_unlink_locked(evicted);
		spinlock_acquire(&evicted->lock);
		spinlock_release(&evicted->lock);
		tcp_heap_free_locked(evicted);
		kTcpStats.connections_reaped++;
	}
}

// A failed dial: the dialing thread entombs its own attempt at once
// rather than leaving it for the poll's next sweep — a refused dial
// returns in a millisecond and costs the caller no handle, so a loop of
// them is bounded by nothing else. The failure counters live here because
// dial is the only place that knows which failure it was.
static void tcp_dial_entomb(tcp_conn_t* c)
{
	uint64_t lf = spinlock_acquire_irqsave(&kTcpListLock);
	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	bool refused = c->reset;
	c->state = TCP_CLOSED;
	c->detached = true;
	tcp_conn_entomb_locked(c, kTicksSinceStart);
	spinlock_release_irqrestore(&c->lock, irqflags);
	tcp_tomb_evict_over_cap_locked();
	if (refused)
		kTcpStats.connections_refused++;
	else
		kTcpStats.connect_timeouts++;
	spinlock_release_irqrestore(&kTcpListLock, lf);
}

uint64_t tcp_leak_bracket_snapshot(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&kTcpListLock);
	uint64_t moves = kTcpStats.heap_moves;
	spinlock_release_irqrestore(&kTcpListLock, lf);
	return moves;
}

// ── 4. THE CLOCK ────────────────────────────────────────────────────────────
void tcp_poll(void)
{
	uint64_t now = kTicksSinceStart;

	uint64_t lf = spinlock_acquire_irqsave(&kTcpListLock);
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
		//
		// TIME_WAIT and CLOSED are past transmitting. TIME_WAIT still
		// answers a late FIN (tcp_input); CLOSED speaks only through
		// tcp_input's RST. The CLOSED exclusion is what keeps the morgue
		// display-only whatever a death left armed in the slot: a listed
		// corpse must not retransmit it.
		if (c->snd_len > 0 && c->state != TCP_TIME_WAIT && c->state != TCP_CLOSED &&
		    now >= c->rto_deadline)
		{
			if (++c->retries > TCP_MAX_RETRIES)
			{
				// No connect_timeouts count here, and that is not an
				// omission: dial's own 10-second deadline gives up on a
				// silent handshake long before a SYN's retries could
				// exhaust (their backoff sums past 30 seconds), and dial
				// counts that death itself. What dies HERE is an
				// established conversation whose peer stopped answering,
				// and the caller hears it as a reset.
				c->reset = true;         // the peer is gone; give up honestly
				c->state = TCP_CLOSED;
				c->snd_len = 0;
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
		//
		// CLOSED gets a linger too — the morgue — but for the OBSERVER,
		// not the protocol: a dial that timed out or a fetch that ended
		// used to vanish from /sys/net/tcp on the very next pass, so the
		// aftermath of a failure was unreadable the moment it mattered
		// (Chris, on VBox, racing cat against the reaper to glimpse
		// SYN_SENT). Either linger is held as a ROW: the first sweep to
		// find a conn in TIME_WAIT strips its buffers, and the first to
		// find one CLOSED and detached entombs it — buffers, port, the
		// morgue clock starts (tcp.h § the morgue). A dial that failed
		// entombed itself before this sweep could.
		bool reap = false;
		if (c->state == TCP_TIME_WAIT && c->detached)
		{
			if (!c->stripped)
				tcp_conn_strip_locked(c);
			reap = now >= c->time_wait_until;
		}
		else if (c->state == TCP_CLOSED && c->detached)
		{
			if (!c->tombstone)
				tcp_conn_entomb_locked(c, now);
			reap = (now >= c->closed_at + TCP_MORGUE_TICKS);
		}

		spinlock_release_irqrestore(&c->lock, irqflags);

		if (reap)
		{
			// The unlink leaves *pp naming c's successor, which is what
			// the loop reads next. Buffers went at the strip; a tombstone
			// gave its port back then too, TIME_WAIT gives it back now.
			tcp_list_unlink_locked(c);
			if (c->tombstone)
				tcp_tomb_remove_locked(c);
			else
				tcp_port_release_locked(c->local_port);
			printd(DEBUG_NET, "tcp: reaped %u.%u.%u.%u:%u (local port %u %s)\n",
			       NET_IPV4_OCTETS(c->peer_ip), c->peer_port, c->local_port,
			       c->tombstone ? "was already free" : "free");
			tcp_heap_free_locked(c);
			kTcpStats.connections_reaped++;
			continue;
		}
		pp = &c->next;
	}
	// After the walk, never inside it: an eviction unlinks whichever
	// tombstone is oldest, and the walk's cursor may point through it.
	tcp_tomb_evict_over_cap_locked();
	spinlock_release_irqrestore(&kTcpListLock, lf);
}

void tcp_wake_if_ready(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&kTcpListLock);
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
	spinlock_release_irqrestore(&kTcpListLock, lf);
}

// ── Handle plumbing: dial, read, write, close ───────────────────────────────
tcp_conn_t* tcp_conn_dial(net_device_t* dev, uint32_t peer_ip, uint16_t peer_port,
                          int64_t* why)
{
	// Pessimism as default: every early exit below is a resource problem
	// unless the handshake itself says otherwise (reset/timeout at the end).
	if (why) *why = OS64_NET_ERR_NO_RESOURCES;

	// THE DRAW, THE ALLOCATION AND THE PUBLICATION ARE ONE CRITICAL
	// SECTION. The draw first, so a dial that finds the range full leaves
	// having allocated nothing; then the buffers, under the same lock,
	// because TCP's heap moves only under it (tcp.h heap_moves — an
	// allocation made outside it and published later was a 65KB gap no
	// bracket could see); then the row. Two tasks dialing at once cannot
	// open two connections on one local port, because the bit is claimed
	// in the same section the row is published in.
	//
	// The bitmap holds the port of every listed connection that is not a
	// tombstone — the live ones, and TIME_WAIT, which is the port
	// protection that state exists to give. A tombstone gave its port
	// back when it was entombed, having nothing left to protect (tcp.h §
	// the morgue).
	//
	// The search is BOUNDED to one pass over the dynamic range at one bit
	// test per candidate, and an exhausted range is a clean NO_RESOURCES —
	// not a spin. Both halves matter because this lock is irqsave: the
	// worst case runs with interrupts off on this core and every other
	// TCP user waiting on the lock, so it has to be cheap at exactly the
	// load — a full range — it exists to handle. A draw that walked the
	// connection list per candidate was ~16384 × list-length compares
	// under those conditions (Codex, PR #46; 16384 connections at ~64KB
	// apiece fit comfortably in this machine's RAM, so a full range is
	// reachable, not theoretical).
	uint64_t lf = spinlock_acquire_irqsave(&kTcpListLock);
	uint16_t local_port = 0;
	bool found = false;
	for (uint32_t tries = TCP_EPHEMERAL_COUNT; tries > 0; tries--)
	{
		uint16_t candidate = s_next_ephemeral++;
		if (s_next_ephemeral < TCP_EPHEMERAL_BASE)
			s_next_ephemeral = TCP_EPHEMERAL_BASE;
		uint32_t bit = candidate - TCP_EPHEMERAL_BASE;
		if (s_ephemeral_used[bit / 64] & (1ULL << (bit % 64)))
			continue;
		s_ephemeral_used[bit / 64] |= (1ULL << (bit % 64));
		local_port = candidate;
		found = true;
		break;
	}
	if (!found)
	{
		spinlock_release_irqrestore(&kTcpListLock, lf);
		return NULL;         // *why already answers NO_RESOURCES, the honest verdict
	}

	tcp_conn_t* c = tcp_heap_alloc_locked(sizeof(*c));
	c->rcv_buf = tcp_heap_alloc_locked(TCP_RCV_BUF);
	c->snd_buf = tcp_heap_alloc_locked(TCP_MSS);
	c->dev = dev;
	c->peer_ip = peer_ip;
	c->peer_port = peer_port;
	c->local_port = local_port;
	c->snd_mss = 536;
	c->snd_wnd = TCP_MSS;

	uint32_t iss = tcp_initial_seq(peer_ip, peer_port, c->local_port);
	c->snd_una = iss;
	c->snd_nxt = iss;
	c->state = TCP_SYN_SENT;

	c->next = kTcpConnList;
	c->prev = NULL;
	if (kTcpConnList != NULL)
		kTcpConnList->prev = c;
	kTcpConnList = c;
	spinlock_release_irqrestore(&kTcpListLock, lf);

	printd(DEBUG_NET, "tcp: dialing %u.%u.%u.%u:%u from port %u (iss 0x%x)\n",
	       NET_IPV4_OCTETS(peer_ip), peer_port, c->local_port, iss);

	// The opening SYN. It rides the reliable path, so a lost SYN is
	// retransmitted by the same timer that protects data.
	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	tcp_send_reliable(c, NULL, 0, TCP_SYN);
	spinlock_release_irqrestore(&c->lock, irqflags);

	// Wait out the handshake by polling the state rather than joining the
	// wake sweep, which keeps that sweep's conditions about DATA only. The
	// nap is REAL: this said "short naps" while calling wait(), which spins
	// on `pause` — so a dial to an address that answers with silence burned a
	// whole core for the ten-second timeout. On a machine where the gateway
	// drops SYNs instead of resetting them that is every single dial. Same
	// cadence, no burn; safe here because the lock above is already released
	// and this runs in task context.
	uint64_t deadline = kTicksSinceStart + TCP_CONNECT_TIMEOUT;
	while (kTicksSinceStart < deadline)
	{
		if (c->state == TCP_ESTABLISHED)
			return c;
		if (c->reset || c->state == TCP_CLOSED)
			break;
		nap(10);
	}

	if (c->state != TCP_ESTABLISHED)
	{
		// The two failures a caller can actually act on: REFUSED means the
		// machine answered and said no (wrong port? service down?); TIMEOUT
		// means silence (wrong address? unplugged? filtered?). This used to
		// live only in a printd — the caller deserves it more than the log.
		if (why) *why = c->reset ? OS64_NET_ERR_REFUSED : OS64_NET_ERR_TIMEOUT;
		printd(DEBUG_NET, "tcp: dial to %u.%u.%u.%u:%u failed (%s)\n",
		       NET_IPV4_OCTETS(peer_ip), peer_port,
		       c->reset ? "refused" : "timed out");
		tcp_dial_entomb(c);
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
