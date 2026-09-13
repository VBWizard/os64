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
// g_master is the STREAM pty whose slave husk sits on. Both bridge threads
// write g_conn — the kernel serializes concurrent writes to one TCP handle
// (tcp_conn_write copies under the connection lock), so the shared writer is
// safe against corruption; only the telnet ENGINE is single-threaded, owned
// by the inbound loop.
static int32_t g_conn = 1;
static int64_t g_master = -1;
static volatile int g_session_over = 0;

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

// ── husk -> client (the outbound bridge thread) ─────────────────────────────
// Stateless NVT encoding: a lone LF becomes CR LF (an existing CR is left
// alone, so husk's own "\r\n" does not become "\r\r\n"), and a 0xFF is
// doubled so a byte of output can never be read as IAC. last_cr carries the
// CR-then-LF decision across reads. Ends when the pipe does — husk exited and
// the slave's seats emptied, which is the STREAM pty's EOF.
static int64_t outbound_thread(void *arg)
{
	(void)arg;
	uint8_t in[512];
	uint8_t out[1200];   // worst case is 2x plus the odd split CR — 1200 > 2*512
	int last_cr = 0;

	for (;;)
	{
		int64_t n = os64_read((int32_t)g_master, in, sizeof(in));
		if (n <= 0)
			break;
		size_t o = 0;
		for (size_t i = 0; i < (size_t)n; i++)
		{
			uint8_t b = in[i];
			if (b == '\r')
			{
				out[o++] = '\r';
				last_cr = 1;
			}
			else if (b == '\n')
			{
				if (!last_cr)
					out[o++] = '\r';
				out[o++] = '\n';
				last_cr = 0;
			}
			else
			{
				if (b == 0xFF)
					out[o++] = 0xFF;
				out[o++] = b;
				last_cr = 0;
			}
			if (o > sizeof(out) - 3)
			{
				if (write_all((int32_t)g_conn, out, o) < 0)
					goto done;
				o = 0;
			}
		}
		if (o && write_all((int32_t)g_conn, out, o) < 0)
			break;
	}
done:
	g_session_over = 1;   // husk is gone; wake the inbound loop off its poll
	return 0;
}

// Flush whatever the engine has queued for the peer, reporting back how much
// actually went so a partial write is not re-sent.
static void flush_engine(telnet_t *eng)
{
	size_t len = 0;
	const uint8_t *p = telnet_pending(eng, &len);
	if (len == 0)
		return;
	int64_t w = os64_write((int32_t)g_conn, p, len);
	if (w > 0)
		telnet_sent(eng, (size_t)w);
}

// Push keystrokes into the pty master, all of them (a short write drops the
// tail otherwise). Human typing is far below the ring, but a paste is not.
static void keys_to_master(const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len)
	{
		int64_t n = os64_write((int32_t)g_master, buf + off, len - off);
		if (n <= 0)
			break;
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
	telnet_offer_server(&eng);
	flush_engine(&eng);   // the server speaks first: WILL ECHO/SGA, DO NAWS

	// Husk's output goes out on its own thread, because it and the inbound
	// read each block on one source and os64 has threads for exactly this.
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
			flush_engine(&eng);

			uint32_t notes = telnet_notices(&eng);
			if (notes & TELNET_NOTE_RESIZE)
			{
				uint16_t cols, rows;
				if (telnet_peer_size(&eng, &cols, &rows) && cols && rows)
					os64_pty_resize(g_master, cols, rows);
			}

			// used == 0 means the data buffer filled with no room to make
			// progress; it was just drained to the master, so the next pass
			// of this same loop consumes more. A used of 0 with a full input
			// still un-consumed would spin, so break to read afresh only when
			// nothing at all moved.
			if (used == 0 && dlen == 0)
				break;
		}
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
