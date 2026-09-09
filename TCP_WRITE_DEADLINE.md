# TCP write waits with a deadline

`os64_write_for(handle, data, length, timeout_ms)` gives a TCP caller a bounded
wait for send-ring space. It returns a queued prefix as soon as one is
available. The caller retains the suffix and owns any overall operation
budget across calls. Accepted bytes remain TCP's responsibility; a timeout
does not reset, detach or close the connection.

This is a separate syscall, 55, preserving syscall 3 and the three-argument
`os64_write` ABI. The existing write keeps its fill-the-request behavior.
The new call accepts TCP handles only; other handle types return -1, including
for zero-length requests. A valid TCP handle accepts an empty request as a
no-op, with a NULL buffer allowed. No stream EOF is implied by a write result.

Timeout values follow `os64_read_for`: 0 polls, a finite number waits up to
that many milliseconds rounded up to a scheduler tick, and
`OS64_WAIT_FOREVER` permits an indefinite wait for initial progress. Ready
space takes precedence over an expired clock, as ready data does for reads.
A positive result means queued, not delivered or acknowledged. No space by
the deadline returns `OS64_ERR_TIMEOUT`; an interrupted wait reports the
existing signal result and a reset reports the existing write error. A
successful prefix returns immediately, without waiting to fill its suffix.

The syscall samples the monotonic tick counter once before copying user
bytes. Conversion avoids addition overflow and saturates large finite
absolute deadlines instead of wrapping into an immediate or infinite wait.
A separate bounded flag distinguishes a deadline at tick zero from forever.
The copy is limited to 4096 bytes per call; larger requests return a prefix.
The deadline limits waiting for TCP room, not scheduler latency, memory
fault handling or the CPU work of copying/submitting that bounded prefix.

The TCP implementation shares queueing, interruption, ring-wrap and output
logic with the existing write. Waiter registration and the room test hold
the connection lock. Wakeup uses the earlier of the existing one-second
backstop and the caller deadline. An awakened writer clears its registration
before returning. Packet-arrival and tick-sweep wake conditions are unchanged;
retransmission, persist and detached lifetime remain TCP protocol concerns.
Callers serialize writes/close on an owned handle, as with the existing API.

Validation covers current TCP bodies under ASan/UBSan: full-ring poll,
finite expiration, tick-zero deadline, wrapped ring prefixes, wake-before-
deadline progress, interruption/reset cleanup and unchanged blocking writes.
A syscall fixture checks dispatch ABI, bounded copies, timeout conversion,
unsupported handles, empty input and result mapping. A guest network probe
uses the new shared-library call with a host peer that stops reading and then
resumes; the peer verifies exact stream bytes across a timeout and retry.

This supplies a wait primitive for the future TLS driver. That driver must
alternate directions, retain unsent ciphertext, and enforce one overall
monotonic handshake/shutdown budget. No TLS transport or HTTP integration is
part of this prerequisite.

## Running the checks

Host tests:

```sh
ASAN_OPTIONS=detect_leaks=0 tools/test_tcp_host.sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_tcp_write_syscall_host.py
```

For a guest or production-machine check, first start the peer on a reachable
host (substitute its LAN address):

```sh
python3 tools/test_tcp_write_peer.py --bind HOST_IP
```

Then, in os64 with the updated kernel and userland installed:

```sh
/tests/tcpwriteprobe HOST_IP 17260
```

The peer handles one probe run; restart it for another. Both sides must print
PASS. The probe checks a full-ring poll, a 100 ms finite timeout and a retry
on the same connection. The peer checks byte content, total length and EOF.
For QEMU user networking, use the peer's default loopback bind and pass
`10.0.2.2` to the guest probe. This manual network fixture is separate from
`testrun` because it needs an external peer.
