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

// Is anything we sent still waiting for its acknowledgement? This is the
// retransmit timer's ARMED test, and it is asked of the sequence space
// rather than of the ring's byte count because a SYN and a FIN each occupy
// a sequence number while carrying no bytes: judged by byte count, a bare
// SYN or FIN was never armed, so a lost one was never resent — every dial
// into a black hole sat out its full deadline on ONE packet, and a lost
// FIN left the peer holding open a conversation we had already forgotten.
// Disarming needs no site of its own: the ACK that advances snd_una to
// snd_nxt is the disarm.
static inline bool tcp_unacked(const tcp_conn_t* c) { return seq_gt(c->snd_nxt, c->snd_una); }

// How many RING bytes are in flight: the sequence span snd_una..snd_nxt
// less the sequence numbers that name no byte — the SYN while the
// handshake is open, the FIN once it has gone out. The ring's unsent tail
// starts this many bytes past its head.
static inline uint32_t tcp_bytes_in_flight(const tcp_conn_t* c)
{
	uint32_t span = c->snd_nxt - c->snd_una;
	uint32_t units = (c->state == TCP_SYN_SENT ? 1 : 0) + (c->snd_fin_sent ? 1 : 0);
	return span > units ? span - units : 0;   // an acknowledged FIN leaves span 0, not -1
}

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

// ── 2a. THE SENDER: the ring, the window, the clock ─────────────────────────
//
// Everything that must survive loss — SYN, data, FIN — is described by the
// ring and three sequence marks (tcp.h: snd_una, snd_nxt, and the ring's
// end), and is put on the wire by tcp_output, which sends as much of the
// unsent tail as the two windows allow. The peer's window is flow control
// — its buffer, and ignoring it is how a helpful server sends an RST. The
// congestion window is the network's, and it is the half Van Jacobson
// added in 1988 after the ARPAnet spent 1986 retransmitting itself into
// the ground: start small, double per round trip until loss says stop,
// then grow by one segment per round trip. Send only what BOTH allow.
//
// Retransmission is always from the ring's head — the oldest unacked byte
// — because that is the byte the peer is waiting for: a hole anywhere
// else is behind it in sequence and delivers the moment the head does.
// There are two cues, and they resend different amounts. The timer
// (tcp_poll) is the slow one — nothing heard for a whole RTO — and it
// says nothing about what else was lost, so it sends everything
// outstanding again from the head, in order, under slow start. Three
// duplicate acks (tcp_input) is the fast one, and the reason a loss under
// a working connection costs one round trip and not a timeout: every
// segment that lands past the hole makes the peer repeat where it is,
// three repeats cannot be reordering, and the head alone is resent.

// One clean round-trip sample, in ticks — Jacobson/Karels 1988, in the
// paper's own integer form (RFC 6298 §2.3 says the same in reals):
// err = r - srtt; srtt += err/8; rttvar += (|err| - rttvar)/4; timer =
// srtt + 4·rttvar. With srtt kept ×8 and rttvar ×4 the divisions are
// shifts of the stored values and the 4· is free. A sample of zero ticks
// is real on a virtual wire — the ack came back inside one tick — and the
// floor is what keeps the timer honest there.
static void tcp_rtt_sample(tcp_conn_t* c, uint32_t r)
{
	if (c->rtt_samples == 0)
	{
		c->srtt_x8   = r << 3;
		c->rttvar_x4 = r << 1;       // rttvar = r/2, ×4
	}
	else
	{
		int32_t err = (int32_t)r - (int32_t)(c->srtt_x8 >> 3);
		c->srtt_x8 = (uint32_t)((int32_t)c->srtt_x8 + err);
		if (err < 0)
			err = -err;
		err -= (int32_t)(c->rttvar_x4 >> 2);
		c->rttvar_x4 = (uint32_t)((int32_t)c->rttvar_x4 + err);
	}
	uint32_t rto = (c->srtt_x8 >> 3) + (c->rttvar_x4 > 1 ? c->rttvar_x4 : 1);  // G = one tick
	if (rto < TCP_RTO_MIN_TICKS)
		rto = TCP_RTO_MIN_TICKS;
	if (rto > TCP_RTO_MAX_TICKS)
		rto = TCP_RTO_MAX_TICKS;
	c->rto = rto;
	c->rto_backed_off = false;
	c->rtt_samples++;
	kTcpStats.rtt_samples++;
	printd(DEBUG_NET, "tcp: rtt %u ticks -> srtt_x8 %u rttvar_x4 %u rto %u ticks\n",
	       r, c->srtt_x8, c->rttvar_x4, c->rto);
}

// (Re)start the retransmit clock with the timeout in force. The clock
// belongs to WHAT IS OUTSTANDING, so it restarts whenever that changes —
// the first segment into an empty window, or an ACK that retired the head
// with more behind it (RFC 6298 §5.3). The retry BUDGET is not this
// function's: it is spent by timeouts and refilled only by an ack of new
// data (tcp_input), so a timeout's own resend cannot refill it. A timer
// that has backed off stays backed off until that same ack.
static void tcp_rto_arm(tcp_conn_t* c)
{
	if (!c->rto_backed_off)
		c->rto_ticks = c->rto;
	c->rto_deadline = kTicksSinceStart + c->rto_ticks;
}

// Start the round-trip stopwatch on a segment ending at `seq_end`, if none
// is running: one segment is timed at a time, and the ack that reaches
// seq_end stops it. A retransmission of ANYTHING cancels it (Karn's rule,
// tcp_retransmit_head) — the ack could be for either copy.
static void tcp_rtt_start(tcp_conn_t* c, uint32_t seq_end)
{
	if (c->rtt_timing)
		return;
	c->rtt_timing = true;
	c->rtt_seq = seq_end;
	c->snd_sent_at = kTicksSinceStart;
}

// The ring's bytes, `off` past its head, copied out contiguous — a
// segment's payload may wrap the ring's end.
static void tcp_snd_copy_out(const tcp_conn_t* c, uint32_t off, uint8_t* dst, uint32_t len)
{
	uint32_t at = (c->snd_head + off) % TCP_SND_BUF;
	uint32_t first = TCP_SND_BUF - at;
	if (first > len)
		first = len;
	memcpy(dst, c->snd_buf + at, first);
	if (len > first)
		memcpy(dst + first, c->snd_buf, len - first);
}

// Send the ring's bytes [off, off+len) as one segment, the FIN riding it
// if the caller says so (tcp_output decides whether the FIN's sequence
// unit fits the window; a retransmit repeats what the segment first
// carried). The ACK bit always rides data: there is always something to
// acknowledge once the handshake is done.
static void tcp_send_data_segment(tcp_conn_t* c, uint32_t off, uint32_t len, bool fin)
{
	uint8_t payload[TCP_MSS];
	tcp_snd_copy_out(c, off, payload, len);
	tcp_send_segment(c, c->snd_una + off, fin ? (TCP_FIN | TCP_ACK) : TCP_ACK,
	                 payload, (uint16_t)len, false);
}

// The opening SYN: no ACK bit (there is nothing yet to acknowledge, and a
// listener answers a SYN+ACK it never asked for with RST), the MSS option,
// and one sequence number consumed — that is what makes a SYN
// acknowledgeable at all, and why the handshake's numbers are always off
// by one from what you first expect. Caller holds c->lock.
static void tcp_send_syn(tcp_conn_t* c)
{
	tcp_send_segment(c, c->snd_nxt, TCP_SYN, NULL, 0, true);
	c->snd_nxt += 1;
	c->snd_max = c->snd_nxt;
	tcp_rto_arm(c);
	tcp_rtt_start(c, c->snd_nxt);
}

// May this connection put data on the wire? Established, or closing in a
// way that still owes the peer bytes already written (our FIN goes out
// behind them). SYN_SENT has only the SYN to send and TIME_WAIT/CLOSED
// have nothing; a stripped conn has no ring at all.
static bool tcp_may_send_data(const tcp_conn_t* c)
{
	return !c->stripped &&
	       (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT ||
	        c->state == TCP_FIN_WAIT_1 || c->state == TCP_CLOSING ||
	        c->state == TCP_LAST_ACK);
}

// Put as much of the ring's unsent tail on the wire as both windows allow,
// then the FIN if one is queued and everything before it has gone. Called
// under c->lock wherever room opens: a write queued bytes, an ack retired
// some, the peer's window grew. `force` sends one byte PAST a zero window
// — the persist probe (tcp.h TCP_PERSIST_MIN_TICKS) — and nothing else
// ever ignores the window.
//
// SENDER-SIDE SILLY WINDOW AVOIDANCE (RFC 1122 §4.2.3.4), the sender's
// half of the rule tcp_window keeps for the receiver: with data in flight,
// a segment goes out only if it is a FULL one or it EMPTIES the ring —
// a 200-byte segment into a window that has 200 bytes left, with 60KB
// queued behind it, is a stream of tinygrams paced by the peer's reads.
// With NOTHING in flight the rule yields to progress: whatever the window
// allows goes, because waiting for a full segment's worth of window with
// no ack due is waiting for nothing.
static void tcp_output(tcp_conn_t* c, bool force)
{
	if (!tcp_may_send_data(c) || c->snd_fin_sent)
		return;

	uint32_t in_flight = tcp_bytes_in_flight(c);
	// LIMITED TRANSMIT (RFC 3042): the first two duplicate acks each buy
	// one new segment past cwnd. Each duplicate says a segment LEFT the
	// network, and a loss near the end of a window would otherwise never
	// collect the third duplicate that cues fast retransmit — the new
	// segments are what provoke it. The peer's window still bounds it.
	uint32_t cwnd = c->cwnd;
	if (c->dup_acks > 0 && c->dup_acks < 3)
		cwnd += c->dup_acks * (uint32_t)c->snd_mss;
	uint32_t window = c->snd_wnd < cwnd ? c->snd_wnd : cwnd;
	bool sent_any = false;

	while (in_flight < c->snd_count)
	{
		uint32_t unsent = c->snd_count - in_flight;
		uint32_t usable = window > in_flight ? window - in_flight : 0;
		uint32_t chunk = unsent;
		if (chunk > c->snd_mss)
			chunk = c->snd_mss;
		if (chunk > usable)
			chunk = usable;
		if (force && chunk == 0 && in_flight == 0)
			chunk = 1;                                     // the probe: one byte past the window
		if (chunk == 0)
			break;
		if (in_flight > 0 && chunk < c->snd_mss && chunk < unsent)
			break;                                         // SWS: neither full nor final; wait for the ack

		// THE FIN IS A SEQUENCE NUMBER TOO, and it has to fit the peer's
		// window like any byte: a FIN one past the window's edge is
		// dropped at the door (RFC 793 §3.9's acceptability test), and
		// snd_fin_sent would then keep it from going again until the
		// timer. So it rides this segment only if the window has a unit
		// to spare past the data; otherwise it stays queued for the next
		// pass, when an ack has widened the window.
		bool fin = c->snd_fin && in_flight + chunk == c->snd_count && chunk + 1 <= usable;
		// Below snd_max this is a RESEND — a timeout pulled snd_nxt back
		// (tcp_poll) and the ring is going out again in order. It counts as
		// one, and it starts no stopwatch (Karn).
		bool resend = seq_lt(c->snd_una + in_flight, c->snd_max);
		if (!tcp_unacked(c))
			tcp_rto_arm(c);                                // first thing into an empty window: the clock starts
		tcp_send_data_segment(c, in_flight, chunk, fin);
		if (resend)
		{
			c->retransmits++;
			kTcpStats.retransmits++;
		}
		else
			tcp_rtt_start(c, c->snd_una + in_flight + chunk);
		// The send-side twin of TCPRX: every term of throughput = window /
		// round trip, per segment, so one DEBUG_NET boot answers "which
		// window held the upload back" without theorizing first.
		printd(DEBUG_NET, "TCPTX t=%lu seq=%u len=%u inflight=%u queued=%u wnd=%u cwnd=%u%s\n",
		       kTicksSinceStart, c->snd_una + in_flight, chunk, in_flight + chunk,
		       c->snd_count - in_flight - chunk, c->snd_wnd, c->cwnd, resend ? " resend" : "");
		in_flight += chunk;
		c->snd_nxt += chunk;
		sent_any = true;
		if (fin)
		{
			c->snd_fin_sent = true;                        // it rode that segment
			c->snd_nxt += 1;
		}
		if (seq_gt(c->snd_nxt, c->snd_max))
			c->snd_max = c->snd_nxt;
	}

	// A FIN with nothing to carry it: the ring was already empty, its last
	// segment went out before close() queued the FIN, or the last segment
	// filled the window to the byte and the FIN had to wait. Sent when its
	// sequence unit is inside the peer's window — or when nothing is in
	// flight at all, since a zero-length segment AT rcv_nxt is acceptable
	// whatever the window says (§3.9's table, the zero-window row).
	if (c->snd_fin && !c->snd_fin_sent && in_flight == c->snd_count &&
	    (in_flight == 0 || in_flight < c->snd_wnd))
	{
		if (!tcp_unacked(c))
			tcp_rto_arm(c);
		tcp_send_segment(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0, false);
		if (seq_lt(c->snd_nxt, c->snd_max))
		{
			c->retransmits++;                              // the FIN went out before a timeout pulled it back
			kTcpStats.retransmits++;
		}
		c->snd_fin_sent = true;
		c->snd_nxt += 1;
		if (seq_gt(c->snd_nxt, c->snd_max))
			c->snd_max = c->snd_nxt;
		sent_any = true;
	}

	// Data waiting, nothing in flight, and the window says stop: start the
	// persist clock, unless it is already running — at the RTO or a second,
	// whichever is longer, then doubling per probe up to the RTO ceiling.
	// Anything in flight makes it unnecessary (that segment's ack will
	// carry the window), and a window that is open again resets the
	// interval for the next time.
	if (!sent_any && in_flight == 0 && c->snd_count > 0 && c->snd_wnd == 0)
	{
		if (c->persist_deadline == 0)
		{
			uint32_t floor = c->rto > TCP_PERSIST_MIN_TICKS ? c->rto : TCP_PERSIST_MIN_TICKS;
			uint32_t next = c->persist_ticks == 0 ? floor : c->persist_ticks * 2;
			if (next > TCP_RTO_MAX_TICKS)
				next = TCP_RTO_MAX_TICKS;
			c->persist_ticks = next;
			c->persist_deadline = kTicksSinceStart + next;
		}
	}
	else
	{
		c->persist_deadline = 0;
		if (c->snd_wnd != 0)
			c->persist_ticks = 0;
	}
}

// Resend what the peer is waiting for: the SYN while the handshake is
// open, else the ring's head (one segment's worth, the FIN riding it if it
// is the end), else a bare FIN. Karn: the stopwatch is cancelled — from
// here until a fresh segment starts a new one, no ack is a sample.
static void tcp_retransmit_head(tcp_conn_t* c)
{
	c->rtt_timing = false;
	c->retransmits++;
	kTcpStats.retransmits++;
	if (c->state == TCP_SYN_SENT)
	{
		tcp_send_segment(c, c->snd_una, TCP_SYN, NULL, 0, true);
		return;
	}
	uint32_t in_flight = tcp_bytes_in_flight(c);
	if (in_flight == 0)
	{
		tcp_send_segment(c, c->snd_una, TCP_FIN | TCP_ACK, NULL, 0, false);
		return;
	}
	uint32_t len = in_flight < c->snd_mss ? in_flight : c->snd_mss;
	// The FIN rode the head only if the head IS the last segment out.
	tcp_send_data_segment(c, 0, len, c->snd_fin_sent && len == in_flight);
}

// Loss has been detected, one way or the other: RFC 5681 §3.1's response.
// Half of what was in flight is the new threshold (never under two
// segments), and cwnd falls to `to` — one segment after a timeout (the
// network may be badly congested, and slow start is the way to find out),
// the threshold plus the three segments the duplicate acks proved gone
// after a fast retransmit (§3.2 — the pipe is still flowing).
static void tcp_congestion_loss(tcp_conn_t* c, bool timeout)
{
	uint32_t flight = tcp_bytes_in_flight(c);
	uint32_t half = flight / 2;
	uint32_t floor = 2u * c->snd_mss;
	c->ssthresh = half > floor ? half : floor;
	c->cwnd = timeout ? c->snd_mss : c->ssthresh + 3u * c->snd_mss;
}

// cwnd grows on acknowledged new data (RFC 5681 §3.1): by what was acked
// in slow start — doubling per round trip — and by one segment per window
// in congestion avoidance. Capped at the ring, past which a larger window
// could carry nothing.
static void tcp_congestion_grow(tcp_conn_t* c, uint32_t acked)
{
	if (c->cwnd < c->ssthresh)
		c->cwnd += acked < c->snd_mss ? acked : c->snd_mss;
	else
	{
		uint32_t step = (c->snd_mss * c->snd_mss) / c->cwnd;
		c->cwnd += step > 0 ? step : 1;
	}
	if (c->cwnd > TCP_SND_BUF)
		c->cwnd = TCP_SND_BUF;
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

// ── 2b. REASSEMBLY: the ring is the queue ───────────────────────────────────
//
// A segment ahead of rcv_nxt used to be dropped — legal, and priced by the
// chaos rig at 29 seconds for a 100KB fetch under 30% reordering against 2
// seconds clean, because every early segment was thrown away and sent again
// after the hole it followed. 4.2BSD's receiver (1983) held such segments
// and so does this one, with no queue of its own: the receive ring's free
// space IS the window we advertise, so a segment inside the window has a
// slot in the ring already, at offset seq - rcv_nxt past the in-order bytes.
// That offset is stable — a read moves rcv_head and rcv_count by the same
// amount, and an in-order arrival moves rcv_count and rcv_nxt by the same
// amount, so (rcv_head + rcv_count) always points at where byte rcv_nxt
// belongs. The bytes go into their slot; the list remembers which slots are
// full; and when the gap closes, rcv_count simply grows over them.
//
// An in-order arrival that reaches into a held range overwrites it with
// the same bytes: the peer's stream is one stream, and a sequence number
// names one byte of it.

// Keep an ahead-of-sequence segment: write it into its slot and record the
// range, merging with anything it touches. Everything the caller advertised
// is honoured and nothing more — bytes past the window are not ours to
// hold, and a segment for which no slot is free is dropped, counted.
static void tcp_hold(tcp_conn_t* c, uint32_t seq, const uint8_t* data, uint32_t len)
{
	uint32_t room = TCP_RCV_BUF - c->rcv_count;    // exactly what tcp_window offers, before its SWS rounding
	uint32_t wnd_end = c->rcv_nxt + room;
	if (!seq_lt(seq, wnd_end))
	{
		c->out_of_order_dropped++;
		kTcpStats.out_of_order_dropped++;
		return;
	}
	if (seq_gt(seq + len, wnd_end))
		len = wnd_end - seq;                        // the tail beyond our window is the sender's problem
	uint32_t end = seq + len;

	// Where the new range lands in the sorted, disjoint list: `first` is the
	// first range that ends at or after it starts, `last` the first that
	// starts after it ends; everything in [first, last) merges with it into
	// [lo, hi). THE SEGMENT'S OWN seq IS WHAT PLACES ITS BYTES — the merged
	// range may start earlier, at bytes some previous segment already put
	// there, and writing this segment from THAT start shifted it back over
	// them (found by the rig's CRC on the first run, 2026-09-04).
	uint32_t lo = seq, hi = end;
	uint8_t first = 0;
	while (first < c->held_count && seq_lt(c->held[first].seq + c->held[first].len, seq))
		first++;
	uint8_t last = first;
	while (last < c->held_count && seq_leq(c->held[last].seq, end))
	{
		if (seq_lt(c->held[last].seq, lo))
			lo = c->held[last].seq;
		if (seq_gt(c->held[last].seq + c->held[last].len, hi))
			hi = c->held[last].seq + c->held[last].len;
		last++;
	}
	if (first == last && c->held_count == TCP_HELD_MAX)
	{
		c->out_of_order_dropped++;
		kTcpStats.out_of_order_dropped++;
		return;
	}

	// Into the ring, at the offset the sequence number dictates. Done after
	// the slot check so a refused segment leaves no bytes behind.
	uint32_t base = (c->rcv_head + c->rcv_count + (seq - c->rcv_nxt)) % TCP_RCV_BUF;
	for (uint32_t i = 0; i < len; i++)
		c->rcv_buf[(base + i) % TCP_RCV_BUF] = data[i];

	// Replace [first, last) with the merged range.
	uint8_t drop = (uint8_t)(last - first);
	if (drop == 0)
	{
		for (uint8_t i = c->held_count; i > first; i--)
			c->held[i] = c->held[i - 1];
		c->held_count++;
	}
	else if (drop > 1)
	{
		for (uint8_t i = first + 1; i + drop - 1 < c->held_count; i++)
			c->held[i] = c->held[i + drop - 1];
		c->held_count -= (uint8_t)(drop - 1);
	}
	c->held[first].seq = lo;
	c->held[first].len = hi - lo;
	c->out_of_order_held++;
	kTcpStats.out_of_order_held++;
}

// Their FIN, at exactly rcv_nxt: they will send no more. The stream stays
// readable until the buffer drains — THEN read() returns 0.
static void tcp_take_fin(tcp_conn_t* c)
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
		// The normal end of a client connection: both sides have said
		// goodbye. TIME_WAIT now guards the port — see tcp_poll for why
		// that wait is not paranoia.
		c->state = TCP_TIME_WAIT;
		c->time_wait_until = kTicksSinceStart + 2 * TCP_MSL_TICKS;
	}
	printd(DEBUG_NET, "tcp: peer %u.%u.%u.%u:%u closed its half\n",
	       NET_IPV4_OCTETS(c->peer_ip), c->peer_port);
}

// rcv_nxt moved: absorb every held range it now reaches, in order, and the
// held FIN if the stream has arrived at it. The bytes are already in place,
// so absorbing a range is arithmetic on rcv_count and rcv_nxt.
static void tcp_absorb_held(tcp_conn_t* c)
{
	while (c->held_count > 0 && seq_leq(c->held[0].seq, c->rcv_nxt))
	{
		uint32_t end = c->held[0].seq + c->held[0].len;
		if (seq_gt(end, c->rcv_nxt))
		{
			uint32_t more = end - c->rcv_nxt;
			uint32_t room = TCP_RCV_BUF - c->rcv_count;
			if (more > room)
			{
				// Cannot happen: a held range was inside the window when it
				// was held, and the window only moves forward with rcv_nxt.
				// Said loudly rather than trusted silently.
				printd(DEBUG_NET, "tcp: held range past the ring (more=%u room=%u) — truncated\n", more, room);
				more = room;
			}
			c->rcv_count += more;
			c->rcv_nxt += more;
			c->rx_bytes += more;
		}
		for (uint8_t i = 1; i < c->held_count; i++)
			c->held[i - 1] = c->held[i];
		c->held_count--;
	}
	if (c->held_fin && c->rcv_nxt == c->held_fin_seq)
	{
		c->held_fin = false;
		tcp_take_fin(c);
	}
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
	// Judged against snd_max, not snd_nxt: after a timeout pulls snd_nxt
	// back to the head, the peer may acknowledge anything up to what it
	// had already received — everything between is ours, sent once.
	if (synchronized && (flags & TCP_ACK) && seq_gt(ack, c->snd_max))
	{
		printd(DEBUG_NET, "tcp: ACK 0x%x for bytes we have not sent (snd_max 0x%x) — dropped\n",
		       ack, c->snd_max);
		tcp_ack(c);
		spinlock_release_irqrestore(&c->lock, irqflags);
		return;
	}

	// The peer's window, taken from any segment whose ack is current
	// (RFC 793's SND.WL check pared down to what is reachable here: an ack
	// behind snd_una is a straggler, and its window is a stale one). A
	// window that was ZERO and is not any more, with something unacked
	// that THIS segment does not acknowledge, is the reopening the persist
	// probe has been asking about with the probe dropped at the peer's
	// door — so the head is resent now rather than when the backed-off
	// timer next fires. An ack that advances took the probe: the ack path
	// below retires it and sends into the new window, and resending here
	// would count a retransmit and cancel a sample for a byte that landed.
	uint16_t old_wnd = c->snd_wnd;
	if (!(flags & TCP_ACK) || seq_geq(ack, c->snd_una))
		c->snd_wnd = win;
	if (synchronized && old_wnd == 0 && c->snd_wnd > 0 && tcp_unacked(c) && !c->stripped &&
	    !((flags & TCP_ACK) && seq_gt(ack, c->snd_una)))
	{
		tcp_retransmit_head(c);
		tcp_rto_arm(c);
	}

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
				c->snd_una = ack;        // our SYN is acknowledged, which disarms the timer
				if (c->rtt_timing)       // and the handshake is the first round trip measured (not if the SYN was resent — Karn)
				{
					c->rtt_timing = false;
					tcp_rtt_sample(c, (uint32_t)(kTicksSinceStart - c->snd_sent_at));
				}
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
				c->cwnd = TCP_INIT_CWND_SEGMENTS * (uint32_t)c->snd_mss;
				c->ssthresh = TCP_SND_BUF;  // slow start all the way up, until loss says otherwise

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
			// (a) Their acknowledgement. NEW GROUND — an ack past snd_una —
			// retires ring bytes: the head moves up, the clock restarts if
			// anything is still out and stops if not (advancing snd_una is
			// the whole of the disarm, tcp_unacked), the stopwatch samples
			// if this ack covers the segment it was timing, cwnd grows, and
			// progress is proof of life so the spent retries do not count
			// against what remains. An ack that only REPEATS snd_una, with
			// data of ours still out, carrying nothing and moving no
			// window, is the peer saying a segment landed past a hole
			// (RFC 5681 §2's definition of a duplicate): three of them
			// cannot be reordering, so the head is resent without waiting
			// for the timer, and each one after that means one more
			// segment left the network, so cwnd inflates by one to keep
			// the pipe full until the new ack deflates it (§3.2) — unless
			// that new ack stops SHORT of where the window stood when
			// recovery began, which is a second hole (tcp.h `recover`). A
			// repeated ack under a ZERO window is not a duplicate — it is
			// the peer answering a probe, and a probe answered is the
			// window still shut, not a loss.
			if ((flags & TCP_ACK) && seq_gt(ack, c->snd_una))
			{
				uint32_t acked = ack - c->snd_una;
				uint32_t ring_acked = acked;
				// The only sequence number past the ring's bytes is the
				// FIN's, so an ack that reaches past them acknowledges the
				// FIN — whether it went out just now, or before a timeout
				// pulled snd_nxt back and forgot that it had (the guard
				// above admits nothing past snd_max).
				bool fin_acked = false;
				if (acked > c->snd_count)
				{
					ring_acked = c->snd_count;
					fin_acked = true;
					c->snd_fin_sent = true;
				}
				c->snd_head = (c->snd_head + ring_acked) % TCP_SND_BUF;
				c->snd_count -= ring_acked;
				c->snd_una = ack;
				if (seq_lt(c->snd_nxt, c->snd_una))
					c->snd_nxt = c->snd_una;         // a pulled-back snd_nxt skips what the peer already holds
				// Progress ends the backoff (4.4BSD's t_rxtshift = 0): the
				// peer is answering, so the next timeout is the estimator's
				// again, not the doubled one. Karn's rule is about SAMPLING
				// and stays — a resent segment's ack still teaches nothing.
				// Kept until a clean sample, the doubled timer sat at 3.7s
				// on the rig's delay+loss leg and charged a tail loss all of
				// it: a windowed sender under loss rarely goes a whole
				// window without a resend, so the sample it waited for
				// rarely came. The retry budget refills on the same
				// evidence — it is what the exhaustion branch spends.
				c->rto_backed_off = false;
				c->retries = 0;
				if (c->rtt_timing && seq_geq(ack, c->rtt_seq))
				{
					c->rtt_timing = false;
					tcp_rtt_sample(c, (uint32_t)(kTicksSinceStart - c->snd_sent_at));
				}
				if (c->dup_acks >= 3 && seq_lt(ack, c->recover))
				{
					// A PARTIAL ack in fast recovery (RFC 6582 §3.2 step 5):
					// the hole just filled had another behind it, and the
					// peer has nothing left to provoke three more duplicates
					// with. Resend the next hole now, restart the clock, take
					// what was acked out of the inflated window and give one
					// segment back — recovery goes on until `recover` is
					// reached.
					c->cwnd = c->cwnd > acked ? c->cwnd - acked : 0;
					c->cwnd += c->snd_mss;
					tcp_retransmit_head(c);
					tcp_rto_arm(c);
				}
				else
				{
					if (c->dup_acks >= 3)
						c->cwnd = c->ssthresh;       // fast recovery ends: deflate to the threshold
					else
						tcp_congestion_grow(c, acked);
					c->dup_acks = 0;
					if (tcp_unacked(c))
						tcp_rto_arm(c);
				}
				// Our FIN being acknowledged is a state transition, not
				// just bookkeeping — and only the FIN's own ack is: the
				// FIN may still be queued behind data this ack retired.
				if (fin_acked)
				{
					if (c->state == TCP_FIN_WAIT_1)
						c->state = TCP_FIN_WAIT_2;
					else if (c->state == TCP_CLOSING)
					{
						c->state = TCP_TIME_WAIT;
						c->time_wait_until = kTicksSinceStart + 2 * TCP_MSL_TICKS;
					}
					else if (c->state == TCP_LAST_ACK)
						c->state = TCP_CLOSED;   // fully closed; poll reaps it
				}
			}
			else if ((flags & TCP_ACK) && ack == c->snd_una && tcp_unacked(c) &&
			         data_len == 0 && !(flags & (TCP_SYN | TCP_FIN)) &&
			         win == old_wnd && c->snd_wnd != 0)
			{
				c->dup_acks++;
				if (c->dup_acks == 3 && !seq_gt(c->snd_una, c->recover))
				{
					// Duplicates for data BELOW the last recovery mark — the
					// go-back-N resends after a timeout, which the peer
					// largely already holds, each draw one — are not a
					// loss signal (RFC 6582 §3.2 step 1B). Count them
					// down again and wait for the timer or real progress.
					c->dup_acks = 0;
				}
				else if (c->dup_acks == 3)
				{
					c->recover = c->snd_nxt;         // recovery ends when the peer reaches here
					tcp_congestion_loss(c, false);
					tcp_retransmit_head(c);
					kTcpStats.fast_retransmits++;
					printd(DEBUG_NET, "tcp: fast retransmit to %u.%u.%u.%u:%u (seq 0x%x, cwnd %u)\n",
					       NET_IPV4_OCTETS(c->peer_ip), c->peer_port, c->snd_una, c->cwnd);
				}
				else if (c->dup_acks > 3 && c->cwnd < TCP_SND_BUF)
					c->cwnd += c->snd_mss;
			}

			// (b) Their data. In order, it goes into the ring and rcv_nxt
			// advances over it and over whatever held ranges it now reaches;
			// ahead of us, it is held in its slot (tcp_hold); behind us, it
			// is a duplicate. A segment that starts behind us and reaches
			// past us is a retransmission overlapping bytes we took — a
			// sender resends from its oldest unacknowledged byte, which
			// may sit inside a segment we took part of, and RFC 793 §3.9
			// says trim — so its tail is in-order data.
			if (data_len && seq_lt(seq, c->rcv_nxt) && seq_gt(seq + data_len, c->rcv_nxt))
			{
				uint32_t old = c->rcv_nxt - seq;
				data += old;
				data_len = (uint16_t)(data_len - old);
				seq = c->rcv_nxt;
			}
			if (data_len)
			{
				if (seq == c->rcv_nxt)
				{
					uint32_t room = TCP_RCV_BUF - c->rcv_count;   // up to the whole buffer, past 16 bits
					uint16_t take = data_len < room ? data_len : room;
					tcp_rcv_store(c, data, take);
					c->rcv_nxt += take;
					tcp_absorb_held(c);
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
					// us to take that back. held is the ranges still
					// waiting past a hole.
					printd(DEBUG_NET, "TCPRX t=%lu len=%u room=%u took=%u buf=%u win=%u held=%u\n",
					       kTicksSinceStart, data_len, room, take,
					       c->rcv_count, tcp_window(c), c->held_count);
					tcp_ack(c);          // acknowledge immediately: simple,
					                     // and the peer's window depends on it
				}
				else if (seq_gt(seq, c->rcv_nxt))
				{
					// Ahead of us: something between here and the peer
					// reordered or dropped. Keep it, and re-announce where
					// we are — the sender's fast retransmit (RFC 5681 §3.2)
					// is cued by three of these, and that one resend is
					// what closes the hole over everything held behind it.
					tcp_hold(c, seq, data, data_len);
					tcp_ack(c);
				}
				else
				{
					// Behind us: bytes we already took, sent again because
					// our ack never reached them (or not in time). Not
					// reordering — a different weather report — and the
					// re-announce is the cure, which is why it is not the
					// reordering count (Codex, PR #46).
					kTcpStats.duplicates_dropped++;
					tcp_ack(c);
				}
			}

			// (c) Their FIN occupies the sequence number after their last
			// byte. Taken only when the stream has ARRIVED there: a FIN
			// riding a segment whose bytes were not all taken (the ring was
			// full), or one ahead of a hole, waits for rcv_nxt like any
			// other byte — taking it early would step over data still owed.
			// One already taken and sent again (our ack was lost) is
			// re-acknowledged, not re-taken.
			if (flags & TCP_FIN)
			{
				uint32_t fin_seq = seq + data_len;
				if (fin_seq == c->rcv_nxt)
					tcp_take_fin(c);
				else if (seq_gt(fin_seq, c->rcv_nxt))
				{
					c->held_fin = true;
					c->held_fin_seq = fin_seq;
					tcp_ack(c);          // a bare FIN ahead of a hole still earns the re-announce
				}
				else
					tcp_ack(c);
			}

			// (d) Whatever this segment made room for — acked bytes, a
			// wider window — goes out now. tcp_output refuses by state if
			// the FIN above ended the conversation.
			tcp_output(c, false);
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
	c->snd_count = 0;
	c->snd_head = 0;
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
		// display-only whatever a death left unacknowledged in the ring: a
		// listed corpse must not retransmit it.
		if (tcp_unacked(c) && c->state != TCP_TIME_WAIT && c->state != TCP_CLOSED &&
		    now >= c->rto_deadline)
		{
			// A peer holding a ZERO window is not gone: what is out there
			// is the probe byte, the wait is the peer's reader and not the
			// network, and a persist wait has no death in it (tcp.h
			// TCP_PERSIST_MIN_TICKS). The budget is spent only on a window
			// that was open.
			if (++c->retries > TCP_MAX_RETRIES && c->snd_wnd != 0)
			{
				// No connect_timeouts count here, and that is not an
				// omission: dial's own 10-second deadline gives up on a
				// silent handshake long before a SYN's retries could
				// exhaust (their backoff sums past 30 seconds), and dial
				// counts that death itself. What dies HERE is an
				// established conversation whose peer stopped answering
				// — or a detached close whose FIN nobody will ever
				// acknowledge — and the caller hears it as a reset.
				c->reset = true;         // the peer is gone; give up honestly
				c->state = TCP_CLOSED;
			}
			else
			{
				if (c->retries > TCP_MAX_RETRIES)
					c->retries = TCP_MAX_RETRIES;
				c->rto_ticks *= 2;
				if (c->rto_ticks > TCP_RTO_MAX_TICKS)
					c->rto_ticks = TCP_RTO_MAX_TICKS;
				c->rto_backed_off = true;   // stays doubled until the peer acknowledges new data
				// A timeout is loss found the slow way, and the network's
				// state is unknown: back to one segment and slow-start up.
				// It also ENDS any fast recovery in progress — with the
				// duplicate count still standing, the next new ack would be
				// read as a partial ack of a recovery that the timeout has
				// already superseded.
				tcp_congestion_loss(c, true);
				c->dup_acks = 0;
				c->recover = c->snd_max;    // no fast retransmit until the peer is past everything sent before this (RFC 6582 §3.2)
				c->rtt_timing = false;      // Karn: nothing sent from here is a sample
				if (c->state == TCP_SYN_SENT)
				{
					c->rto_deadline = now + c->rto_ticks;
					tcp_send_segment(c, c->snd_una, TCP_SYN, NULL, 0, true);
					c->retransmits++;
					kTcpStats.retransmits++;
				}
				else
				{
					// GO BACK TO THE HEAD (4.4BSD's snd_nxt = snd_una): the
					// timer knows only that the head is gone, and a timeout
					// is the one cue that gives no hint about the rest —
					// so everything outstanding is sent again, in order,
					// paced by slow start from one segment, and the acks
					// that come back say what actually arrived. Resending
					// the head alone and waiting to hear about the rest
					// costs one backed-off timeout per further hole
					// (measured on the rig: 0.5s, then 1.4s, for two holes
					// behind one timeout). tcp_output arms the clock with
					// the backed-off timer as it sends; the persist takes
					// over if the window is shut.
					c->snd_nxt = c->snd_una;
					c->snd_fin_sent = false;
					tcp_output(c, false);
				}
				printd(DEBUG_NET, "tcp: timeout #%u to %u.%u.%u.%u:%u (rto %u ticks, cwnd %u, %u bytes to resend)\n",
				       (uint32_t)c->retries, NET_IPV4_OCTETS(c->peer_ip),
				       c->peer_port, c->rto_ticks, c->cwnd, c->snd_count);
			}
		}

		// The persist probe (tcp_output arms the clock; tcp.h says why it
		// exists): ask a peer that said "stop" whether it still means it.
		// Re-checked under the lock — an ack or a write may have changed
		// the picture since the clock was set, and tcp_output re-arms it
		// if the answer is still "wait".
		if (c->persist_deadline != 0 && now >= c->persist_deadline)
		{
			c->persist_deadline = 0;
			if (tcp_may_send_data(c) && c->snd_wnd == 0 && !tcp_unacked(c) && c->snd_count > 0)
			{
				kTcpStats.window_probes++;
				printd(DEBUG_NET, "tcp: zero-window probe to %u.%u.%u.%u:%u (%u bytes waiting)\n",
				       NET_IPV4_OCTETS(c->peer_ip), c->peer_port, c->snd_count);
				tcp_output(c, true);
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
		// wakes for bytes, for EOF, or for death; a writer wakes when an
		// ack has made room in the ring, or for death. (Same doctrine as
		// pipes — a wake is a hint, never a promise, and both sleepers
		// re-test after waking.)
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
		    (c->snd_count < TCP_SND_BUF || c->reset || c->state == TCP_CLOSED))
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
	c->snd_buf = tcp_heap_alloc_locked(TCP_SND_BUF);
	c->dev = dev;
	c->peer_ip = peer_ip;
	c->peer_port = peer_port;
	c->local_port = local_port;
	c->snd_mss = 536;
	c->snd_wnd = TCP_MSS;
	c->cwnd = TCP_INIT_CWND_SEGMENTS * TCP_MSS;   // resized to the peer's MSS at establishment
	c->ssthresh = TCP_SND_BUF;
	c->rto = TCP_RTO_INITIAL_TICKS;

	uint32_t iss = tcp_initial_seq(peer_ip, peer_port, c->local_port);
	c->snd_una = iss;
	c->snd_nxt = iss;
	c->snd_max = iss;
	c->recover = iss;      // RFC 6582's starting point: below it, no recovery has ever run
	c->state = TCP_SYN_SENT;

	c->next = kTcpConnList;
	c->prev = NULL;
	if (kTcpConnList != NULL)
		kTcpConnList->prev = c;
	kTcpConnList = c;
	spinlock_release_irqrestore(&kTcpListLock, lf);

	printd(DEBUG_NET, "tcp: dialing %u.%u.%u.%u:%u from port %u (iss 0x%x)\n",
	       NET_IPV4_OCTETS(peer_ip), peer_port, c->local_port, iss);

	// The opening SYN. It arms the same timer that protects data, so a
	// lost SYN is resent like anything else.
	uint64_t irqflags = spinlock_acquire_irqsave(&c->lock);
	tcp_send_syn(c);
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
		if (c->reset || !(c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT))
		{
			spinlock_release_irqrestore(&c->lock, irqflags);
			return sent ? (long)sent : TCP_ERR_RESET;
		}

		// Into the ring: as much as fits, and it is on its way the moment
		// tcp_output sees it. QUEUED is the promise a write makes — not
		// acknowledged: the ring, the ack clock and the timer own the bytes
		// from here, and close() will not send its FIN ahead of them. The
		// ring may wrap under the copy.
		uint32_t room = TCP_SND_BUF - c->snd_count;
		if (room > 0)
		{
			uint32_t n = (len - sent) < room ? (uint32_t)(len - sent) : room;
			uint32_t at = (c->snd_head + c->snd_count) % TCP_SND_BUF;
			uint32_t first = TCP_SND_BUF - at;
			if (first > n)
				first = n;
			memcpy(c->snd_buf + at, (void*)(src + sent), first);
			if (n > first)
				memcpy(c->snd_buf, (void*)(src + sent + first), n - first);
			c->snd_count += n;
			sent += n;
			tcp_output(c, false);
			spinlock_release_irqrestore(&c->lock, irqflags);
			continue;
		}

		// The ring is full — a whole window ahead of the peer. Park until an
		// ack makes room (tcp_wake_if_ready's condition) or the backstop
		// re-checks. Registered UNDER THE SAME LOCK as the room check: a gap
		// between "no room" and "registered" is where the ack that frees
		// the ring lands unseen, and the park then runs to its backstop
		// for a wait that was already over.
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
		// The FIN queues BEHIND whatever the ring still holds: a write
		// returns when its bytes are queued, so a close right behind it
		// must not say "no more" ahead of them. tcp_output sends the FIN
		// now if the ring is drained, else on or after the last segment,
		// and the ack clock keeps draining the ring after the handle is
		// gone — the state changes here, the wire catches up.
		c->snd_fin = true;
		c->state = they_finished ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
		tcp_output(c, false);
	}

	// Detach, never block: close() returns immediately and the poll
	// finishes the closing dance and the TIME_WAIT nap in the background.
	// A program should not wait 30 seconds for protocol politeness.
	c->detached = true;
	c->reader = NULL;
	c->writer = NULL;
	spinlock_release_irqrestore(&c->lock, irqflags);
}
