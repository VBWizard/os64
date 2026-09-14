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

**Passive ring storage** has a separate machine-wide limit of
`TCP_PASSIVE_BUFFER_LIMIT` = 32 connections: 64 MiB for their 1 MiB receive
and send rings, plus connection metadata. Half-open, queued, accepted and
detached closing connections share this limit across listeners. A full
limit drops new SYNs before allocation; capacity returns when TCP frees
the rings, including normal close, reset and detached timeout paths.
Accept, child exit and listener replacement do not return this capacity.
Outbound dials use their existing policy. There are no reserved slots per
service: a saturated listener can temporarily prevent another listener
from admitting peers.

**`/sys/net/tcp`** grows a `# listeners` section — port, queued,
half_open, accepted, dropped — and passive connections appear as ordinary
rows (they are ordinary rows). `passive_buffered` and `passive_buffer_limit`
show ring usage and its bound; `syns_dropped_storage` counts storage refusals
separately from `syns_dropped_full` backlog refusals. Each listener's dropped
count includes both reasons.

## 2. The stream pty — `PTY_MODE_STREAM`

PTY.md pre-named this mode on 2026-08-19 and gated it on listen. The
principle it recorded stands: telnetd moves BYTES between a socket and a
shell, and the rendering happens on the far end, so it wants the child's
output *before* the terminal interpreter. Everything expensive about a pty
was built flavor-independent; the flavor is one branch at one choke point.

- **`pty_create(cols, rows)`** stays syscall 44 and creates GRID; syscall
  56 creates STREAM with the same two arguments. Old binaries do not pass
  a mode register. libos64 exposes `os64_pty_create` and
  `os64_pty_create_stream`.
- **A STREAM slave's output is a pipe wearing a tty's identity.** The slave
  owns a `pipe_t`, and the master's `read()` is a `pipe_read`. Geometry lives
  in the tty's dimensions; STREAM allocates no cell or scrollback ring.
  `tty_write` selects the stream before the direct-to-glass fallback.
  Resize updates dimensions without allocation, and unchanged dimensions
  neither bump the generation nor send SIGWINCH in either mode.
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
  ENDED, the STREAM spelling of `OS64_PTY_HUNGUP`. New seats are refused
  after this transition, including through `os64_spawn_seated`; start a new
  session on a fresh master. A spawn reserves its seat before the ELF load,
  serializing with the last-seat departure. Failed loads return that seat
  without making an unused pty appear previously occupied. The pipe close
  runs outside the pty list lock, with a hold protecting the slave while its
  reader wakes. GRID ptys still permit re-seating. When the master closes,
  the seats get SIGHUP as before AND the pipe's read end closes, so a child
  that writes after the terminal left gets `PIPE_ERR_CLOSED` instead of
  parking forever on a ring nobody will drain.
- **`pty_snapshot` on a STREAM pty is refused**, the mirror of `read()` on
  a GRID one: a byte stream has no screen. `pty_resize` works on
  both (SIGWINCH is about geometry, not rendering).
- **The master's read takes a `read_for` patience** — `pipe_read` carries a
  deadline, and a STREAM master is on read's honor roll beside the net
  handles and the console. telnetd's outbound thread reads the master with a
  short one so a reply the inbound thread queued reaches the wire while husk
  is idle. A plain pipe handle's read still refuses a patience: no consumer
  has asked, and a patience accepted and not kept is a lie with a delay.

**A handle a thread is inside is PINNED** (handle.c § The pin — Codex #101
rd4, and the reason this section exists). telnetd is the first program in
os64 that shares handles between threads and parks in a syscall while its
task tears down, and it found what that exposes: a syscall used to resolve a
handle to a bare pointer, and a sibling's close could free the object while
the first thread was still inside it — the outbound thread parked in
`read(master)` woke into a freed pipe once teardown closed the master and
husk's SIGHUP closed the last writer; spawn's TCP reference raced the same
close. Now `handle_pin` checks the slot and takes the object's OWN reference
in one critical section under the task's `handleLock`, and `handle_close`
claims the slot under the same lock before releasing the table's reference.
An operation in flight is a holder: the parked reader is the pipe's last
reader and frees it on the way out, a pty stays unburied while an operation
is inside it from either side (`holds` — the pin's `pty_master_hold`, and
`pty_seat_hold` for a seated task's own console read or write, whose seat
would otherwise stop protecting it the moment a sibling's teardown dropped
it), a spawn's SET_TTY master stays pinned across the whole ELF load and
its seat is reserved before loading, while
its three redirections are SHARED — the child's own reference on each,
taken in the same critical section that finds the parent's slot live
(`handle_share`), because a pin keeps a net conn's row but not its line
and the child needs the line. Every
handle type has its currency — pipe ends, file and directory
`handleRefCount`, a listener's `busy`, a join object's `refcount`, a TCP
conn's `pins` and a UDP or ICMP conn's `holders` — and the console tags
reference nothing. The net conns keep their pin apart from their handle
count on purpose: a sibling closing the last handle still hangs up (FIN,
unbind), and a reader or writer parked on the conn is woken to a CLOSED
verdict rather than left waiting on a silent peer; the pin only keeps the
row from the reaper until the operation leaves. The STREAM pipe itself is held side-lessly by its pty
from birth to burial (pipe.h `holds`), which is what lets a seated writer
take a writer's reference on it whatever the ends have done.
`/tests/pintest` drives the three shapes without a network.

Fatal writes return their death reason to the syscall wrapper, which drops
the pin before terminating. Default SIGPIPE keeps its exit status of 141;
caught SIGPIPE still returns progress or `OS64_INTERRUPTED`. After a child
is published, spawn checks its actual terminal under a lifetime hold for a
master close that preceded publication. This covers inherited PTYs as well
as explicit seating; a later close finds the child in the ordinary sweep.

## 3. telnetd — the 1969 protocol on the 2026 seams

`/bin/telnetd [port]` announces (23 by default) and loops on accept. At most
16 session children run concurrently; excess accepted connections
are closed. Reaping a child releases its process slot, including while the
listener is idle; the TCP storage charge remains until its rings are freed.
For each admitted connection it spawns **`/bin/telnetd -session`**
with handles 0 and 1
= the connection and 2 inherited, which is inetd's 1986 model and
Bernstein's tcpserver's after it: the socket IS stdin and stdout, and the
session program never learns it is on a network. os64's rule that a child
gets 0/1/2 plus exactly what was asked for is that model already.

The session: `os64_pty_create_stream(80, 24)`, `os64_spawn_seated("/bin/
husk")`, then two threads — the main thread reads the connection and feeds
the master, the second reads the master and feeds the connection — because
each side blocks on one source and os64 has threads. **The second thread is
the SOLE connection writer.** The kernel's per-connection lock stops two
writers corrupting memory, but a write is not atomic under backpressure
(`tcp_conn_write` copies what fits, drops the lock, resumes), so a second
writer could split an outbound doubled-IAC and produce an invalid stream.
So the inbound thread never writes the connection: the negotiation replies
it produces go into a lock-free single-producer mailbox, and the outbound
thread drains that mailbox to the socket alongside husk's bytes. The initial
offers are written directly by the main thread before the outbound thread
starts, so the handshake races nothing; a rare mid-session reply flushes on
the next husk output. Whichever direction ends first exits the task; the
kernel's close-all then hangs up the shell (master close → SIGHUP) and sends
the FIN (connection close), in whichever order the table holds them. Nothing
here needs a select.

**The NVT translation** (RFC 854, both directions):

| Inbound (client → shell) | Outbound (shell → client) |
|---|---|
| CR LF and CR NUL become LF; a lone NUL is dropped (the engine already does this) | LF becomes CR LF |
| IAC IP becomes 0x03 — the Ctrl+C intercept fires on the slave | 0xFF is doubled |
| IAC EC / EL become Backspace / Ctrl+U; AYT is answered; AO and BRK are consumed | |
| Negotiated SB NAWS changes geometry; changed dimensions send SIGWINCH | |

**Negotiation reuses the client's engine** (`apps/telnet/telnet_protocol.c`,
host-tested at every chunk size) with a SERVER ROLE (`telnet_init_server`,
`telnet_offer_server`): the server says WILL ECHO, WILL SGA, DO SGA, DO
NAWS; `we_agree`/`he_may` become role-aware so it agrees to ECHO and SGA as
ours and NAWS and SGA as his, and refuses the rest by name. The RFC 1143
machinery is symmetric and stays one copy. What the role adds to the engine:
an inbound NAWS subnegotiation, while the peer's option is enabled, is
PARSED (cols, rows, IAC-doubled dimensions
and all) into `telnet_peer_size` and a `TELNET_NOTE_RESIZE` notice, where
the client engine only ever skipped a subnegotiation to throw it away. ECHO
is offered by the server because echo is the reader's job in os64 — husk
echoes what it reads — and a client that echoed locally as well would show
every key twice. Inbound line endings are the engine's job too in the server
role: CR LF, CR NUL and a lone CR all collapse to one `\n`, or husk reads a
client's newline as two Enters and answers with a second empty prompt. The
outbound half (husk's bytes → client) needs no negotiation state — a bare LF
becomes CR LF, a 0xFF is doubled — so telnetd does it inline, in the sole
connection-writer thread described above.

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

- **Kernel self-test:** none for the listener yet — the stack has no
  loopback (a dial to our own address goes to the wire and is not looped
  back), so the machine cannot accept its own connection. Booked; the host
  script is the fixture until then.

`tools/test_telnet_host.sh` covers both roles, including NAWS with escaped
255, refusal/withdrawal, and command delivery with a full decoded buffer.
`tools/telnetd_probe.py` drives dropped sessions, DONT ECHO, and input floods.
`tools/test_telnetd_limits.py` adds a bounded acceptance run for an idle
guest: 16 live sessions, eight excess peers refused, reuse after a child is
reaped, negotiated resize, a 5,001-frame resize burst, and EC/EL at husk.
`tools/test_telnetd_storage.py` holds 31 closing connections after their
session children exit, checks storage refusal with one observer connection,
and verifies admission resumes after a peer completes its FIN exchange.

**The pin, proven 2026-09-13** (the Codex #101 rd4 P1s — § 2 above):

- `/tests/pintest` in the ring-3 suite: a thread parks reading a pipe, a
  stream pty master, and a thread handle while its sibling closes the handle
  under it. Before the pin each shape was a tripwired ring-0 use-after-free;
  now each reader gets the ordinary answer (EOF, EOF, the worker's value).
  The suite: 46 passed, 0 failed, 2 skipped; the kernel's own 30 + 29 + 3.
- `tools/telnetd_probe.py 2323 session N` — N logins that each run `ls /`
  and then DROP the socket with husk still seated, the teardown-under-a-
  parked-reader shape itself. 60 in a row: no panic, `ps` clean after each
  batch, `/sys/net/tcp` accepted == reaped, and `free` moved by 3–4KB per
  batch of twenty, not per session (a 64KB step once, a block-cache line).
  The LATE-phase `task_teardown_leak` passes on an idle machine; it fails
  if the probe is run DURING it, by design — it measures the whole
  machine's free-page count and says so.
- `tools/telnetd_probe.py 2323 dontecho`: the server's explanation arrives
  before the FIN, then EOF.
- `tools/test_telnet_host.sh` gained the server role: the opening offers,
  ECHO taken and refused (the notice), NAWS as a resize notice, Enter's
  three spellings and IAC IP at every chunk size.

## Round 11 validation — 2026-09-13

This follow-up starts at `0f65199`. All 47 review threads were read: 42
previously resolved (including the STREAM re-seat race then recorded in
DECLINED.md, addressed in round 13 below), and five current findings accepted
against that head.

- PTY creation holds the slave before publishing the master and releases
  the hold after its diagnostic. A commit that closes itself, a cancelled
  reservation, and a later table sweep all preserve that hold. The sibling
  accept path captures peer identity before publishing its TCP handle.
- The listener caps session children at 16 and releases capacity on reap.
- NAWS is consumed as geometry only while the peer's option is enabled.
- STREAM has no cell allocation; resize changes geometry without rebuilding
  cells. Repeating the current size leaves generation and SIGWINCH alone.
  GRID continues to preserve text, cursor and scrollback on a changed size.
- IAC EC and EL deliver Backspace and Ctrl+U in the server role, retaining
  the command when the decoded output buffer is full.

Validation on the follow-up tree:

- Full strict `make -j8` passed. `git diff --check` passed. The retirement
  scan found only the intentional `PTY_MODE_*` enum-family comment.
- The added NAWS/erase regressions produced 70 failures before the protocol
  fixes. Afterwards `tools/test_telnet_host.sh` passed, including 1,600
  differential comparisons and five localhost scenarios. ASan/UBSan were
  enabled; leak detection was disabled because LSan cannot run under this
  environment's tracing. The TCP-options host suite also passed.
- Private q35 QEMU, 8 cores, 2 GiB, virtio networking and localhost port
  2323: kernel tests **30 pre-boot + 31 post-boot + 3 late, zero failures**.
  New `pty_publication_hold` covers both PTY modes and the three publication
  outcomes; `pty_resize_modes` covers 1,024 alternating STREAM resizes with
  repeated unchanged requests, preserved pipe output and GRID text retention.
- `/tests/testrun`: **46 passed, 0 failed, 2 skipped**, including winchtest.
  `/tests/pintest` passed; `/tests/ptyprobe` reported **16 passed, 0 failed**.
- `tools/test_telnetd_limits.py`: 16 concurrent shells, eight excess peers
  closed, capacity reused after reap, NAWS before WILL and after WONT
  ignored, enabled NAWS applied, 5,001 resize frames followed by correct
  geometry, and EC/EL verified against husk.
- Existing `telnetd_probe.py` DONT-ECHO, flooded-input disconnect and five
  dropped-session runs passed. The final TCP snapshot contained the observer
  connection and no leftover test connections. Faults in the guest suite's
  intentional protection tests are expected; no kernel panic occurred.

Evidence is in `/tmp/pr101-rd11/`, with the strict build in
`/tmp/pr101-rd11-build.log` and protocol before/after runs in
`/tmp/pr101-telnet-before.log` and `/tmp/pr101-telnet-after.log`. These are
private QEMU results; the fixes have not been deployed to the P5.

## Round 12 validation — 2026-09-13

At `7beacea`, the 48th review thread identified a gap in the process cap:
reaping a Telnet child leaves TCP rings allocated while its detached close
waits for the peer. The Telnet comment claiming the child count bounded TCP
storage was wrong. The separate passive storage limit in section 1 follows
the ring lifetime, including across listener replacement.

- The new host regression failed against the previous TCP implementation:
  a SYN allocated another pair of rings after 32 detached connections.
  With the fix, the full TCP host suite passed under ASan/UBSan with leak
  detection disabled for tracing. It covers withheld FIN ACKs, listener
  replacement, shared half-open capacity across ports, normal FIN completion,
  reset after stripping without double release, and timeout recovery.
  The harness also gained the missing port-refusal ABI constant and a live
  handle count for fixtures that exercise handle-aware writes.
- Full strict `make -j8` and `git diff --check` passed.
- Private q35 QEMU, 8 cores, 2 GiB: **30 pre-boot + 31 post-boot + 3 late
  kernel tests passed**, and `/tests/testrun` reported **46 passed, 0 failed,
  2 skipped**. No kernel panic occurred.
- `tools/test_telnetd_storage.py` ended 31 sessions in 8.8 seconds while
  withholding the clients' FINs. Together with the observer, their rings
  filled the 32-connection limit; an excess SYN was counted and received no
  Telnet session. Completing one peer's FIN exchange freed capacity and a
  replacement shell ran a command. These real guest connections reached
  FIN_WAIT_2; the host regression separately withholds the server-FIN ACK.
- `tools/test_telnetd_limits.py` passed its existing session-cap, resize,
  erase and reuse checks. The final TCP snapshot contained one observer,
  `passive_buffered: 1`, and no leftover test connections.

Evidence is in `/tmp/pr101-rd12/`, `/tmp/pr101-rd12-before.log`,
`/tmp/pr101-rd12-after.log` and `/tmp/pr101-rd12-build.log`. These fixes
have been validated in private QEMU, not deployed to the P5.

## Round 13 validation — 2026-09-13

The 49th review thread, against `fea1fb1`, demonstrated sequential re-seating
through the public API after STREAM EOF. The claim that nothing could
re-seat a stream was wrong. The EOF transition now refuses further seats;
the reservation and last-seat decrement share the pty list lock. This also
addresses the concurrent re-seat case previously declined, so that entry
has been removed from DECLINED.md. A failed first ELF load returns its
reservation without closing a previously unused stream. GRID reuse remains
supported.

- `/tests/streamseat` failed on the previous kernel with "spawn accepted
  after STREAM EOF". It now passes failed-load retry, output draining to
  EOF, rejection after EOF, and two successive GRID seats. It is included
  in `/tests/testrun`, which passed **47 tests, 0 failures, 2 skips**.
- The `pty_stream_seats` kernel regression checks cancellation on an unused
  stream, reservation before last-seat departure, cancellation after the
  occupied seat leaves, refusal after EOF, and refusal after master close.
  Its initial polling fixture used the wrong kernel deadline; the corrected
  check uses an expired absolute tick, not the public API's zero-timeout
  convention.
- The test registry capacity increased from 64 to 128: the new test must
  not displace `backstop_preemption`. The final private q35 QEMU boot,
  8 cores and 2 GiB, passed **30 pre-boot + 32 post-boot + 3 late tests**,
  including both tests. No kernel panic occurred.
- The existing Telnet limits, resize, erase and capacity-reuse probe passed.
  Five final login/`ls /`/disconnect sessions passed with time for child reap
  between sessions. Final `ps -ef` had only the observer session; TCP showed
  its one buffered connection plus three stripped CLOSED diagnostic rows.
- Full strict `make -j8` and `git diff --check` passed. The stale-reference
  scan's `SET_TTY` hits are intentional shorthand for `OS64_SPAWN_SET_TTY`;
  the flag remains live and those descriptions still apply.

Evidence is in `/tmp/pr101-rd13/`, including `before-test.log`, `testrun.log`,
`final-boot.log`, `final-probe.log`, `cleanup.log` and `final-build.log`.
These are private QEMU results; the fix has not been deployed to the P5.

## Round 14 validation — 2026-09-13

Two findings against `2f77993` exposed sibling paths omitted by the earlier
fixes. The claim that fatal writes unpinned before death was false for
default SIGPIPE; the explicit-seat hangup recheck did not cover inheritance.

- Write returns a named death reason to its wrapper. The wrapper unpins
  first, then applies default SIGPIPE or the pending terminating signal.
  Default SIGPIPE still exits 141; catchable SIGPIPE and partial progress
  keep their existing behavior.
- After scheduler publication, spawn holds the child's actual terminal by
  registry lookup and checks master closure for inherited and explicit
  PTYs. A child that has already exited cannot leave this check dereferencing
  a buried slave. The hold is released after the check.
- `tools/test_syscall_lifetime_host.py` compiles the actual pipe-write case,
  pin wrapper and spawn publication tail with controlled lifetime seams.
  Both regressions failed before the fix: a live pin at death, and no hangup
  for an inherited seat closed before publication. ASan/UBSan now pass
  default death, interrupted death, handled SIGPIPE, partial progress,
  failed copying, explicit/inherited hangup, and open/buried/VT terminals.
  Leak detection is disabled for the host environment's tracing.
- Private q35 QEMU, 8 cores and 2 GiB: **30 pre-boot + 32 post-boot + 3 late
  kernel tests passed**; `/tests/testrun` passed **48 tests, 0 failures,
  2 skips**. `/tests/pipeexit` adds 32 default-SIGPIPE deaths with status 141.
  Read-only GDB inspection of the live pipe list before and after a separate
  32-death batch found **one pipe both times**, the observer's STREAM pty.
- Five Telnet disconnects during `/bin/sleep 60` startup left no surviving
  sleep process. After reaping settled, `ps -ef` showed only the observer
  session and TCP reported one buffered connection. The host publication
  seam, rather than timing these guest runs, proves the exact missed-sweep
  ordering.
- Full strict `make -j8` and `git diff --check` passed. The stale-reference
  scan reports the live flag's intentional `SET_TTY` shorthand.

Evidence is in `/tmp/pr101-rd14/`, including before/after host logs, the
strict build, guest suite, kernel log, pipe-count GDB script and snapshots.
These fixes have not been deployed to the P5.

## Round 15 validation — 2026-09-13

Two findings against `b9bf056` concern task storage after spawn publication
and a held carriage return during Telnet's negotiated shutdown.

- Spawn takes a task lifetime hold before scheduler submission and releases
  it after the terminal check and PID capture. Collection can proceed, but
  the undertaker cannot unlink or bury a held task. This hold protects task
  storage; terminal lifetime still requires its separate registry hold.
  The old comments claiming one worker pass guaranteed a full sleep interval
  were wrong and are corrected in the task code and teardown debt entry.
- The outbound Telnet bridge resolves a held CR as CR NUL before mailbox
  bytes reach the wire and before negotiated shutdown with an empty mailbox.
  This also covers a notice drained before its producer publishes the end
  flag. Idle-timeout and EOF flushing share the same helper.
- The production spawn-tail regression fails on the old code when a child
  is collected and buried at publication. It now checks storage survival
  through terminal access and PID capture, including burial at final release.
  A second host harness compiles the actual task hold/release and two-phase
  reaper: collected, auto-reaped and orphaned held tasks survive repeated
  passes while unrelated burial proceeds; final release permits burial.
- The production outbound-loop regression failed on the old code's missing
  CR. Six deterministic cases now pass: notice plus end flag, notice before
  flag, idle timeout, split CRLF, EOF and shutdown with an empty mailbox.
  Every socket write is forced short. All three focused harnesses pass
  ASan/UBSan with leak detection disabled for the host tracing environment.
- The existing Telnet host suite passes its 1,600 differential schedules
  and five local socket scenarios. New ring-3 fixture `/tests/spawnreap`
  verifies 128 returned child PIDs against concurrent sibling reap results.

The full guest run also exposed an existing `wait()` race: a child can set
`exited` before it enters the dead-child list, or exit between separate dead
and live probes. Both can falsely report "no such child". Wait now checks
for an uncollected child under the same graveyard lock as the dead-status
probe, retaining an exiting child as a valid target until collection.
`tools/test_task_wait_host.py` fails against the pre-round-15 kernel source
and passes both transition orders plus missing/already-collected refusals
with the fix. STREAM-seat and TAR pipeline fixtures now print the PID, wait
result and status on failure; no retry masks a failed wait.

Final validation on private q35 QEMU with 8 cores and 2 GiB passed **30
pre-boot + 32 post-boot + 3 late kernel tests** and **49 userland tests,
0 failures, 2 skips**. A real DONT ECHO peer received the closing notice and
EOF; settled process/TCP snapshots showed just the observer session and one
buffered connection. Full strict `make -j8`, `git diff --check`, the
stale-reference scan and read-only `make fsck-ext2` checks of root and home
passed. The initial failing guest runs and the before/after wait regression
are retained alongside the final passing results.

Evidence is in `/tmp/pr101-rd15/`. These changes have not been deployed to
the P5.

## Booked (DEBTS.md rows follow the code)

UDP announce; announce on one address of several; SYN cookies; loopback
(and with it an in-OS listener fixture); a deadline on plain pipe-handle
reads (the STREAM master's has one); `/sys` rows for ptys; the STREAM
slave's kernel-text drop counter surfacing somewhere readable.
