# SERVERS.md — the inbound door: announce, the stream pty, and telnetd

*Design record, Fable, 2026-09-12, written before the code per the known-debt
rule. Chris opened it that morning: once os64 can be reached over the wire,
the models can explore and test on the P5 instead of only in QEMU. The three
pieces below are the order the work is done in, and each earns its keep
without the next: a listener serves every server after this one, the stream
pty serves any program that wants a shell's bytes rather than its screen, and
telnetd is the 1969 protocol that proves the seam end to end before sshd
(SSHD.md, Quinn's spec) replaces it on the same seams.*

**Read this first if you are about to expose the machine:** telnetd is
PLAINTEXT and hands a shell to anyone who can reach port 23. os64 has no
users, so that shell is the machine. The trust boundary is the LAN it sits
on — Chris's, behind ICS — and nothing else. sshd is what makes the door
safe to leave open; telnetd is what proves the door opens.

## 1. The listener — `announce`, and accept is a read

NETWORK.md ruling #3 (ratified 2026-07-28) already fixed the shape; this is
its arrival. The verb is Plan 9's `announce`, kept for the reason `dial` was:
it says what it does — "I am here, on this port."

- **`announce(const os64_netdest_t *local)`** (syscall 55) returns a
  LISTENER handle (`HANDLE_NET_LISTENER`). `protocol` must be TCP (a UDP
  announce waits for its consumer — DEBTS). `ip` must be 0: "every address
  this machine has", which is one NIC today (a per-address announce waits
  for a second NIC — DEBTS). `port` is 1..65535. The dial string spelling is
  `tcp!*!23`, `*` being how every dialer since Plan 9 has spelled "any of
  mine".
- **`read(listener, &conn, sizeof conn)`** blocks until a connection has
  completed its handshake, then yields ONE `os64_netconn_t {handle, peer_ip,
  peer_port}` — the new stream's handle is allocated in the reader's table,
  and the peer's identity arrives with it (accept's sockaddr out-param and
  getpeername, retired in one stroke). Returns `sizeof(os64_netconn_t)`. A
  buffer shorter than that is refused, never half-filled. `os64_read_for`'s
  patience works on it (`OS64_ERR_TIMEOUT` when nobody came); a signal ends
  the wait like every other park.
- **Refusals** are specific, the dial table's doctrine: `BAD_DEST` (a
  non-zero ip, a protocol other than TCP, port 0), `NO_NIC`, `NO_RESOURCES`
  (handles, memory), and one new code, **`OS64_NET_ERR_PORT_TAKEN`**:
  another listener already answers there, or a dialed connection holds that
  port (a port inside the ephemeral range is claimed in the same bitmap the
  draw reads, so the two can never collide).

### The kernel's side

A `tcp_listener_t` is a row on `kTcpListenerList`, guarded by
`kTcpListLock` like the connections — the demux takes that lock anyway, and
a listener is consulted only when the four-tuple search finds nobody. A
SYN for an announced port creates a PASSIVE connection: same `tcp_conn_t`,
same list, same timers, born in `SYN_RECEIVED` with `c->listener` naming
its owner. The peer's SYN options are parsed exactly as a SYN-ACK's are
(the parser was written for both); our SYN-ACK carries our MSS always and
our window shift ONLY if theirs did — RFC 7323 §2.2 says the passive side
may not offer scaling to a peer that did not ask, and a shift the peer is
not applying would have us reading every window 32× too large.

`SYN_RECEIVED` is the one state tcp_input did not handle, and it handles it
now: the ACK that completes the handshake must acknowledge exactly our
SYN-ACK (anything else is answered with RST, RFC 793 §3.9); a retransmitted
SYN from the same peer gets the SYN-ACK again; an acceptable RST returns the
row to nothing (there is no reader to wake — nobody has this connection
yet); the RTO resends the SYN-ACK and the retry budget abandons it. On
completion the connection is ESTABLISHED, queued on its listener, and the
listener's parked reader is woken — through the same claim-and-wake exit
that data arrival uses, because DOORBELL.md's rule applies: the reader
should not wait for a tick.

**Ownership, because it is where lifetime bugs live.** A connection belongs
to exactly one of: a listener (`c->listener != NULL`), a handle, or nobody
(`detached`). Accept moves it from the first to the second. The reaper
treats listener-owned like detached for the purpose of freeing a CLOSED
row, and NOT for the no-progress abort that a closed handle earns — a
queued connection nobody has read yet is not making progress, and that is
fine. **Closing a listener** unlinks it (no new SYN can find it), aborts
every connection it still owns with an RST to the peer (an accept queue
nobody will read is a promise the machine can no longer keep), hands those
rows to the reaper, wakes its parked reader (who returns `INVALID`), and
leaves the row to `tcp_poll` to free once no accept is inside it
(`busy == 0`) — close never frees what a parked reader may still be
holding, the tombstone discipline again.

**The backlog** is `TCP_LISTEN_BACKLOG` = 16, counting half-open and
accepted-not-yet-read together. A SYN past it is DROPPED silently and
counted (`syns_dropped`), which is what every stack did before SYN cookies
and what a LAN-facing listener needs. SYN cookies are booked, not built:
the threat model is a home network.

**`/sys/net/tcp`** grows a `# listeners` section — port, queued,
half_open, accepted, dropped — and passive connections appear as ordinary
rows (they are ordinary rows).

## 2. The stream pty — `PTY_MODE_STREAM`

PTY.md pre-named this mode on 2026-08-19 and gated it on listen. The
principle it recorded stands: telnetd moves BYTES between a socket and a
shell, and the rendering happens on the far end, so it wants the child's
output *before* the terminal interpreter. Everything expensive about a pty
was built flavor-independent; the flavor is one branch at one choke point.

- **`pty_create(cols, rows, mode)`** — the syscall's third register, 0 =
  GRID (what every existing caller passes), 1 = STREAM, anything else
  refused. libos64: `os64_pty_create` stays GRID, `os64_pty_create_stream`
  is the other door.
- **A STREAM slave's output is a pipe wearing a tty's identity** — PTY.md's
  own phrase, taken literally: the slave owns a `pipe_t`, and the master's
  `read()` is a `pipe_read`. os64 already owns every rule the pipe has; the
  pty borrows them rather than restating them. The grid stays allocated and
  is never fed: it is what `/proc/self/tty` and `pty_resize` measure, and a
  pty with no cells is the "not ready" glass path in `tty_write`, which must
  never be where a stream lands.
- **TWO WRITE PATHS INTO THAT PIPE, because one may block and the other may
  not.** A seated task writing its console handle reaches the console-out
  syscall, which for a STREAM slave calls `pipe_write` — it BLOCKS on a full
  ring, which is the point: a remote terminal reading slowly is the reader
  being slow, never a reason to drop the program's output ("never drop a
  byte"). Kernel-originated text — a ring-3 death headline from the
  exception path, which may not park — reaches `tty_write`, whose STREAM
  branch is `pipe_write_if_room`: it pushes what fits and counts the rest in
  `stream_dropped`. That is the one byte this design chooses to lose, and it
  is never a byte the child wrote. A closed pipe (master gone) is BENIGN in
  both paths — the seated write absorbs the bytes and the task proceeds
  toward the SIGHUP it also received, exactly as a write to an orphaned GRID
  slave is absorbed by the grid.
- **Input is unchanged.** The master's write still becomes keystrokes on
  the slave — 0x03 runs the Ctrl+C intercept against the slave's
  foreground, 0x04 is EOT — because a remote terminal's keys are keys.
- **EOF has one meaning in each direction.** When the slave's seats empty
  (after it has ever been seated — a young pty is not a finished one) the
  pipe's write end closes and the master's read returns 0: THE SESSION
  ENDED, the STREAM spelling of `OS64_PTY_HUNGUP`. When the master closes,
  the seats get SIGHUP as before AND the pipe's read end closes, so a child
  that writes after the terminal left gets `PIPE_ERR_CLOSED` instead of
  parking forever on a ring nobody will drain.
- **`pty_snapshot` on a STREAM pty is refused**, the mirror of `read()` on
  a GRID one: a grid nobody fed is not a screen. `pty_resize` works on
  both (SIGWINCH is about geometry, not rendering).
- **The master's `read_for` patience stays refused** — pipes carry no
  deadline and a lie with a delay is the thing the tripwire doctrine
  forbids. telnetd needs none: it runs one thread per direction.

## 3. telnetd — the 1969 protocol on the 2026 seams

`/bin/telnetd [port]` announces (23 by default) and loops on accept. For
each connection it spawns **`/bin/telnetd -session`** with handles 0 and 1
= the connection and 2 inherited, which is inetd's 1986 model and
Bernstein's tcpserver's after it: the socket IS stdin and stdout, and the
session program never learns it is on a network. os64's rule that a child
gets 0/1/2 plus exactly what was asked for is that model already.

The session: `os64_pty_create_stream(80, 24)`, `os64_spawn_seated("/bin/
husk")`, then two threads — the main thread reads the connection and
feeds the master, the second reads the master and feeds the connection —
because each side blocks on one source and os64 has threads. Whichever
direction ends first exits the task; the kernel's close-all then hangs up
the shell (master close → SIGHUP) and sends the FIN (connection close),
in whichever order the table holds them. Nothing here needs a select.

**The NVT translation** (RFC 854, both directions):

| Inbound (client → shell) | Outbound (shell → client) |
|---|---|
| CR LF and CR NUL become LF; a lone NUL is dropped (the engine already does this) | LF becomes CR LF |
| IAC IP becomes 0x03 — the Ctrl+C intercept fires on the slave | 0xFF is doubled |
| IAC AYT is answered; AO, EC, EL, BRK are consumed | |
| SB NAWS becomes `os64_pty_resize` — SIGWINCH reaches the shell | |

**Negotiation reuses the client's engine** (`apps/telnet/telnet_protocol.c`,
host-tested at every chunk size) with a SERVER ROLE (`telnet_init_server`,
`telnet_offer_server`): the server says WILL ECHO, WILL SGA, DO SGA, DO
NAWS; `we_agree`/`he_may` become role-aware so it agrees to ECHO and SGA as
ours and NAWS and SGA as his, and refuses the rest by name. The RFC 1143
machinery is symmetric and stays one copy. What the role adds to the engine:
an inbound NAWS subnegotiation is PARSED (cols, rows, IAC-doubled dimensions
and all) into `telnet_peer_size` and a `TELNET_NOTE_RESIZE` notice, where
the client engine only ever skipped a subnegotiation to throw it away. ECHO
is offered by the server because echo is the reader's job in os64 — husk
echoes what it reads — and a client that echoed locally as well would show
every key twice. The outbound half (husk's bytes → client) needs no
negotiation state — a bare LF becomes CR LF, a 0xFF is doubled — so telnetd
does it inline rather than through the shared engine's queue, which the two
bridge threads would otherwise race over with no lock to hold.

**Launch:** the `TELNETD` cmdline token, the CRON precedent — the kernel
starts it once userland is up, NOT husk.rc, because husk.rc runs in every
husk and two listeners on one port is a refusal. Deliberately absent from
the lifeboat entry. **A cmdline token does not ship to the P5**: Chris adds
it to the P5's own limine.conf, or types `telnetd &` at a prompt.

## Verification

**Proven end to end in QEMU on 2026-09-12** (virtio-net + slirp with
`hostfwd=tcp::2323-:23`, telnetd launched by hand as `telnetd &` on VT1):

- The listener announced port 23; `/sys/net/tcp` showed
  `23 LISTEN 0 16 <accepted> 0` and passive connections as ordinary
  ESTABLISHED rows on local port 23.
- A host telnet client negotiated (WILL ECHO/SGA, DO NAWS, a NAWS size
  sent), reached husk, and ran `ls /`, `ls /bin`, `cat /sys/net/tcp` with
  complete output — the blocking-write path carrying multi-line output with
  no loss.
- TWO concurrent sessions ran as two passive connections (the spawn-per-
  connection model), and `exit` on a session closed its connection cleanly
  (husk's SIGHUP → session process exit → FIN). No panic, no leak across
  repeated connect/disconnect.

Still owed:

- **The engine's host test** should gain the server role: WILL/DO/WONT/DONT
  from a server's point of view and an inbound NAWS with a 255 in it, at
  every chunk size the harness already runs. The client role's cases still
  pass unchanged.
- **A scripted acceptance test** (`tools/test_telnetd_host.py`) folding the
  hand-run round trip above into the harness, so a later slice can be driven
  by bytes instead of screendumps. On the P5 the same script, or a Windows
  or WSL2 `telnet`, against its own address.
- **Kernel self-test:** none for the listener yet — the stack has no
  loopback (a dial to our own address goes to the wire and is not looped
  back), so the machine cannot accept its own connection. Booked; the host
  script is the fixture until then.

## Booked (DEBTS.md rows follow the code)

UDP announce; announce on one address of several; SYN cookies; loopback
(and with it an in-OS listener fixture); a deadline on pipe reads (and so
on a STREAM master's read); `/sys` rows for ptys; the STREAM slave's
kernel-text drop counter surfacing somewhere readable.
