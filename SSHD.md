# SSHD — remote commands and interactive shells

Design: Fable, 2026-09-13. Implementation: Quinn, originally stacked on
`fable/servers` at `8b5e974` in `.worktrees/sshd`, branch `codex/sshd`.
This record incorporates the server-branch API audit into Fable's design.

The primary consumer is a command runner:

```sh
ssh -i ~/.ssh/id_ecdsa os64@p5 'cat /sys/net/tcp'
ssh -i ~/.ssh/id_ecdsa os64@p5 'false'
echo $?  # 1
```

`ssh -t -i ~/.ssh/id_ecdsa os64@p5` opens an interactive husk on a STREAM
PTY. There is one session channel per connection, beside up to seven
`direct-tcpip` forwards to this machine's loopback (§ Local forwarding). The
daemon is userland;
the kernel addition is the `SSHD` token's existing-style launch hook. No new
syscall, pipe operation, PTY operation, or cryptographic ABI is introduced.

## Identity, enrollment, and starting the service

The client key must be ECDSA P-256:

```sh
ssh-keygen -t ecdsa -b 256 -f ~/.ssh/id_ecdsa
```

Enroll the **public** line from `id_ecdsa.pub` in the target's
`/home/authorized_keys` through an existing trusted access path. Do not copy
the client private key to the target. The files named `authorized_keys` on
the configuration ladder are merged, with duplicates harmless. The image
ships no authorized keys. Supported lines are:

```text
ecdsa-sha2-nistp256 <base64 public-key blob> [comment]
```

Other recognizable key types are skipped with their type logged. Options
such as `from=`, `command=`, and `restrict` refuse the key file; they are not
stripped into unrestricted grants. Invalid supported keys, truncated lines,
and excess key/file capacity also refuse the session. There are at most 64
distinct enrolled keys; each file is at most 128 KiB and each line fits 2047
bytes. The outer type, inner type, curve name, point length, and P-256 point
are validated before enrollment.

`/home/sshd_host_key` is persistent machine state, outside the config ladder:

```text
ecdsa-p256 <64 hexadecimal digits>
# generated epoch <UTC seconds>
```

The listener reads this key or generates it on first start using seeded
`/dev/random` and BearSSL's EC key generator. Exclusive creation prevents
replacement of an unreadable or concurrently created identity. A malformed,
partial, oversized, or unreadable existing file is refused. A persistence
failure does not silently generate a replacement on the next boot. The
public point is recomputed, and its OpenSSH `SHA256:` fingerprint is printed
at startup. Compare that fingerprint with the client's first-connect result.
The key is not an OpenSSH private-key file.

`sshd.conf` uses the ordinary configuration ladder; the shipped copy is
written to the ext2 root's `/etc` by the image build, and the FAT lifeboat
carries none, since it runs no services:

```text
port = 22
forward = loopback
```

`forward` is `loopback` (the default when the key is absent) or `none`; any
other value refuses the file, as an unreadable one does. The listener reads
the file once and hands each session the policy on its command line.

Start `sshd &` at a prompt, or add `SSHD` to the intended boot entry's kernel
command line. Standard boot entries do not automatically enable the service;
the lifeboat remains without it. **Building this branch does not deploy it
to P5. The P5 boot menu does not travel with an ordinary refresh.**

A public key grants machine access. os64 has no user accounts: the requested
username is recorded for an accepted login but is not an authorization role.
Password and keyboard-interactive authentication are refused without prompts.

## Protocol contract

The supported client is OpenSSH 9.6p1, including the Ubuntu
`3ubuntu13.19` installation used for validation. No algorithm override is
needed for an ECDSA P-256 client key.

| Slot | Algorithm |
| --- | --- |
| Exchange | `curve25519-sha256` |
| Host and authentication key | `ecdsa-sha2-nistp256` |
| Cipher in each direction | `aes128-ctr` |
| MAC in each direction | `hmac-sha2-256` |
| Compression | `none` |

The server identifies as `SSH-2.0-os64sshd_1.0`. It advertises
`kex-strict-s-v00@openssh.com` in the initial exchange, recognizes the client
counterpart there, and resets each direction's packet sequence at NEWKEYS
when strict KEX is negotiated. Under strict KEX the initial exchange refuses
IGNORE, DEBUG and UNIMPLEMENTED, as OpenSSH does; a rekey, and a peer that
did not negotiate strict KEX, accept them at any time after identification
(RFC 4253 section 11). Any message the current state does not handle,
known type or not, is answered with UNIMPLEMENTED, before authentication
and during an exchange alike, as OpenSSH answers; the one exception is
strict KEX, which ends the initial exchange on any unexpected packet. The
rule names no message numbers. The client identification line may be the RFC's full
255 bytes including CRLF. Extension-info is not advertised. Cipher and
MAC state belong to separate receive/transmit contexts.

BearSSL is linked through the private foundation archive. AES uses
`br_aes_ct64_ctrcbc_ctr`, whose counter carries across the full 128 bits;
the 32-bit-counter CTR entry point is unsuitable. The MAC authenticates the
sequence and plaintext SSH packet before encrypted input reaches auth or
channel handling. X25519 rejects wrong-length and all-zero shared secrets.
The X25519 output bytes are interpreted in network order for SSH's mpint,
without reversing that byte string. BearSSL's scalar API is big endian;
RFC 7748's scalar test vector is converted at the test boundary.

The exchange hash covers the identification strings, complete KEXINIT
payloads, host public blob, both ephemeral points, and the mpint secret.
ECDSA signs SHA-256 of that exchange hash. Raw r/s values are encoded as
canonical positive SSH mpints, including the maximum case where both need a
leading zero. Key derivation uses RFC 4253's A through F labels. The first
exchange hash remains the session ID through rekeys.

Each connection obtains a fresh 32-byte `/dev/random` seed for its HMAC
DRBG. Unavailable entropy prevents session initialization; there is no weak
seed fallback. The DRBG supplies cookies, ephemeral scalars, and padding.
Private scalar copies, pending derived keys, and the engine are wiped when
those lifetimes end normally.

Authentication validates the signature over `string session_id` plus the
original userauth request prefix. An enrolled-key query is not an
authentication success. Five failed requests or 120 seconds without success
end the connection. The listener caps concurrent session children at 16.

Client-initiated rekeys are supported while a command or shell is running.
The server also initiates after 512 MiB in either direction or one hour.
A KEX has a separate 120-second completion deadline. Connection replies
received in flight before the client's KEXINIT are deferred until NEWKEYS;
application output pauses during KEX. A channel close during that exchange
keeps the transport running until NEWKEYS releases the deferred close reply
or the KEX deadline expires. A connection that is ending (its session
closed and no forwards live) does not initiate another server rekey.
The deferred reply budget is 8 KiB of stored replies. Their framed wire cost
is tracked alongside, and receive takes no further packet until the output
queue has room for one maximum packet plus that whole replay, so NEWKEYS can
always release it; the budget's worst case fits the queue with room to spare.

## Commands, channels, and backpressure

The server advertises a 2 MiB receive window and 32768-byte data packets.
Peer window and maximum-packet limits constrain outgoing data. stdout and
stderr take turns spending shared credit; priority advances once per service
pass, away from the first stream that sent bytes, so neither sparse window
updates nor credit just over one staging buffer can keep favoring one
stream. The session's streams and the forwards take turns going first for
the output queue's room by the same rule, so a trickle of room is never all
one group's. The lead passes because every channel sends what fits the
free room rather than all or nothing: a channel that waited for room for
its whole staged packet would never spend any, and so never hand over. Credit for input the command took is owed until it can go out
as one WINDOW_ADJUST: never during a key exchange, where each adjust would
wait in the deferred-reply budget and enough of them would exhaust it, and
never without output room. Window
addition overflow, data exceeding the advertised window, invalid channel
numbers, malformed lengths, and invalid protocol transitions are refused.
Transport packet length is capped at 35000 bytes. KEX name-lists are capped
at 4096 bytes and names at 64; userauth and channel requests are capped at
8192 bytes.

`exec` spawns `/bin/husk -c <command>` with three independent pipes.
Channel data feeds stdin; stdout uses CHANNEL_DATA; stderr uses
CHANNEL_EXTENDED_DATA type 1. SSH EOF closes stdin after queued input drains.
After the child exits and both output streams drain, the server sends
`exit-status`, EOF, and CLOSE, in that order. A close handshake has a bounded
wait. Backpressured network output also has a finite no-progress deadline.

**Audit corrections to the original design:**

- This branch limits both a spawn argument and husk's command line to 255
  bytes. SSH refuses longer commands before spawning; it does not advertise
  the draft's 4 KiB allowance. Shell syntax is husk's: `exit N` ignores N and
  `>&2` is not POSIX descriptor duplication on this baseline. The SSH
  fixture's `-streams` and `-stderr` modes test status 7 and raw stderr
  without relying on those shell features.
- Ordinary pipe reads reject finite patience on the server branch. stdout
  and stderr therefore have dedicated blocking readers, each feeding a
  bounded 64 KiB SPSC queue. A separate stdin writer owns a bounded 2 MiB
  queue. The main event loop alone reads/writes the socket and owns the
  engine, eliminating cross-thread cipher/rekey coordination. Worker queue
  publication uses release/acquire ordering. Kernel handle pins protect
  outstanding operations during task teardown.
- os64 renders a newline as a new row at column zero. The interactive
  adapter emits CR LF for a lone LF so OpenSSH's raw terminal renders that
  same motion, preserving existing CR LF pairs across read boundaries.
  This is terminal output adaptation; exec stdout/stderr stay byte-exact.
- An exec request with an allocated PTY is refused; the interactive path is
  `pty-req` followed by `shell`. Shell requests without a PTY are refused.
  `pty-req` records TERM and dimensions, and the seated shell is spawned
  with TERM set to that name when it is a plain printable one (a value off
  the wire is data until it passes that test); `window-change` resizes the STREAM
  PTY. Nonzero dimensions must fit the kernel's 2–512 columns and 2–256
  rows; zero preserves the last accepted component (or initial default).
  A live resize commits geometry and reports success only after the PTY
  accepts it. Before shell startup, a valid resize updates its initial size. Terminal-mode opcode framing is checked, while os64 owns the line
  discipline. Interactive EOF uses Ctrl-D; a STREAM master lacks an
  independent write-half close.
- Disconnect closes command pipes. A non-PTY command that does not interact
  with those pipes can outlive the connection; this is not a job-cancellation
  interface. Interactive teardown closes the PTY master and hangs up the
  seated shell.

Channel types other than `session` and `direct-tcpip`, and second session
channels, receive OPEN_FAILURE. Unsupported global requests, including
`tcpip-forward` (remote forwarding, `ssh -R`), receive REQUEST_FAILURE when
requested. Unsupported channel requests, including env, subsystem/SFTP, agent
forwarding, signal, break, and xon-xoff, receive CHANNEL_FAILURE when
requested. No passwords, per-user access control, Ed25519, SFTP/scp,
compression, remote forwarding, or multiplexed sessions are implemented.
Reversing conditions are recorded in DEBTS.md.

## Local forwarding (`direct-tcpip`, 2026-09-23)

`ssh -L 5900:localhost:5900 os64@p5` reaches a service on the P5's loopback.
It is REMOTE.md's tunnel, and anything else announced on `127.0.0.1` can use
it too. The session is local channel 0, and forward *i* is local channel
*i + 1*, seven at most. An eighth open receives OPEN_FAILURE
`RESOURCE_SHORTAGE`.

- **The destination is judged by its address, never its spelling.** The
  engine refuses on sight a host that is empty, over 255 bytes or not
  printable, and a port of 0 or above 65535, and hands the rest to the
  daemon. The daemon resolves the host with `os64_resolve`, accepts only
  `127.0.0.0/8` (`localhost` passes because `/etc/hosts` says 127.0.0.1),
  and dials. A refused destination is `ADMINISTRATIVELY_PROHIBITED`
  ("forwarding reaches this machine's loopback only") and is logged; a
  dial that fails is `CONNECT_FAILED` with `os64_dial_reason`'s words. A
  name that has to go to DNS blocks only the session asking.
- **Windows.** Each forward advertises 256 KiB, sized for keystrokes and
  requests, not the session's 2 MiB. Outgoing data obeys the peer's window
  and packet size and is sized to the output queue's free room, as the
  session's is (see Commands, channels, and backpressure), and a window
  overrun or overflowing
  adjustment disconnects, the session's rules. Credit for bytes the local
  connection took follows the session's rule: owed through a key exchange
  or a full queue and sent as one adjust, never dropped, or the client's
  window would shrink for good. Each forward's two queues (256 KiB
  toward the local connection, 32 KiB toward the client) are allocated when
  it opens and freed when it closes, which keeps the program inside its
  link slot.
- **Closing.** When the local connection ends, EOF and CLOSE are sent once
  everything read from it has gone out; like exit-status they wait out a key
  exchange. Both CLOSEs crossing ends the channel, but the slot is freed
  only after the bytes the client sent before its CLOSE have been delivered
  to the local connection (or it failed): a client may CLOSE without an EOF
  first, and its last bytes are still owed. **A client EOF
  ends the local connection whole** once the client's bytes are delivered:
  os64 has no ring-3 half-close, so a reply the local service would have
  sent after a half-close is lost (DEBTS). A viewer or browser that closes
  fully loses nothing.
- **Lifetime.** A closed session does not end the connection while forwards
  are live; `ssh -N` opens no session at all. The connection ends when the
  client leaves. The server's rekey limits hold for the connection, so
  forwards outliving the session still rekey.
- **Channel numbers.** Every reply to an open is addressed by the client's
  number for it, so an open (session or forward) that names a number one of
  the client's live channels already uses ends the connection; a number is
  live until both CLOSEs for it have crossed, and then it may be used again.
- **The loop naps only on a pass that moved nothing.** It used to nap a
  tick every pass, which is fine for a shell but capped a forward at one
  read per tick.

Proof: `sshtest`'s forward matrix (policy, confirmation fields, data and
window credit, send clamping, requests, both close orders, EOF, window
violations, refusals, destination shape, seven-and-an-eighth, slot reuse,
a session alongside, KEX holding a close, credit kept through a full
queue and a key exchange, sends sized to the free room, a closed slot held
until release, channel numbers live and dead). The
daemon-loop harness gained `forward` (the config key), `linger` (a closed
session with a live forward keeps the connection and yields rather than
naps), `closedrain` (a closed forward delivers its queue, then is released
once) and `mixedfair` (a session and a forward that always have more than a
pass's room each get a share within 2x; the round-1 daemon, which always
served the session first, split it 5.4 to 1). The host
adapter mirrors the daemon's rules, and real OpenSSH drives `ssh -N -L`:
3 MiB each way, the same across client and server rekeys, seven concurrent
forwards, the eighth refused, a non-loopback destination and a closed port
refused, and a session sharing the connection. In QEMU, host `ssh -N -L`
through the port forward to `looptest echo 127.0.0.1` echoed 3 MiB each way,
and the two refusals came back with their reasons.

## Validation and review boundaries

```sh
tools/test_sshd_host.sh --build-dir /tmp/sshd-host
python3 tools/sshd_probe.py 127.0.0.1 --port 2222 \
  --key /tmp/sshd-host/client --known-hosts /tmp/sshd-qemu/known_hosts
```

The host build runs the same pure engine and BearSSL primitive sources
under ASan/UBSan. A loopback-only POSIX adapter uses fixed **test** host-key
and DRBG inputs; it is not a second deployable daemon. OpenSSH tests cover
authentication, separate stdout/stderr, exit status, refusal paths, binary
streams larger than the window, repeated forced rekeys, and independent
host-fingerprint serialization. The host driver runs local test commands;
its keys and known-hosts file are generated in the temporary test directory.

`/tests/sshtest` runs the codec/crypto/engine fixture inside os64. Additional
fixture modes supply explicit stdout/stderr markers with status 7 and copy
binary stdin to stderr. `sshd_probe.py` checks these, large binary transfers,
interactive shell/resize, and a fresh command after an abruptly dropped
interactive client. `--fixture` selects an explicitly installed test copy.

An initial fixture named `sshdtest` exposed a separate task-address collision:
its assigned link base equalled `USER_TASK_MEMORY_BASE` (`0x10000000`). Its
first PLT fetch encountered zeroed NX memory, and cleanup panicked in the
allocator. Renaming it to `sshtest` places this fixture at `0x22000000` on the
stacked build. That avoids the observed collision; it does not fix the
underlying app-base/task-allocation overlap. DEBTS.md records the separate
kernel-scope follow-up.

External crypto/implementation review is still required before a production
merge. Host/QEMU checks and user-reported P5 results are recorded separately
below. Parent server PR #101 is merged; this branch includes its reviewed
implementation through `userland` at `893efea`. SSH implementation review
can now proceed against that base.

## Recorded validation — 2026-09-13

Original validation parent: `fable/servers` at `8b5e974`. Full strict
`make -j8` passed.
The complete staged diff passes `git diff --cached --check`. GCC's analyzer
reported no diagnostics for the transport and connection engines.

- Host ASan/UBSan fixture: **683 checks, 0 failures**. OpenSSH 9.6p1 passed
  separate stdout/stderr and status 7, `false` status 1, unenrolled/Ed25519
  refusal, overlong exec refusal, 3 MiB binary echo with repeated client
  rekeys, server-initiated rekeys with data in flight, oversized-window
  stderr output, unsupported PTY exec refusal, and independent fingerprint
  comparison. Evidence: `/tmp/sshd-host/final.log`.
- Private q35 QEMU, 4 vCPUs, 2 GiB, separate NVMe root and copied home
  images, loopback port 2222: **683 guest checks, 0 failures**; binary
  stdin/stdout and stderr each passed 3 MiB; explicit stream markers returned
  status 7; forced rekeys passed. Interactive Enter, CRLF rendering,
  88x30 to 103x41 live resize, and a fresh command after an abruptly dropped
  interactive client passed. Strict host-key checking succeeded after
  reboot with the retained home disk. Evidence:
  `/tmp/sshd-qemu/final-probe.log`, `/tmp/sshd-qemu/final-probe/pty.log`,
  `/tmp/sshd-qemu/serial-final.log`.
- `tools/stale_refs.sh` reported documentary references to SSH message
  names, the example `id_ecdsa` path, and the newly added probe script
  names. Each remains intentional and current; there were no new
  superlative claims. Evidence: `/tmp/sshd-stale-refs.log`.
- Chris deployed the branch binaries to the P5 and rebooted, enrolled an
  ECDSA P-256 public key, and confirmed SSH login and remote `ls /` output.
  A remote `top` command streamed updates every second. After about half an
  hour of exploratory use, he reported no SSH failures. These are
  user-reported hardware results, not an instrumented repeat of the host
  and QEMU suites.

The earlier fixture-address collision is recorded above and in DEBTS.md;
the final guest serial log contains no panic or segmentation-fault report.
External review and a production merge remain pending.

## Merge-forward validation — 2026-09-13

Server PR #101 merged into `userland` at `893efea` after Codex's clean
review of `46a2ccc`. Merging that base into `codex/sshd` required no conflict
resolution. It brings the reviewed TCP storage limits, PTY and handle
lifetimes, task publication holds, wait-race fix and server regressions into
the SSH branch. The SSH implementation required no code changes.

- Full strict `make -j8` passed. The SSH fixture remains linked at
  `0x22000000`; its separate app-base collision debt remains open.
- Host ASan/UBSan: **683 checks, 0 failures**, followed by the complete
  OpenSSH interoperability suite, including 3 MiB transfers, client/server
  rekeys, stream separation, status and refusal cases.
- Private q35 QEMU with 8 vCPUs, 2 GiB and copied root/home disks:
  **30 pre-boot + 32 post-boot + 3 late kernel tests passed**. The complete
  userland suite, invoked through SSH, passed **49 tests, 0 failures,
  2 skips**. The SSH guest fixture passed **683 checks, 0 failures**.
- The OpenSSH guest probe passed 3 MiB binary stdin/stdout with forced
  rekeys, 3 MiB binary stderr, stream markers/status 7, overlong-command
  refusal, interactive shell/CRLF/live resize and a fresh command after an
  abruptly dropped interactive client. Strict host-key checking used the
  retained test guest identity.
- Read-only ext2 checks passed for both copied root and home disks.
  Diff whitespace checks passed. The stale-reference scan's imported
  `PTY_MODE_*`/`SET_TTY` shorthand and bounded-loop comments remain current.

Evidence: `/tmp/sshd-merge/`, `/tmp/sshd-merge-build.log` and
`/tmp/sshd-merge-host.log`. The P5 results above cover the earlier branch;
this merged build has not been deployed there. SSH implementation review
and its production merge remain pending.

## Review round 1 — 2026-09-13

Four findings against `7d177ea` are addressed:

- Both daemon close paths wait for an active KEX before ending the channel
  handshake. A deferred reciprocal close is sent after NEWKEYS; a stalled
  exchange still reaches its 120-second deadline. Server rekey initiation
  stops after channel closing begins. The host adapter follows the same rule.
- PTY dimensions parse into locals and commit after the enclosing request
  is accepted. Pre-PTY window changes and invalid terminal modes cannot
  contaminate a later zero/unspecified size.
- Output stream priority advances after bytes are sent, not on idle loop
  iterations. One-byte window replenishments at different phases give both
  pending streams progress. The host adapter uses the same rotation rule.
- A socket flush interrupted by a caught signal retains the queued bytes
  for retry, like a timeout. Positive short writes consume only those bytes;
  EOF and other errors still end the connection.

The pre-fix dimension regression reported four failed checks. Each of the
new production-loop regressions (interrupted flush, close during KEX and
sparse-credit fairness) failed independently before the fix. The host loop
harness controls I/O and engine events; it does not simulate cryptography.
A separate real-engine fixture checks deferred CLOSE release by NEWKEYS.
The ordinary host runner now includes the loop harness automatically.

Host validation passes **704 engine checks** plus all three loop groups
under ASan/UBSan, and the full OpenSSH interoperability suite. Private
8-core QEMU passes **704 guest SSH checks** and the full 3 MiB/rekey/PTY
probe. Eight early-exit commands, each given 1 MiB of unread stdin, retain
complete output and status 0. The kernel boot suite passes **65 tests**.
Strict build, whitespace/stale-reference checks and read-only root/home
ext2 checks pass. Evidence and before/after logs are under
`/tmp/pr102-rd1/`. These fixes have not been deployed to the P5.

## Review round 2 — 2026-09-13

The finding against `b3a7ac0` exposed a mismatch between SSH's accepted
geometry and the kernel PTY bounds, plus an ignored resize result.

- PTY allocation requests and window changes refuse nonzero columns outside
  2–512 or rows outside 2–256. Zero retains its unspecified-component meaning.
- Window changes expose a proposal to the adapter. `ssh_resize_result()`
  commits it and sends a requested success reply only after the adapter
  succeeds; failure retains the last accepted geometry and sends failure
  when requested. A proposal before shell startup supplies its initial size.
  Both the os64 daemon and POSIX test adapter report their resize result.
- The bounds regression failed 36 checks on the old code, and the extracted
  daemon adapter regression separately failed to report syscall failure.
  Tests cover both size limits, zero components, reply suppression, failed
  resize preserving state, and completion being consumed once. The host
  runner includes the adapter regression with its prior loop tests.

Host ASan/UBSan passes **798 engine checks**, four daemon regression groups,
and the full OpenSSH interoperability suite. Private 8-core QEMU passes
**798 guest SSH checks**, the full 3 MiB/rekey/PTY probe, and **65 kernel
tests**. The extended live probe verifies that 600x300 and 1x1 are refused
without changing the PTY, 512x256 is applied, and 0x0 preserves that size.
Strict build, diff/stale-reference checks and read-only root/home ext2
checks pass. Evidence and before/after logs are under `/tmp/pr102-rd2/`.
These fixes have not been deployed to the P5.

## Review round 3 — 2026-09-14

Three findings against `ce55515`, all in the transport:

- IGNORE, DEBUG and UNIMPLEMENTED are handled ahead of the key-exchange
  gate. Strict KEX refuses them during the initial exchange only, which is
  OpenSSH's `KEX_INITIAL` rule; a rekey, and a peer that never negotiated
  strict KEX, accept them at any time, as RFC 4253 section 11 requires. The
  comment claiming the peer must send the exchange message next was false
  and is gone.
- Every deferred reply's framed wire cost is tracked, and receive takes no
  packet until the output queue has room for one maximum packet plus that
  replay, so NEWKEYS can never fail to release what the budget accepted. A
  static assertion holds the budget's worst case (one-byte replies at 48
  wire bytes each) plus one packet inside the queue.
- A 255-byte identification line, 253 characters plus CRLF, is accepted.

The engine fixture grew a transport-message matrix (non-strict initial
exchange, strict initial exchange, strict client sending IGNORE before
KEXINIT, strict and non-strict rekeys, malformed messages), the
identification bounds in both directions, and a 910-reply deferred replay
that is refused at the old output threshold and released at the reserved
one. Against the old transport those checks fail 31 times.

Host ASan/UBSan passes **1776 engine checks**, the four daemon regression
groups, and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **1776 guest SSH checks** and the full 3 MiB/rekey/PTY probe; the
kernel suite passes 30 pre-boot and 32 post-boot tests, and the boot's log
carries no fault. Strict build, diff/stale-reference checks and read-only
ext2 checks of the copied root and home partitions pass. These fixes have
not been deployed to the P5.

## Review round 4 — 2026-09-14

One finding against `4b087aa`: within one service pass both streams could
send, and the second send handed priority straight back to the first, so a
replenishment just over the 4096-byte staging buffer split 4096:1 the same
way on every update. The daemon now rotates once per pass, away from the
first stream that sent bytes; the host adapter mirrors the rule. The loop
harness gained an 8-replenishment, 4097-byte credit case that the old daemon
fails at a 32768:8 split and the new one passes at 16388:16388.

Host ASan/UBSan passes **1776 engine checks**, the four daemon regression
groups and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **1776 guest SSH checks**, the full 3 MiB/rekey/PTY probe and the
kernel suite (30 + 32 + 3), with no fault in the boot's log and clean
read-only ext2 checks of the copied root and home partitions. Strict build
and diff checks pass. Not deployed to the P5.

## Review round 5 — 2026-09-14

Three findings against `e8f659f`, none in the transport:

- An unknown message before authentication is answered with UNIMPLEMENTED
  naming its sequence number (RFC 4253 section 11.4); a connection-protocol
  message there is still a state violation and disconnects. The fixture
  checks five unknown types and one channel open before authentication; the
  old connection layer fails ten of those checks.
- `etc/sshd.conf` is written to the ext2 root's `/etc` by the image build,
  as a dependency and a copy. The FAT lifeboat deliberately carries none.
- `tools/sshd_probe.py --skip-fixture` now skips every fixture-dependent
  stage, so a production-only image can be probed.

Host ASan/UBSan passes **1793 engine checks**, the four daemon regression
groups and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **1793 guest SSH checks** and the full 3 MiB/rekey/PTY probe, and a
second boot passes the probe with `--skip-fixture`; the kernel suite passes
its pre-boot and post-boot phases, the boot's log carries no fault, the
root image carries `/etc/sshd.conf`, and read-only ext2 checks of the copied
root and home partitions pass. Strict build and diff checks pass. Not
deployed to the P5.

## Review round 6 — 2026-09-14

One finding against `c1d4d57`, the sibling of round 5's: the key-exchange
gate still disconnected on an unknown type. An unknown message during a
non-strict exchange or any rekey now earns UNIMPLEMENTED, as OpenSSH's
`kex_protocol_error` answers; strict KEX still ends the initial exchange on
any unexpected packet, and a known message out of order still disconnects.
The helper is the transport's, shared with the connection layer. The
fixture checks four unknown types in each of those states and seven known
types out of order; the old engine fails 24 of those checks.

Host ASan/UBSan passes **1895 engine checks**, the four daemon regression
groups and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **1895 guest SSH checks**, the full 3 MiB/rekey/PTY probe and a
second pass with `--skip-fixture`; the kernel suite passes 30 + 32 + 3, the
boot's log carries no fault, and read-only ext2 checks of the copied root
and home partitions pass. Strict build and diff checks pass. Not deployed
to the P5.

## Review round 7 — 2026-09-14

One finding against `1483128`: the gate's "known" predicate read `>= 30`,
which swept the userauth method range, the client-reserved range and the
locally assigned range (192–255) into the disconnect. It now names the
ranges this server speaks — 30–50 and 80–100 beside service, ext-info and
NEWKEYS — matching the pre-auth rule, so a vendor-extension type during an
exchange reaches UNIMPLEMENTED. The fixture's unknown set grew to 51, 79,
101, 127, 192 and 255; the round-6 engine fails 36 of those checks.

Host ASan/UBSan passes **2003 engine checks**, the four daemon regression
groups and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **2003 guest SSH checks**, the full 3 MiB/rekey/PTY probe and a
second pass with `--skip-fixture`; the kernel suite passes 30 + 32 + 3, the
boot's log carries no fault, and read-only ext2 checks of the copied root
and home partitions pass. Strict build and diff checks pass. Not deployed
to the P5.

## Review round 8 — 2026-09-14

One finding against `b89807e`: the accepted `pty-req`'s terminal type never
reached the shell. `ssh_term_env` answers it as a TERM value only when it
is a plain printable name without `=`; the daemon sets TERM before the
seated spawn, so the shell and everything it runs inherit it, and the host
adapter does the same. No os64 program reads TERM yet; the value came off
the wire with the PTY and the cost is one validated setenv. The fixture
checks the rule both ways, and the live probe reads `$TERM` back through
the guest PTY.

Host ASan/UBSan passes **2023 engine checks**, the four daemon regression
groups and the full OpenSSH interoperability suite. Private 8-core QEMU
passes **2023 guest SSH checks**, the full 3 MiB/rekey/PTY probe with the
TERM read-back and a second pass with `--skip-fixture`; the kernel suite
passes 30 + 32 + 3, the boot's log carries no fault, and read-only ext2
checks of the copied root and home partitions pass. Strict build and diff
checks pass. Not deployed to the P5.

## Review round 9 — 2026-09-14

Two findings against `10baf57`:

- The "known message" predicate, on its fourth round, still held the
  unassigned 83–89 gap. It is gone: before authentication and during an
  exchange alike, any message the state does not handle is answered with
  UNIMPLEMENTED, known type or not, as OpenSSH answers; strict initial KEX
  stays the one disconnect. A rule that names no numbers has no range to
  be off by. The fixture sends one value from every RFC 4250 range and the
  gap in each state; the round-8 engine fails 82 of those checks.
- A `sshd.conf` the ladder names but cannot open was accepted as absent,
  so the daemon listened on 22 with the operator's file unread. The port
  logic is `configured_port`, which resolves and then reads; only genuine
  absence permits the default, and an unreadable file refuses startup. The
  daemon loop harness gained a `port` group scripting the ladder: absent,
  unreadable, truncated, a valid port, an empty file, and invalid values.
  A guest cannot produce "named but unopenable" on ext2 without a
  permissions model, so that proof is the harness's.

Host ASan/UBSan passes **2204 engine checks**, five daemon regression groups
and the full OpenSSH interoperability suite. Private 8-core QEMU passes
**2204 guest SSH checks** and the full 3 MiB/rekey/PTY probe; the kernel
suite passes 30 + 32 + 3, the boot's log carries no fault, and read-only
ext2 checks of the copied root and home partitions pass. Strict build and
diff checks pass. Not deployed to the P5.

## References

- [RFC 4251](https://www.rfc-editor.org/rfc/rfc4251.html): SSH types and mpints.
- [RFC 4252](https://www.rfc-editor.org/rfc/rfc4252.html): public-key authentication.
- [RFC 4253](https://www.rfc-editor.org/rfc/rfc4253.html): transport and key derivation.
- [RFC 4254](https://www.rfc-editor.org/rfc/rfc4254.html): channels and exit status.
- [RFC 5656](https://www.rfc-editor.org/rfc/rfc5656.html): ECDSA encoding.
- [RFC 8731](https://www.rfc-editor.org/rfc/rfc8731.html): SSH X25519 encoding.
- [OpenSSH 9.6 PROTOCOL](https://github.com/openssh/openssh-portable/blob/V_9_6_P1/PROTOCOL): strict KEX.
