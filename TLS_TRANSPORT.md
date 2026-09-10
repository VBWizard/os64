# TLS transport adapter

`<tls/transport.h>` adds an owned TCP adapter in `libtls.so`, alongside the
byte API in `<tls/tls.h>`. Creation takes a connected TCP handle, TLS config
and optional finite handshake/shutdown limits (defaults: 30 s and 2 s).
Success transfers handle ownership; failure leaves it with the caller.
The adapter creates and owns its TLS client. The caller serializes its
operations and stops using the adopted handle directly. Supplying a connected
TCP handle is a caller precondition; creation does not query its type.
An empty write is a no-op and cannot serve as a TCP type check.

DNS and TCP dialing precede adoption and are outside these TLS deadlines.
Creation samples the monotonic clock before creating the client; entropy and
UTC still come from the production constructor. The handshake deadline is
retained across calls. Shutdown records its deadline on the first
`begin_close`, and repeated calls cannot restart it. A clock failure, rate
change or backward tick count fails closed. Arithmetic saturates rather than
wrapping; zero or infinite configured lifetime limits are rejected.
An automatically generated reply to peer closure receives the shutdown
budget too. An expired active deadline is checked before that transition.

`step(timeout_ms)` performs bounded transport work, trying both ready
network directions before waiting. Zero polls. It waits on one needed
direction using the supplied finite patience, capped by the remaining protocol
budget and rounded up by the kernel to scheduler ticks, then checks progress
again. When both directions need service, it caps the wait at 10 ms and
alternates them so a full send ring cannot starve input. A single-direction
wait uses the caller's patience without periodic adapter wakeups. Empty waits
return NEED_PROGRESS and leave the connection usable.
A caller must not busy-loop on NEED_PROGRESS: drain available plaintext,
supply/flush output, or call step with a finite wait as appropriate.

Each direction has a 4096-byte ciphertext buffer with separate consumed and
remaining offsets. Taking engine output transfers ownership to that buffer;
a short TCP write advances its offset, and timeout preserves its suffix.
Pending input is fed by accepted-prefix count before another read can replace
it. Step never discards authenticated plaintext to make protocol progress.
Read/write remain nonblocking plaintext prefix operations; flush requests
records, and step drives them. A positive write means accepted into TLS,
not delivered. Application calls inspect their transfer count even when a
terminal status accompanies it.
The adapter reports HANDSHAKE_DONE and accepts plaintext output after final
handshake ciphertext has reached TCP, so a blocked final flight remains
subject to the handshake deadline.

Handshake/shutdown expiration is terminal TIMEOUT. Caught OS I/O interruption
returns NEED_PROGRESS promptly, preserving the connection and pending bytes;
that step does not start another I/O attempt after the interruption. The caller
decides whether to resume or explicitly abort with CANCELLED. Protocol deadline
expiry still wins if the budget has expired. Network errors are TRANSPORT. Raw TCP EOF goes through
the engine's truncation rules. Received close_notify is clean TLS EOF, but a
pending close reply must be submitted before the adapter reports completed
closure. An error while submitting that reply is a transport failure.
Fatal TLS errors preserve their upstream/policy diagnostics; alert output is
not promised after failure. Abort and free do not wait for a close exchange.
Terminal cleanup closes the owned handle once. Failure clears pending buffers
and aborts the byte engine. Free wipes the adapter and destroys/wipes the client.
TCP may finish transmitting already accepted ciphertext after handle close.

State/read/write/flush/step/close check active lifetime deadlines. Completion
of handshake retires its deadline; beginning shutdown installs the separate
shutdown deadline. Enforcement occurs when the owner enters the API; no
background task closes an idle adapter. The limits bound protocol waiting and repeated progress,
not scheduler latency, individual cryptographic operations or TCP connection
establishment. Application operation budgets belong to the caller and are
not reset by positive step results.

Validation uses deterministic host seams around the actual adapter for short
I/O, simultaneous directions, retained suffixes, deadline trickles, clock
failures, cancellation, EOF and ownership. A real engine/peer fixture checks
the pump across authenticated traffic. A guest probe exercises the installed
library and TCP syscalls with a controlled OpenSSL peer. Public root
distribution and HTTP integration remain separate slices.

## Checks and guest use

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_tls_transport_host.py
python3 tools/audit_tls.py
```

The network probe embeds the existing public fixture root, used solely by
this test program. Its peer uses Python's standard-library `ssl` module and
includes the public fixture key as PEM; no third-party Python packages are
required.
Start the peer on a reachable host, substituting its LAN address:

```sh
python3 tools/test_tls_transport_peer.py --bind HOST_IP
```

For a Windows host with this worktree in WSL2, run Windows Python from
PowerShell with the script's UNC path; changing to the WSL directory or
activating a virtual environment is unnecessary:

```powershell
py '\\wsl$\Ubuntu-big2\home\yogi\src\os64\.worktrees\tls-transport\tools\test_tls_transport_peer.py' --bind HOST_IP
```

Use the Windows host's LAN address for `HOST_IP`, including in the os64
commands below.

In os64, with the TCP deadline kernel and updated userland installed:

```sh
/tests/tlstransportprobe HOST_IP 17270 good
/tests/tlstransportprobe HOST_IP 17270 stall
/tests/tlstransportprobe HOST_IP 17270 truncated
/tests/tlstransportprobe HOST_IP 17270 badname
```

Both sides must report PASS for each case. The successful exchange verifies
65,536 patterned plaintext bytes in each direction followed by clean TLS
closure. The negative cases require TIMEOUT for a silent handshake,
TRUNCATED for a bare TCP FIN and CERTIFICATE for the wrong hostname.
For QEMU user networking, omit `--bind` and use `10.0.2.2` in the guest.
Restart the peer for each four-case run, or select one with `--cases good`
(or another case name). These are manual network probes, separate from
`testrun`; public fixture keys establish no public-web trust.
