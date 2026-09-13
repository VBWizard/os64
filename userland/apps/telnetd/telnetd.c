// telnetd — the inbound shell (SERVERS.md § 3; RFC 854).
//
// Telnet is 1969, older than TCP itself, and it is the half of a remote
// login that is NOT cryptography: it moves bytes between a socket and a
// shell, and it proves the whole inbound seam — the TCP listener, the
// STREAM pty, spawn-with-a-connection — end to end before sshd (SSHD.md)
// replaces it on the very same seams. What it hands out is a shell to
// anyone who can reach the port, in the clear; the trust boundary is the
// LAN it sits on, and nothing else.
//
// THE 1986 SHAPE (inetd, and Bernstein's tcpserver after it): one process
// listens and accepts; each connection is a fresh CHILD whose stdin and
// stdout ARE the socket. os64's rule that a child gets exactly the handles
// it was handed is that model already, so telnetd is one binary with two
// modes:
//
//   telnetd [port]     the LISTENER: announce (23 by default), and for each
//                      connection spawn `telnetd -session` with handles 0
//                      and 1 = the connection.
//   telnetd -session   ONE session: create a STREAM pty, seat /bin/husk on
//                      it, and bridge — telnet in one direction, husk's
//                      bytes in the other. A crash here takes one session,
//                      never the listener or another login.
//
// THE NVT TRANSLATION (RFC 854), and where each half lives:
//   inbound  (client -> husk): telnet_receive decodes — CR LF / CR NUL to a
//            plain newline, IAC commands answered, NAWS to a pty resize.
//   outbound (husk -> client): a bare LF becomes CR LF and a 0xFF is
//            doubled, done inline here because it needs no negotiation state
//            and sharing the engine's queue across the two bridge threads
//            would be a race with no lock to hold.

#include <stdint.h>
#include <stddef.h>
#include "os64/os64.h"
#include "os64/pty.h"    // os64_pty_create_stream / os64_pty_resize — the STREAM slave
#include "../telnet/telnet_protocol.h"

// ── The one session this process serves ─────────────────────────────────────
// A -session process serves exactly one login, so these are its whole world.
// g_conn is handles 0 AND 1 (the connection, from the listener's spawn);
// g_master is the STREAM pty whose slave husk sits on.
//
// EXACTLY ONE THREAD WRITES g_conn: the outbound bridge. The kernel's
// per-connection lock stops two writers corrupting memory, but it does NOT
// make a write atomic — under backpressure tcp_conn_write copies what fits,
// drops the lock, and resumes, so a second writer could split an outbound
// doubled-IAC and produce an invalid Telnet stream (Codex #101 P2). So the
// inbound thread never touches g_conn; the negotiation replies it produces go
// through the mailbox below to the outbound thread, which serializes them
// with husk's bytes.
static int32_t g_conn = 1;
static int64_t g_master = -1;
static volatile int g_session_over = 0;
// Set by the inbound thread when the session must END ON OUR SAY-SO with a
// last word to the client (a client that refused our echo). Published ONLY
// once that word is entirely in the mailbox — the engine's queue drained to
// the last byte — with release ordering, and read with acquire by the
// outbound thread, the sole socket writer, which then drains the mailbox one
// final time and ends. So the word reaches the wire before the FIN does, and
// no interleaving can show the flag with the word still in the engine
// (Codex #101 rd6). g_word below, with g_word_ends set, is the inbound
// thread's own note of the word still owed to the engine, and so of the
// flag owed after it.
static volatile int g_end_after_drain = 0;
// A notice of ours on its way into the engine — the AYT answer, or the last
// word — and how much of it the engine has taken so far: telnet_send_text
// accepts SHORT when its queue is full, so under reply backpressure a notice
// enters the engine in pieces across flushes (Codex #101 rd7, rd8). One slot:
// a second AYT while the first answer is still going in is not answered
// again (one answer in a backpressured window is evidence enough), and the
// last word REPLACES whatever is pending, since nothing after it matters.
// g_word_ends says the pending word is the last one and the end is owed once
// the whole of it is in.
static const char *g_word = 0;
static size_t g_word_len = 0;
static size_t g_word_off = 0;
static int g_word_ends = 0;

// The reply mailbox: a lock-free single-producer/single-consumer byte ring.
// The inbound thread (producer) enqueues the engine's negotiation replies;
// the outbound thread (consumer) drains them to g_conn. Power-of-two size so
// the indices mask cleanly; the monotonic counters are published with
// release / read with acquire, the standard SPSC handshake. Replies are a
// few bytes and rare, so 1KB never fills in practice.
#define MBOX_CAP 1024u
static uint8_t g_mbox[MBOX_CAP];
static volatile uint32_t g_mbox_head;   // consumer advances this
static volatile uint32_t g_mbox_tail;   // producer advances this

static size_t mbox_put(const uint8_t *buf, size_t len)
{
	uint32_t tail = g_mbox_tail;
	uint32_t head = __atomic_load_n(&g_mbox_head, __ATOMIC_ACQUIRE);
	size_t space = MBOX_CAP - (uint32_t)(tail - head);
	size_t n = len < space ? len : space;
	for (size_t i = 0; i < n; i++)
		g_mbox[(tail + (uint32_t)i) & (MBOX_CAP - 1)] = buf[i];
	__atomic_store_n(&g_mbox_tail, tail + (uint32_t)n, __ATOMIC_RELEASE);
	return n;
}

static size_t mbox_get(uint8_t *buf, size_t cap)
{
	uint32_t head = g_mbox_head;
	uint32_t tail = __atomic_load_n(&g_mbox_tail, __ATOMIC_ACQUIRE);
	size_t avail = (uint32_t)(tail - head);
	size_t n = avail < cap ? avail : cap;
	for (size_t i = 0; i < n; i++)
		buf[i] = g_mbox[(head + (uint32_t)i) & (MBOX_CAP - 1)];
	__atomic_store_n(&g_mbox_head, head + (uint32_t)n, __ATOMIC_RELEASE);
	return n;
}

// Write every byte or give up — a short write on a live connection means the
// send ring was full and the kernel took what it could; loop until it all
// lands or the connection fails.
static int write_all(int32_t h, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len)
	{
		int64_t n = os64_write(h, buf + off, len - off);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

// ── husk -> client (the outbound bridge thread, and the SOLE g_conn writer) ──
// Every iteration first drains the reply mailbox (bytes the inbound thread
// queued), then reads husk's output with a SHORT DEADLINE and encodes it. The
// deadline is what lets a mailbox reply reach the wire while husk is idle: the
// read times out, the loop drains the mailbox, and re-reads — so a client
// that waits for a mid-session negotiation reply no longer stalls on future
// shell output (Codex #101 rd2). Encoding: a bare CR is held (`pending_cr`)
// until the next byte is known — CR LF for a newline, CR NUL for a standalone
// CR, as the NVT requires (a lone CR misrenders a strict client's line
// redraw) — and a 0xFF is doubled so output can never be read as IAC. Ends
// when the pipe does: husk exited and the slave's seats emptied, the STREAM
// pty's EOF.
static int64_t outbound_thread(void *arg)
{
	(void)arg;
	uint8_t in[512];
	uint8_t out[2100];   // up to 4 bytes per input byte (a resolved CR NUL + a doubled 0xFF)
	uint8_t mb[256];
	int pending_cr = 0;

	for (;;)
	{
		size_t m;
		while ((m = mbox_get(mb, sizeof(mb))) > 0)
			if (write_all((int32_t)g_conn, mb, m) < 0)
				goto done;
		if (__atomic_load_n(&g_end_after_drain, __ATOMIC_ACQUIRE))
		{
			// The inbound thread published the end AFTER its last word
			// went into the mailbox, so the word is there by now — but the
			// drain above may have run before it arrived. Drain once more,
			// then end.
			while ((m = mbox_get(mb, sizeof(mb))) > 0)
				if (write_all((int32_t)g_conn, mb, m) < 0)
					goto done;
			goto done;
		}

		int64_t n = os64_read_for((int32_t)g_master, in, sizeof(in), 100);
		if (n == OS64_ERR_TIMEOUT)
		{
			// No husk output. A CR held for its lookahead has now waited a
			// whole tick of silence: the program wrote a bare CR to redraw
			// its line and is waiting for input, so it was a standalone CR
			// — send it as CR NUL now rather than when the next byte
			// happens to arrive (Codex #101 rd5).
			if (pending_cr)
			{
				pending_cr = 0;
				uint8_t crnul[2] = { '\r', 0 };
				if (write_all((int32_t)g_conn, crnul, 2) < 0)
					goto done;
			}
			continue;                 // loop back to drain the mailbox
		}
		if (n <= 0)
			break;                    // EOF (husk gone) or a read error

		size_t o = 0;
		for (size_t i = 0; i < (size_t)n; i++)
		{
			uint8_t b = in[i];
			if (pending_cr)
			{
				pending_cr = 0;
				if (b == '\n')        // CR LF — a newline
				{
					out[o++] = '\r';
					out[o++] = '\n';
					if (o > sizeof(out) - 4)
					{
						if (write_all((int32_t)g_conn, out, o) < 0) goto done;
						o = 0;
					}
					continue;         // both bytes consumed
				}
				out[o++] = '\r';      // a standalone CR — CR NUL, then process b
				out[o++] = 0;
			}
			if (b == '\r')
				pending_cr = 1;       // hold it: the next byte decides CR LF vs CR NUL
			else if (b == '\n')       // a lone LF is a newline
			{
				out[o++] = '\r';
				out[o++] = '\n';
			}
			else
			{
				if (b == 0xFF)
					out[o++] = 0xFF;
				out[o++] = b;
			}
			if (o > sizeof(out) - 4)
			{
				if (write_all((int32_t)g_conn, out, o) < 0)
					goto done;
				o = 0;
			}
		}
		if (o && write_all((int32_t)g_conn, out, o) < 0)
			break;
	}
	if (pending_cr)                   // a trailing CR at EOF is a standalone CR
	{
		uint8_t crnul[2] = { '\r', 0 };
		write_all((int32_t)g_conn, crnul, 2);
	}
done:
	g_session_over = 1;   // husk is gone; wake the inbound loop off its poll
	return 0;
}

// The engine's queued replies straight to the socket — the STARTUP flush,
// while the main thread is still the only writer (the outbound thread has not
// started). Reports how much went so a partial write is not re-sent.
static void engine_to_socket_direct(telnet_t *eng)
{
	size_t len = 0;
	const uint8_t *p = telnet_pending(eng, &len);
	if (len == 0)
		return;
	if (write_all((int32_t)g_conn, p, len) == 0)
		telnet_sent(eng, len);
}

// The engine's queued replies into the mailbox — the STEADY-STATE flush, from
// the inbound thread, which must not write the socket. Whatever does not fit
// stays queued in the engine for the next call.
static void engine_to_mailbox(telnet_t *eng)
{
	size_t len = 0;
	const uint8_t *p = telnet_pending(eng, &len);
	if (len == 0)
		return;
	size_t q = mbox_put(p, len);
	if (q)
		telnet_sent(eng, q);
}

// The inbound thread's whole flush: everything the engine has queued goes
// to the mailbox as far as it fits, and if the session's last word has been
// queued and the engine is now EMPTY, the end is published — after the
// bytes, never before. Called after every receive AND on every poll timeout,
// because under reply backpressure a flush transfers only part of the queue,
// and the rest must not wait for the client's next keystroke to move
// (Codex #101 rd6).
static void flush_engine(telnet_t *eng)
{
	// The rest of a notice the engine could only take part of: offered
	// again on every flush until all of it is in. A finished notice frees
	// the slot; a finished LAST word owes the end, once the engine has
	// drained to empty.
	if (g_word != 0 && g_word_off < g_word_len)
		g_word_off += telnet_send_text(eng, g_word + g_word_off, g_word_len - g_word_off);
	if (g_word != 0 && g_word_off == g_word_len && !g_word_ends)
		g_word = 0;
	engine_to_mailbox(eng);
	size_t left = 0;
	(void)telnet_pending(eng, &left);
	if (g_word != 0 && g_word_ends && g_word_off == g_word_len && left == 0)
		__atomic_store_n(&g_end_after_drain, 1, __ATOMIC_RELEASE);
}

// Queue a notice of ours (see g_word). `ends` marks the last word, which
// takes the slot from anything pending; an ordinary notice waits its turn
// or, if one is already going in, is dropped.
static void queue_word(const char *word, size_t len, int ends)
{
	if (g_word != 0 && !ends)
		return;
	if (g_word != 0 && g_word_ends)
		return;   // the end is already on its way; nothing outranks it
	g_word = word;
	g_word_len = len;
	g_word_off = 0;
	g_word_ends = ends;
}

// Push keystrokes into the pty master — a full input ring returns a short
// count and then 0, and dropping the tail at the first 0 lost part of an
// ordinary large paste (Codex #101 P2), so on 0 the push naps and retries
// while husk drains room. BUT ONLY FOR SO LONG. A ring that stays full for
// half a second is a program that is not reading its input, and the loop
// that waits on it is the only reader of the socket: it would never see
// the client hang up, and a client that typed into a `sleep` and left
// would hold the session, the shell, the command and the connection until
// the command ended — repeatable until the kernel ran out of memory (Codex
// #101 rd10). A terminal drops typeahead when its buffer is full, and so
// does this one: what could not be delivered by the deadline is counted in
// g_keys_dropped and let go, and the loop returns to the socket. The
// patience is real time off the monotonic clock — a nap of 2 ms is a whole
// tick, so counting naps would wait five times longer than it says — and it
// is charged ONCE PER STALL: after a ring has stayed full for the whole
// patience, every further push drops at once until a write lands again, so
// a 4 KB flood costs one patience, not one per 512-byte read.
static uint64_t g_keys_dropped = 0;
static int g_keys_stalled = 0;
#define KEYS_PATIENCE_MS 500

static uint64_t now_ms(void)
{
	os64_ticks_t t;
	if (os64_ticks(&t) < 0 || t.per_second == 0)
		return 1;   // no clock: never 0, which the caller reads as "not started"
	return 1 + t.ticks * 1000 / t.per_second;
}

static void keys_to_master(const uint8_t *buf, size_t len)
{
	size_t off = 0;
	uint64_t started = 0;
	while (off < len)
	{
		int64_t n = os64_write((int32_t)g_master, buf + off, len - off);
		if (n < 0)
			break;                 // the master failed
		if (n == 0)
		{
			// Ring full. Retry — UNLESS husk has exited: an empty STREAM
			// slave still accepts writes and never fails just because its
			// seats emptied, so with nobody draining the ring the retry
			// would spin forever and never reach the session-over check
			// (Codex #101 rd2, a hole in round 1's retry fix). And unless
			// the ring is already known stuck, or the patience is spent:
			// then the rest is typeahead a program did not want, dropped
			// the way a terminal drops it.
			uint64_t now = now_ms();
			if (started == 0)
				started = now;
			if (g_session_over || g_keys_stalled || now - started >= KEYS_PATIENCE_MS)
			{
				g_keys_stalled = 1;
				g_keys_dropped += len - off;
				break;
			}
			os64_sleep(2);         // let husk read, then retry
			continue;
		}
		g_keys_stalled = 0;        // it read: the ring moves again
		off += (size_t)n;
	}
}

// ── One session ─────────────────────────────────────────────────────────────
static int run_session(void)
{
	// The STREAM pty: husk's output reaches read(master) as bytes, which is
	// what a remote terminal wants — the rendering happens at the far end.
	g_master = os64_pty_create_stream(80, 24);
	if (g_master < 0)
	{
		os64_printf("telnetd: pty_create_stream failed (%ld)\n", (long)g_master);
		return 1;
	}

	char *const husk_argv[] = { "/bin/husk", 0 };
	int64_t husk = os64_spawn_seated("/bin/husk", husk_argv, g_master);
	if (husk < 0)
	{
		os64_printf("telnetd: could not seat /bin/husk (%ld)\n", (long)husk);
		return 1;
	}

	telnet_t eng;
	telnet_init_server(&eng);
	// The server's own notices are LINES: their `\r\n` must reach the
	// client as CR LF (a new line), not the engine's default CR NUL (a bare
	// carriage return, which would overprint the line the notice landed
	// on). husk's bytes never pass through this setting — the outbound
	// thread encodes those itself (Codex #101 rd8).
	telnet_set_eol_crlf(&eng, true);
	telnet_offer_server(&eng);
	engine_to_socket_direct(&eng);   // server speaks first: WILL ECHO/SGA, DO
	                                 // NAWS — written directly, no other writer yet

	// Husk's output goes out on its own thread, because it and the inbound
	// read each block on one source and os64 has threads for exactly this.
	// From here on that thread is the ONLY g_conn writer.
	int64_t ob = os64_thread(outbound_thread, 0);
	if (ob < 0)
	{
		os64_printf("telnetd: could not start the output bridge (%ld)\n", (long)ob);
		return 1;
	}

	// Inbound: decode the client, feed husk, answer negotiation, follow a
	// window resize. The poll wakes every 250ms so a husk that exited first
	// (g_session_over) ends the session promptly instead of after the
	// client's next keystroke.
	uint8_t net[512];
	uint8_t data[512];
	for (;;)
	{
		int64_t n = os64_read_for((int32_t)g_conn, net, sizeof(net), 250);
		if (n == OS64_ERR_TIMEOUT || n == OS64_INTERRUPTED)
		{
			if (g_session_over)
				break;
			flush_engine(&eng);   // replies a full mailbox held back move now, not at the next key
			continue;
		}
		if (n <= 0)
			break;   // the client hung up

		size_t off = 0;
		while (off < (size_t)n)
		{
			size_t dlen = 0;
			size_t used = telnet_receive(&eng, net + off, (size_t)n - off,
			                             data, sizeof(data), &dlen);
			off += used;
			if (dlen)
				keys_to_master(data, dlen);

			// Notices BEFORE the flush, so an AYT answer queued here rides
			// out with the negotiation replies in the same drain.
			uint32_t notes = telnet_notices(&eng);
			if (notes & TELNET_NOTE_RESIZE)
			{
				uint16_t cols, rows;
				if (telnet_peer_size(&eng, &cols, &rows) && cols && rows)
					os64_pty_resize(g_master, cols, rows);
			}
			if (notes & TELNET_NOTE_AYT)
			{
				// "Are you there?" wants visible evidence (RFC 854), or a
				// client probing an idle-but-live session concludes it is
				// dead (Codex #101 rd2). telnet_send_text queues it; the
				// outbound thread is the one that writes the socket.
				static const char yes[] = "\r\n[telnetd: yes]\r\n";
				queue_word(yes, sizeof(yes) - 1, 0);   // fed in whole across flushes (rd8)
			}
			if ((notes & TELNET_NOTE_ECHO) && !telnet_option_ours(&eng, TELNET_OPT_ECHO))
			{
				// The client REFUSED our echo (DONT ECHO: a line-mode client
				// that echoes for itself). The shell behind the pty echoes
				// through the kernel's line discipline and os64 has no
				// master-side switch to turn that off, so honouring the
				// refusal is impossible and ignoring it shows every
				// keystroke twice. Say so and end the session, rather than
				// run one that misbehaves (Codex #101 rd5). The word goes
				// through the mailbox like every reply; flush_engine
				// publishes the end once the word has left the engine, and
				// the outbound thread sends it and then ends the session.
				static const char sorry[] =
					"\r\n[telnetd: this server echoes (RFC 857); a client that"
					" declines it would see every keystroke twice -- closing]\r\n";
				queue_word(sorry, sizeof(sorry) - 1, 1);   // flush_engine feeds it in, whole, before the end
			}
			flush_engine(&eng);   // the outbound thread sends these

			// used == 0 && dlen == 0 means telnet_receive could neither emit
			// data nor answer the next byte — BACKPRESSURE: the client stopped
			// reading, so the mailbox and the engine's reply queue are full.
			// Do NOT break to a fresh socket read — the unconsumed suffix
			// (net[off..n]) is not lost, it is retried once the outbound
			// thread drains space (Codex #101 rd3; the old comment here
			// claimed the reread was safe, and it was not). off stays put, so
			// the while loop re-runs telnet_receive on the same bytes. End if
			// husk exited meanwhile.
			if (used == 0 && dlen == 0)
			{
				if (g_session_over)
					break;
				os64_sleep(2);
			}
		}

		// End the session even while input keeps arriving: if husk has exited
		// (the outbound thread set this), a client still typing would keep
		// every read succeeding and never let the timeout branch above notice
		// (Codex #101 P2), leaving a dead shell unreaped and the session held.
		if (g_session_over)
			break;
	}

	// Typeahead the session had to drop (keys_to_master) is said once, on
	// the LISTENER's console — handle 2 stays the listener's — never down
	// the wire, which the client may already have hung up.
	if (g_keys_dropped)
	{
		char line[96];
		int32_t n = os64_snprintf(line, sizeof(line),
		                          "telnetd: session dropped %lu bytes of typeahead a program did not read\n",
		                          (unsigned long)g_keys_dropped);
		if (n > 0)
			os64_write(2, line, (size_t)n);
	}

	// Returning ends the task: the master closes (husk hears SIGHUP and
	// exits) and the connection closes (its FIN goes out), in whatever order
	// the handle table holds them. The outbound thread ends with the task.
	return 0;
}

// ── The listener ────────────────────────────────────────────────────────────
static int run_listener(int argc, char **argv)
{
	uint16_t port = 23;
	if (argc > 1)
	{
		int64_t p = os64_atoi(argv[1]);
		if (p < 1 || p > 65535)
		{
			os64_printf("telnetd: %s is not a port (1-65535)\n", argv[1]);
			return 2;
		}
		port = (uint16_t)p;
	}

	os64_netdest_t local = { .ip = 0, .port = port, .protocol = OS64_NET_TCP };
	int64_t listener = os64_net_announce(&local);
	if (listener < 0)
	{
		os64_printf("telnetd: cannot announce port %u: %s\n",
		            (unsigned)port, os64_dial_reason(listener));
		return 1;
	}
	os64_printf("telnetd: listening on port %u\n", (unsigned)port);

	char *const session_argv[] = { "/bin/telnetd", "-session", 0 };
	for (;;)
	{
		// Reap every finished session, then accept with a PATIENCE so this
		// runs again on its own. os64 has no SIGCHLD to interrupt a blocking
		// accept when a session exits, and a plain blocking accept would
		// leave a departed session a zombie until the NEXT connection woke
		// the loop — which, if nobody connects again, is forever (a bug
		// Chris caught by exiting one telnet session and finding its corpse
		// still listed). A 2-second accept means the reap runs at least that
		// often even while idle, so a session is collected within a couple
		// of seconds of its exit. os64_reap never blocks; the wait lives in
		// the accept.
		while (os64_reap(0) > 0)
			;

		os64_netconn_t peer;
		int64_t r = os64_read_for((int32_t)listener, &peer, sizeof(peer), 2000);
		if (r == OS64_ERR_TIMEOUT || r == OS64_INTERRUPTED)
			continue;   // idle tick, or a signal — loop back to the reap and re-accept
		if (r < 0)
		{
			os64_printf("telnetd: accept failed (%ld) — stopping\n", (long)r);
			break;
		}
		if (r != (int64_t)sizeof(peer))
			continue;   // a refusal short of the whole struct — never half a peer

		// The child's stdin AND stdout are the connection; the kernel takes
		// a reference for each slot, so the FIN waits for the child's own
		// closes (SERVERS.md). stderr (-1) stays this listener's — a
		// diagnostic lands on the console, not down the wire.
		int64_t child = os64_spawn_redirected("/bin/telnetd", session_argv,
		                                      peer.handle, peer.handle, -1, 0);
		if (child < 0)
			os64_printf("telnetd: could not spawn a session (%ld)\n", (long)child);
		os64_close(peer.handle);   // the listener's own copy; the child holds its two
	}

	os64_close((int32_t)listener);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 's')
		return run_session();
	return run_listener(argc, argv);
}
