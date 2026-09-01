#ifndef TCP_H
#define TCP_H

// tcp.h — Transmission Control Protocol (RFC 793, September 1981).
//
// The boss fight, and the reason this arc was chosen. Everything below TCP
// moves messages that may vanish, duplicate, or arrive out of order; TCP's
// job is to build, on top of that, the illusion every program actually
// wants: a RELIABLE ORDERED BYTE STREAM. Two machines that have never met
// agree on where the stream starts, what has arrived, and when it ends —
// using nothing but numbered packets and timers. Cerf and Kahn's 1974
// design (TCP and IP were one protocol then; the 1978 split into TCP-over-IP
// is why the layering below exists at all) has survived 45 years of the most
// adversarial deployment environment in computing, mostly unchanged.
//
// THE THREE HARD PARTS, and where each lives:
//
//   1. THE STATE MACHINE (eleven states, tcp.c's tcp_input). A connection
//      is a shared understanding, so both sides must agree on which stage
//      of the conversation they are in — while the only evidence either
//      has is packets that may be lost. Opening costs three packets, and
//      closing costs four, because each direction of the stream must be
//      shut down independently (one side can stop talking and keep
//      listening — the `cat file | ssh host` shape).
//
//   2. SEQUENCE ARITHMETIC (the seq_* helpers below). Every byte of the
//      stream has a 32-bit number, and those numbers WRAP. "Is this byte
//      newer?" can therefore never be `a > b` — it must be modular
//      comparison, which is the single most reliable source of bugs in
//      every TCP ever written, including the ones in production today.
//
//   3. RETRANSMISSION (the RTO timer, tcp_poll). Nothing else makes the
//      stream reliable: send, and if no acknowledgement arrives in time,
//      send again — backing off, because the network may be congested
//      precisely BECAUSE everyone is retransmitting (the 1986 congestion
//      collapse, and Van Jacobson's answer to it).
//
// V1 SCOPE, stated plainly so the omissions are decisions:
//   - ACTIVE open only (we dial out). Listeners — ruling #3's "read a
//     listener handle to accept" — are the next slice, and the state
//     machine below already carries their states.
//   - STOP-AND-WAIT sending: one unacknowledged segment at a time. This
//     is honest for what os64 sends today (an HTTP request is one small
//     segment) and it costs nothing on the RECEIVE side, which is where
//     a page fetch's bytes actually flow. A real send window is a booked
//     DEBT, not a pretense.
//   - Out-of-order arrivals are DROPPED (the sender will resend them in
//     order). Legal — RFC 793 permits a receiver to discard anything it
//     cannot use — and it means no reassembly queue in v1. Booked.
//   - Congestion control: fixed window. Also booked. os64 on slirp is
//     not going to melt the internet, but silence would still be a lie.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thread.h"
#include "spinlock.h"
#include "driver/net/net_device.h"

#define TCP_HDR_MIN  20

// Header flags (the low six bits of the data-offset/flags word).
#define TCP_FIN  0x01   // "I have no more data to send" — half-close
#define TCP_SYN  0x02   // "synchronize sequence numbers" — the opener
#define TCP_RST  0x04   // "this connection is nonsense; forget it"
#define TCP_PSH  0x08   // "deliver what you have" — advisory, we honor by always delivering
#define TCP_ACK  0x10   // "the acknowledgement field means something"

// The eleven states of RFC 793's diagram. CLOSED and LISTEN are the two
// that aren't really connections; TIME_WAIT is the one everybody forgets
// and then wonders why their server can't rebind a port.
typedef enum tcp_state
{
	TCP_CLOSED = 0,
	TCP_LISTEN,        // (next slice: the listener handle, ruling #3)
	TCP_SYN_SENT,      // we dialed; waiting for SYN+ACK
	TCP_SYN_RECEIVED,  // (next slice)
	TCP_ESTABLISHED,   // the stream is open in both directions
	TCP_FIN_WAIT_1,    // we closed; waiting for our FIN to be acknowledged
	TCP_FIN_WAIT_2,    //   ...acknowledged; waiting for THEIR FIN
	TCP_CLOSE_WAIT,    // they closed; we may still send (half-close)
	TCP_CLOSING,       // both closed simultaneously — the rare corner
	TCP_LAST_ACK,      // we closed after them; waiting for the last ACK
	TCP_TIME_WAIT,     // the wait that protects the NEXT connection (see tcp.c)
} tcp_state_t;

// Buffers. The receive buffer IS the advertised window, so it sets how much
// the peer may have in flight toward us — and since the NICs are drained
// once per scheduler pass, it sets the THROUGHPUT CEILING: window per pass.
// 64KB is the most a 16-bit window field can say (RFC 1323 scaling would
// be the next step), 64KB per 10ms pass is ~6.4MB/s, and it costs 64KB of
// kmalloc per open connection. (8KB here measured 366KB/s: the window
// emptied inside every pass — five segments, then win=0 — and the sender
// idled for the rest of each one.) MSS 1460 = the classic ethernet number:
// 1500 MTU - 20 IP - 20 TCP, the reason so much of the internet's traffic
// arrives in 1460-byte pieces.
#define TCP_RCV_BUF  65536
#define TCP_MSS      1460

// Timers, in ticks (100/s). Fixed RTO with exponential backoff — a real
// stack measures round-trip time and adapts (Jacobson/Karels, 1988); ours
// is booked as a DEBT and stated here so nobody mistakes 1 second for
// measurement. TIME_WAIT is 2×MSL, and MSL ("maximum segment lifetime")
// is the assumption that no packet outlives 15 seconds on this network —
// RFC 793 suggests 2 minutes for the internet at large; a virtual wire and
// a home LAN are not the 1981 ARPAnet, and 30 seconds of TIME_WAIT is
// already deep paranoia by our standards.
#define TCP_RTO_TICKS       (1 * TICKS_PER_SECOND)
#define TCP_RTO_MAX_TICKS   (8 * TICKS_PER_SECOND)
#define TCP_MAX_RETRIES     6
#define TCP_MSL_TICKS       (15 * TICKS_PER_SECOND)
#define TCP_CONNECT_TIMEOUT (10 * TICKS_PER_SECOND)

// The morgue: how long a CLOSED, detached connection stays listed before
// the poll reaps it. DISPLAY policy, not protocol — TIME_WAIT's linger is
// TCP's own law (see tcp_poll), this one exists so /sys/net/tcp can answer
// "what just happened" after a dial fails or a fetch ends. A human who ran
// a command that died gets this long to cat the aftermath.
//
// A conversation that ENDED lies in state: its buffers and its port stay
// its own until the reap, the port because TIME_WAIT's protection is
// exactly a port held past the last segment. A dial that FAILED is a
// tombstone (tcp_dial_entomb): the row, and nothing else — buffers freed
// and port returned the moment dial gave up, because no conversation
// happened and there is nothing for a held port to protect. Tombstones
// are CAPPED, oldest evicted: a refused dial returns at once and costs
// no handle, so a loop of them is the one way to fill the morgue faster
// than it drains, and a capful of rows says "refused, again" as well
// as sixty thousand would.
#define TCP_MORGUE_TICKS      (15 * TICKS_PER_SECOND)
#define TCP_MORGUE_TOMBSTONES 64

// In-band sentinels (the pipe.c convention).
#define TCP_ERR_INTERRUPTED (-3L)   // a signal ended the wait (signal_park_must_end)
#define TCP_ERR_RESET       (-4L)   // peer sent RST, or the connection died
#define TCP_ERR_TIMEOUT     (-5L)   // caller's read deadline expired, no bytes

typedef struct tcp_conn
{
	tcp_state_t state;
	net_device_t* dev;
	uint32_t peer_ip;
	uint16_t peer_port, local_port;

	// ── SEND state (RFC 793's SND.* variables, same names on purpose) ──
	uint32_t snd_una;      // oldest byte sent but not yet acknowledged
	uint32_t snd_nxt;      // next sequence number we will send
	uint16_t snd_wnd;      // the peer's advertised receive window
	uint16_t snd_mss;      // the peer's advertised MSS (or 536, the RFC floor)

	// The retransmit slot: v1 holds ONE unacknowledged segment. Its
	// presence (snd_len > 0) is what arms the RTO timer.
	uint8_t* snd_buf;
	uint32_t snd_len;
	uint32_t snd_seq;      // sequence number of snd_buf[0]
	uint8_t  snd_flags;    // the FIN/SYN riding this segment, if any
	uint64_t rto_deadline; // kTicksSinceStart when we resend
	uint32_t rto_ticks;    // current timeout (doubles on each retry)
	uint8_t  retries;

	// ── RECEIVE state (RCV.*) ──
	uint32_t rcv_nxt;      // next sequence number we expect — the ACK we send
	uint8_t* rcv_buf;      // the byte STREAM ring (not datagrams: TCP is bytes)
	uint32_t rcv_head, rcv_count;
	bool     rcv_fin;      // their FIN arrived: after the buffer drains, EOF
	bool     zero_window;  // we advertised 0; owe a window update when drained

	bool     reset;        // RST received or fatal error: reads/writes fail
	uint64_t time_wait_until;
	uint64_t closed_at;    // the morgue clock: when the poll first saw this
	                       // conn CLOSED and detached (0 = not yet)

	spinlock_t lock;
	thread_t* volatile reader;   // parked in tcp_conn_read
	thread_t* volatile writer;   // parked in tcp_conn_write
	bool detached;               // handle closed; poll may reap after TIME_WAIT
	bool tombstone;              // a failed dial's row: buffers freed, port returned,
	                             // owns nothing the reap must give back (tcp_dial_entomb)

	uint64_t rx_bytes, tx_bytes, retransmits, out_of_order_dropped;

	struct tcp_conn* next;
} tcp_conn_t;

typedef struct tcp_stats
{
	uint64_t connections_opened;
	uint64_t connections_refused;   // RST to our SYN — nobody listening
	uint64_t connect_timeouts;
	// Funerals: connections unlisted and freed — by the poll's reap, or
	// by the tombstone cap's eviction.
	// It answers "is the reaper alive?" while corpses linger in
	// /sys/net/tcp — and it marks that real kernel heap (a 64KB receive
	// ring per corpse that had a conversation; a tombstone's went at the
	// failed dial) was freed with no task burial to account for it, on
	// the morgue's clock rather than anybody's syscall. Anything
	// auditing memory across a window (task_teardown_leak brackets on
	// this) has to be able to see that one landed inside it.
	uint64_t connections_reaped;
	uint64_t segments_in, segments_out, retransmits;
	uint64_t bad_checksum;
	uint64_t no_connection;         // segment for a 4-tuple we don't know (we RST it)
	uint64_t resets_received;
	// The machine-wide twin of the per-conn counter, because a reaped
	// connection takes its copy to the grave: "has this stack EVER seen
	// reordering" has to survive the connections that answered it.
	uint64_t out_of_order_dropped;
	// Bytes we already took, sent again: OUR ack was lost or late, and the
	// peer resent from where it last heard from us. Not reordering — kept
	// apart so out_of_order_dropped answers only "is the network
	// reordering", the question the reassembly debt hangs on, while this
	// one answers "is the network eating our acks".
	uint64_t duplicates_dropped;
} tcp_stats_t;
extern tcp_stats_t kTcpStats;

// The connection list, exported for /sys/net/tcp. The discipline is
// tcp_poll's: hold kTcpListLock across the walk (it guards the links),
// take each conn's own lock to read its fields. Readers outside tcp.c
// observe; they never unlink, retire, or wake anything.
extern tcp_conn_t* kTcpConnList;
extern spinlock_t kTcpListLock;

// ── The API behind HANDLE_NET_TCP ───────────────────────────────────────────
// Active open. BLOCKS until the handshake completes (task context only);
// NULL on refusal, timeout, or no resources. `why` (may be NULL if the
// caller doesn't care) gets the specific OS64_NET_ERR_* on failure — a dial
// that fails three different ways owes the caller three different answers
// (REFUSED = RST, the door is closed; TIMEOUT = nobody answered at all;
// NO_RESOURCES = our side couldn't even pick up the phone).
tcp_conn_t* tcp_conn_dial(net_device_t* dev, uint32_t peer_ip, uint16_t peer_port,
                          int64_t* why);

// Stream read: blocks for at least one byte, returns SHORT (whatever is
// available — the filter contract every os64 read follows), 0 at EOF after
// the peer's FIN drains. Negative = interrupted, reset, or deadline expiry
// (`deadline` = absolute tick, 0 = forever; buffered bytes and EOF both
// outrank an expired clock).
long tcp_conn_read(tcp_conn_t* c, void* buf, size_t len, uint64_t deadline);

// Stream write: blocks until every byte is acknowledged (v1 stop-and-wait),
// returns the count written. Negative = interrupted or reset.
long tcp_conn_write(tcp_conn_t* c, const void* buf, size_t len);

// Orderly shutdown: sends FIN, detaches from the handle, and lets the poll
// finish the closing dance (and the TIME_WAIT nap) in the background — a
// close() must not block a program for 30 seconds of protocol politeness.
void tcp_conn_close(tcp_conn_t* c);

// Inbound segment (IP header stripped, addresses host-order). RX context.
void tcp_input(net_device_t* dev, uint32_t src_ip, uint32_t dst_ip,
               const void* seg, uint16_t length);

// Timers: retransmission, connect timeout, TIME_WAIT reaping. From
// processSignals beside dhcp_poll.
void tcp_poll(void);

// Level-triggered wake sweep, beside udp_conn_wake_if_ready.
void tcp_wake_if_ready(void);

#endif // TCP_H
