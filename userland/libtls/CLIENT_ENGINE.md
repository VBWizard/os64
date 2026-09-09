# Private TLS client engine

This slice depends on the BearSSL foundation in PR #74. Carry foundation
review fixes forward before publishing the dependent slice.

The engine source here is private behind the
[public byte library](../../TLS_PUBLIC_LIBRARY.md). It adds no kernel/libos64
interface or transport integration. The
[certificate-policy factory](../../TLS_CERTIFICATE_POLICY.md) supplies DER trust
snapshots and the acceptance gate. The [trust-store loader](../../TLS_TRUST_STORE.md)
supplies complete PEM snapshots. The [OS-input constructor](../../TLS_PRODUCTION_INPUTS.md)
supplies production randomness and UTC time. Transport integration remains
required for native HTTPS.

## Profile and ownership

`port/client_profile.c` explicitly selects TLS 1.2, six ECDHE AEAD suites,
SHA-256/384/512 signatures, and scalar EC/RSA/AES/GHASH/ChaCha20/Poly1305.
Renegotiation is disabled. An unoffered ALPN selection fails; an absent ALPN
extension is allowed. The profile does not inherit the full upstream client
initializer. The minimal-X.509 initializer is a private helper; its minimum
RSA byte length does not establish the bit-length or anchor policy in TLS.md.

`port/client_engine.c` owns the protocol context, bidirectional record buffer,
normalized DNS hostname, copied ALPN strings, and an owned validator. Hostnames
are ASCII DNS names of at most 253 bytes with labels of at most 63 bytes. IP
literals are unsupported. ALPN allows eight nonempty, NUL-free names of at
most 255 bytes each and 1024 bytes combined.

The private validator factory receives the copied hostname and explicit UTC
validation date, and returns a fresh owned validator. It must retain its
immutable trust snapshot and any other later dependencies. Factory context is
borrowed during construction; it is not a connection dependency. A non-NULL
partial result is destroyed even if the factory reports failure. The future
public wrapper must supply the policy-enforcing factory in `certificate_policy.h`;
an application-provided trust bypass is not part of that public design.

The synchronous entropy callback must fill the requested 32 bytes completely
before returning success. Its context is not retained. Failure prevents a
connection from being returned; cancellation and timeout remain distinguishable.
The production `/dev/random` adapter must map an unseeded `-1` refusal to
`TLS_ENTROPY_UNAVAILABLE`; construction must not wait for the pool to seed.
The seed is wiped on success and failure. Destroy releases the owned validator
and wipes the connection allocation. Each connection has one serialized owner;
different connections share no mutable engine state. Abort stops I/O; callers
still destroy the connection to release and wipe its storage.

## Progress and shutdown

The engine owns no transport handle and performs no DNS, filesystem, clock,
sleep, or scheduler operations. Callers drive `feed`/`take` for ciphertext and
`write`/`read` for plaintext. Each call copies one accepted prefix and returns
both its count and status. Processing the final accepted bytes can return a
terminal status together with a nonzero count. Zero-length calls do not
acknowledge upstream buffers; invalid arguments do not poison a live engine.
State flags tell the caller which operations can make progress.
Before the initial handshake completes, accepted ciphertext is capped at
1 MiB across calls, including warning records. A transfer crossing the cap
accepts at most the remaining prefix; further input fails with `TLS_LIMIT`
without acknowledging excess bytes. Post-handshake traffic has no such cap.

State includes the bounded `policy_reason` supplied by the owned validator's
optional diagnostic callback. The certificate-policy factory provides it;
fixture factories without a callback report `TLS_POLICY_OK`. It remains
separate from the terminal status and upstream error and exposes no peer text.
When that callback reports `TLS_POLICY_LIMIT`, terminal error classification
returns `TLS_LIMIT` even if BearSSL reports an incomplete or invalid certificate.
The upstream error remains available in state.

Close stops new plaintext writes, flushes accepted output, and waits for the
caller to drain already buffered authenticated input before invoking upstream
closure. After closure starts, subsequently arriving application records use
BearSSL's discard semantics. Closing during the initial handshake cancels it.
Transport EOF forbids further ciphertext input and plaintext writes; buffered
authenticated plaintext and pending output can drain. Accepted plaintext is
flushed into ciphertext before truncation is reported, including when the
caller has not explicitly flushed it. A peer close reply can
finish cleanly after EOF; a bare FIN or incomplete record yields truncation.
Terminal errors are sticky, including across a later abort. Fatal-alert output
may be drained when upstream supplies it; abort disables all I/O.

## Validation

The host harness uses fixture-only deterministic entropy and pinned-key
validators against the imported BearSSL sample servers. These fixtures prove
protocol exchanges and wrapper behavior, not production certificate policy.

Run from the repository root:

```sh
python3 tools/test_tls_profile_host.py
python3 tools/test_tls_engine_host.py
```

Each command first runs the foundation harness. Both accept
`--foundation /path/to/adapted/core.a` to reuse a successful foundation build
from the same pin/configuration. ASan/UBSan remain enabled. If LeakSanitizer
refuses to run under tracing, use `ASAN_OPTIONS=detect_leaks=0`; supported
hosts should retain leak detection. The engine fixture additionally counts owned
allocations and checks that their bytes are zero before freeing them.

The fixtures cover:

- Actual ClientHello version, suites, SHA-2 signature pairs, curves, SNI,
  ALPN, null compression, and empty session ID checks; fragmented TLS 1.0/1.1,
  unoffered RSA/CBC suites, and unoffered ALPN rejected.
- Complete TLS 1.2 handshakes for all six suites; 70,013 bytes transferred in
  each direction per suite with ciphertext fragments from one to 4096 bytes.
  Small plaintext reads, buffered-input closure, and flushing accepted output
  during close are checked.
- Configuration copy lifetime, hostname/ALPN limits, negative epochs, midnight,
  leap-day conversion, allocation/factory/entropy failures, and wiped cleanup
  checks. Two interleaved connections must remain independent when one aborts.
- Warning-record traffic at the 1 MiB initial-handshake input boundary and
  more than 1 MiB of post-handshake traffic.
- Certificate refusal, damaged authenticated records, bare EOF with buffered
  input and unflushed output, partial-record EOF, EOF with a pending close
  reply, cancellation,
  and sticky transport/timeout errors.

Build the port sources through the existing private freestanding PIC build
rule and check a relocatable link against the foundation archive. Its external
functions should be `os64_malloc`, `os64_free`, `os64_memcpy`, `os64_memmove`,
`os64_memset`, and `os64_strlen`.

The guest `tlsinputtest` exercises engine creation, ClientHello output and
cleanup through the [OS-input constructor](../../TLS_PRODUCTION_INPUTS.md).
The host harness uses the adapted pinned server implementation; the
certificate-policy harness supplies separate negative and handshake-gate
coverage. Independent-peer interoperability, guest network handshakes,
and fuzzing remain validation gates in TLS.md. The public library audit
checks its selected production source list against the linked objects.
