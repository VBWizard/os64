// looptest — the machine talks to itself (REMOTE.md § 1).
//
// The in-OS listener fixture SERVERS.md had been owed since the day announce
// arrived: with no loopback, a dial to our own address went out on the wire
// and nothing could accept its own call, so every server was proved from the
// host alone. With lo, this program is both ends.
//
//   1. The refusals: a second listener on a port is PORT_TAKEN whatever its
//      address; an announce on a LAN address and a UDP dial to loopback are
//      BAD_DEST; a dial to a closed loopback port is REFUSED at once.
//   2. A loopback-only door: accept names the peer 127.0.0.1, /sys/net/tcp
//      shows the listener with its address, and 3 MiB crosses each way with
//      a checksum, in two phases so neither side's ring fills while the
//      other is not reading.
//   3. `localhost` dials through /etc/hosts to the same door.
//   4. A wildcard door answers on loopback too, as INADDR_ANY always has.
//   5. The client's close is the server's EOF.
//
// What this cannot show from inside is the other half of the promise: that a
// SYN to the loopback-only port from the LAN is refused. That is driven from
// the host through QEMU's port forward (REMOTE.md § 1, Proof).
//
// Exit codes: 0x100BBAC0 success, 0x100BF0nn the step that failed.

#include "os64/os64.h"

#define LOOPTEST_OK              0x100BBAC0
#define LOOPTEST_ANNOUNCE        0x100BF001   // the loopback announce was refused
#define LOOPTEST_NOT_TAKEN       0x100BF002   // a second listener on the port was not PORT_TAKEN
#define LOOPTEST_LAN_ADDRESS     0x100BF003   // an announce on a LAN address was not BAD_DEST
#define LOOPTEST_UDP_LOOPBACK    0x100BF004   // a UDP dial to loopback was not BAD_DEST
#define LOOPTEST_NOT_REFUSED     0x100BF005   // a dial to a closed loopback port was not REFUSED
#define LOOPTEST_NO_THREAD       0x100BF006
#define LOOPTEST_DIAL            0x100BF007   // the dial to the loopback door failed
#define LOOPTEST_SERVER          0x100BF008   // the server thread reported a failure (printed)
#define LOOPTEST_UPLOAD          0x100BF009   // client -> server bytes or checksum wrong
#define LOOPTEST_DOWNLOAD        0x100BF00A   // server -> client bytes or checksum wrong
#define LOOPTEST_SYS_ROW         0x100BF00B   // /sys/net/tcp does not show the listener's address
#define LOOPTEST_LOCALHOST       0x100BF00C   // `localhost` did not reach the door
#define LOOPTEST_WILDCARD        0x100BF00D   // a wildcard door did not answer on loopback

// 127.0.0.1 in host order, which is how accept reports a peer (os64/net.h).
#define LOOPBACK_IP   0x7F000001u
#define TRANSFER      (3u * 1024u * 1024u)
#define CHUNK         8192

// A stream both ends can generate independently, so the receiver checks every
// byte without the sender shipping its expectations alongside.
static uint8_t pattern_byte(uint32_t i)
{
	uint32_t x = i * 2654435761u;
	return (uint8_t)(x >> 24);
}

// Fletcher-style: order matters, so a reordered or duplicated chunk shows.
static uint64_t mix(uint64_t sum, const uint8_t *p, int64_t n)
{
	for (int64_t i = 0; i < n; i++)
		sum = sum * 31u + p[i];
	return sum;
}

static int64_t write_all(int32_t h, const uint8_t *p, uint32_t n)
{
	uint32_t done = 0;
	while (done < n)
	{
		int64_t w = os64_write(h, p + done, n - done);
		if (w <= 0)
			return w;
		done += (uint32_t)w;
	}
	return (int64_t)done;
}

// Send TRANSFER pattern bytes; returns the checksum of what was sent, or 0
// after printing why.
static uint64_t send_pattern(int32_t h, const char *who)
{
	static uint8_t buf[CHUNK];
	uint64_t sum = 1;
	for (uint32_t off = 0; off < TRANSFER; off += CHUNK)
	{
		for (uint32_t i = 0; i < CHUNK; i++)
			buf[i] = pattern_byte(off + i);
		if (write_all(h, buf, CHUNK) != CHUNK)
		{
			os64_printf("looptest: %s write failed at offset %u\n", who, off);
			return 0;
		}
		sum = mix(sum, buf, CHUNK);
	}
	return sum;
}

// Receive exactly TRANSFER bytes and check each against the pattern.
static bool recv_pattern(int32_t h, const char *who, uint64_t *sum_out)
{
	static uint8_t rbuf[CHUNK];
	uint64_t sum = 1;
	uint32_t got = 0;
	while (got < TRANSFER)
	{
		uint32_t want = TRANSFER - got < CHUNK ? TRANSFER - got : CHUNK;
		int64_t n = os64_read(h, rbuf, want);
		if (n <= 0)
		{
			os64_printf("looptest: %s read answered %ld after %u bytes\n", who, (long)n, got);
			return false;
		}
		for (int64_t i = 0; i < n; i++)
			if (rbuf[i] != pattern_byte(got + (uint32_t)i))
			{
				os64_printf("looptest: %s byte %u is wrong\n", who, got + (uint32_t)i);
				return false;
			}
		sum = mix(sum, rbuf, n);
		got += (uint32_t)n;
	}
	*sum_out = sum;
	return true;
}

typedef struct
{
	int32_t  listener;
	uint64_t upload_sum;     // what the server received
	uint32_t peer_ip;
	int64_t  eof;            // the server's read after the client closed
} server_t;

// The server: accept one call, take the upload, send its checksum back, send
// the download, then wait for the client to hang up.
static int64_t serve(void *arg)
{
	server_t *s = (server_t *)arg;
	os64_netconn_t conn;
	if (os64_accept(s->listener, &conn) != 0)
	{
		os64_printf("looptest: accept failed\n");
		return 1;
	}
	s->peer_ip = conn.peer_ip;
	if (!recv_pattern(conn.handle, "server", &s->upload_sum))
		return 2;
	if (write_all(conn.handle, (const uint8_t *)&s->upload_sum, sizeof(s->upload_sum)) != sizeof(s->upload_sum))
		return 3;
	if (send_pattern(conn.handle, "server") == 0)
		return 4;
	uint8_t byte;
	s->eof = os64_read(conn.handle, &byte, 1);
	os64_close(conn.handle);
	return 0;
}

// Accept one call and hang up — enough to prove a door answered.
static int64_t answer_once(void *arg)
{
	int32_t listener = (int32_t)(int64_t)arg;
	os64_netconn_t conn;
	if (os64_accept(listener, &conn) != 0)
		return 1;
	os64_close(conn.handle);
	return conn.peer_ip == LOOPBACK_IP ? 0 : 2;
}

static bool sys_shows_loopback_listener(void)
{
	static char text[16384];
	int64_t h = os64_open("/sys/net/tcp", NULL);
	if (h < 0)
		return false;
	int64_t n = os64_read((int32_t)h, text, sizeof(text) - 1);
	os64_close((int32_t)h);
	if (n <= 0)
		return false;
	text[n] = '\0';
	// One line, both facts: the row for this port must END in its address.
	for (char *line = text; *line != '\0'; )
	{
		char *end = line;
		while (*end != '\0' && *end != '\n')
			end++;
		char saved = *end;
		*end = '\0';
		bool match = os64_glob_match("4401 LISTEN * 127.0.0.1", line);
		*end = saved;
		if (match)
			return true;
		line = (*end == '\n') ? end + 1 : end;
	}
	return false;
}

// `looptest hold <address>`: keep a door open on port 4401 for thirty seconds
// and greet whoever gets through, so the HOST can knock through QEMU's port
// forward. With 127.0.0.1 nobody should (the forward delivers to the LAN
// address); with * the same knock is the control that shows the forward
// works (spelled `any` as well as `*`, because a shell globs a bare star).
// Exits with the number of calls answered.
static int hold(const char *address)
{
	if (os64_streq(address, "any"))
		address = "*";
	char dial[64];
	os64_snprintf(dial, sizeof(dial), "tcp!%s!4401", address);
	int64_t lh = os64_announce(dial);
	if (lh < 0)
	{
		os64_printf("looptest hold: %s: %s\n", dial, os64_dial_reason(lh));
		return 255;
	}
	os64_printf("looptest hold: %s open for 30 seconds\n", dial);
	int answered = 0;
	for (int second = 0; second < 30; second++)
	{
		os64_netconn_t conn;
		int64_t n = os64_read_for((int32_t)lh, &conn, sizeof(conn), 1000);
		if (n != (int64_t)sizeof(conn))
			continue;
		os64_printf("looptest hold: answered a call from %u.%u.%u.%u\n",
		            conn.peer_ip >> 24, (conn.peer_ip >> 16) & 0xFF,
		            (conn.peer_ip >> 8) & 0xFF, conn.peer_ip & 0xFF);
		os64_write(conn.handle, "looptest\n", 9);
		os64_close(conn.handle);
		answered++;
	}
	os64_close((int32_t)lh);
	return answered;
}

int main(int argc, char **argv)
{
	if (argc == 3 && os64_streq(argv[1], "hold"))
		return hold(argv[2]);

	int64_t lh = os64_announce("tcp!127.0.0.1!4401");
	if (lh < 0)
	{
		os64_printf("looptest: announce on loopback: %s\n", os64_dial_reason(lh));
		return LOOPTEST_ANNOUNCE;
	}
	int64_t r = os64_announce("tcp!*!4401");
	if (r != OS64_NET_ERR_PORT_TAKEN)
	{
		os64_printf("looptest: a second listener on 4401 answered %ld\n", (long)r);
		return LOOPTEST_NOT_TAKEN;
	}
	r = os64_announce("tcp!10.0.2.15!4404");
	if (r != OS64_NET_ERR_BAD_DEST)
	{
		os64_printf("looptest: an announce on a LAN address answered %ld\n", (long)r);
		return LOOPTEST_LAN_ADDRESS;
	}
	r = os64_dial("udp!127.0.0.1!9");
	if (r != OS64_NET_ERR_BAD_DEST)
	{
		os64_printf("looptest: a UDP dial to loopback answered %ld\n", (long)r);
		return LOOPTEST_UDP_LOOPBACK;
	}
	r = os64_dial("tcp!127.0.0.1!4403");
	if (r != OS64_NET_ERR_REFUSED)
	{
		os64_printf("looptest: a dial to a closed loopback port answered %ld\n", (long)r);
		return LOOPTEST_NOT_REFUSED;
	}
	if (!sys_shows_loopback_listener())
		return LOOPTEST_SYS_ROW;

	static server_t server;
	server.listener = (int32_t)lh;
	server.eof = -1;
	int64_t th = os64_thread(serve, &server);
	if (th < 0)
		return LOOPTEST_NO_THREAD;

	int64_t ch = os64_dial("tcp!127.0.0.1!4401");
	if (ch < 0)
	{
		os64_printf("looptest: dial to the loopback door: %s\n", os64_dial_reason(ch));
		return LOOPTEST_DIAL;
	}
	uint64_t sent = send_pattern((int32_t)ch, "client");
	uint64_t echoed = 0;
	int64_t n = os64_read((int32_t)ch, &echoed, sizeof(echoed));
	uint64_t got = 0;
	bool down_ok = recv_pattern((int32_t)ch, "client", &got);
	os64_close((int32_t)ch);

	int64_t verdict = -1;
	os64_thread_join((int32_t)th, &verdict);
	if (verdict != 0)
	{
		os64_printf("looptest: server step %ld failed\n", (long)verdict);
		return LOOPTEST_SERVER;
	}
	if (sent == 0 || n != sizeof(echoed) || echoed != sent || server.upload_sum != sent)
		return LOOPTEST_UPLOAD;
	if (!down_ok || got != sent)
		return LOOPTEST_DOWNLOAD;
	if (server.peer_ip != LOOPBACK_IP)
	{
		os64_printf("looptest: accept named the peer %x, wanted 127.0.0.1\n", server.peer_ip);
		return LOOPTEST_SERVER;
	}
	if (server.eof != 0)
	{
		os64_printf("looptest: the server's read after the hang-up answered %ld, wanted EOF\n",
		            (long)server.eof);
		return LOOPTEST_SERVER;
	}

	// The name, through the resolver and /etc/hosts, to the same door.
	th = os64_thread(answer_once, (void *)lh);
	if (th < 0)
		return LOOPTEST_NO_THREAD;
	ch = os64_dial("tcp!localhost!4401");
	if (ch >= 0)
		os64_close((int32_t)ch);
	os64_thread_join((int32_t)th, &verdict);
	if (ch < 0 || verdict != 0)
	{
		os64_printf("looptest: localhost: %s\n", ch < 0 ? os64_dial_reason(ch) : "the door did not answer");
		return LOOPTEST_LOCALHOST;
	}
	os64_close((int32_t)lh);

	int64_t wh = os64_announce("tcp!*!4402");
	if (wh < 0)
		return LOOPTEST_WILDCARD;
	th = os64_thread(answer_once, (void *)wh);
	if (th < 0)
		return LOOPTEST_NO_THREAD;
	ch = os64_dial("tcp!127.0.0.1!4402");
	if (ch >= 0)
		os64_close((int32_t)ch);
	os64_thread_join((int32_t)th, &verdict);
	os64_close((int32_t)wh);
	if (ch < 0 || verdict != 0)
		return LOOPTEST_WILDCARD;

	os64_printf("looptest: loopback carried 3 MiB each way; the refusals and both doors answer as designed\n");
	return LOOPTEST_OK;
}
