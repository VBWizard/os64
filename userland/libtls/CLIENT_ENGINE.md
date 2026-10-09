# Private TLS client engine

The private engines sit behind the
[public byte library](../../docs/design/completed/TLS_PUBLIC_LIBRARY.md).
`client_engine.c` owns a union of the TLS 1.3 state machine and BearSSL's
TLS 1.2 client, with shared byte-prefix, EOF, shutdown and terminal handling.
The [certificate-policy factory](../../docs/design/completed/TLS_CERTIFICATE_POLICY.md)
supplies the acceptance gate for both engines. The
[production-input constructor](../../docs/design/completed/TLS_PRODUCTION_INPUTS.md)
supplies fresh randomness and UTC time. The transport API and libfetch drive
these byte engines for native HTTPS.

## Profile and ownership

The default protocol selects `port/tls13_handshake.c`: TLS 1.3 with AES-128-GCM,
AES-256-GCM or ChaCha20-Poly1305, X25519/P-256 initial shares and P-384 retry.
It authenticates the existing policy-approved certificate with ECDSA or
RSA-PSS CertificateVerify and verifies Finished before exposing plaintext.
Certificate and NewSessionTicket framing stream across record boundaries;
other handshake messages have an 8 KiB bound. Retry and epoch transitions
must end their record. The [TLS13 design](../../docs/design/completed/TLS13.md)
describes the profile and its explicit exclusions.

`OS64_TLS_PROTOCOL_TLS12` selects `port/client_profile.c`: six ECDHE AEAD suites,
SHA-256/384/512 signatures, and scalar EC/RSA/AES/GHASH/ChaCha20/Poly1305.
Renegotiation is disabled. An unoffered ALPN selection fails; an absent ALPN
extension is allowed. The profile does not inherit the full upstream client
initializer. The minimal-X.509 initializer is a private helper; its minimum
RSA byte length does not establish the bit-length or anchor policy in TLS.md.

`OS64_TLS_PROTOCOL_TLS12_FALLBACK` selects the same BearSSL profile for a
retry after `PEER_CHOSE_TLS12`. Before reporting handshake completion or
exposing either application direction, the wrapper rejects `DOWNGRD01` and
`DOWNGRD00` in the authenticated server random as `TLS13_ERR_DOWNGRADE`, a
sticky protocol failure. Explicit TLS12 selection does not apply this guard.

`port/client_engine.c` owns the protocol context, bidirectional record buffer,
normalized DNS hostname, copied ALPN strings, and an owned validator. Hostnames
are ASCII DNS names of at most 253 bytes with labels of at most 63 bytes. IP
literals are unsupported. ALPN allows eight nonempty, NUL-free names of at
most 255 bytes each and 1024 bytes combined.

The private validator factory receives the copied hostname and explicit UTC
validation date, and returns a fresh owned validator. It must retain its
immutable trust snapshot and any other later dependencies. Factory context is
borrowed during construction; it is not a connection dependency. A non-NULL
partial result is destroyed even if the factory reports failure. The public
wrapper supplies the policy-enforcing factory in `certificate_policy.h`;
applications cannot inject a trust bypass.

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
After TLS 1.3 server authentication, ciphertext input remains available while
output is pending, unless unread plaintext occupies the receive buffer or the
peer has closed. This lets an upload receive an early response even when socket
writes are blocked. Initial ClientHello flights drain before processing their
responses, whose handlers reuse the send buffer for the next flight.
Before the initial handshake completes, accepted ciphertext is capped at
1 MiB across calls, including warning records. A transfer crossing the cap
accepts at most the remaining prefix; further input fails with `TLS_LIMIT`
without acknowledging excess bytes. Post-handshake traffic has no such cap.

State includes the bounded `policy_reason` supplied by the owned validator's
optional diagnostic callback. The certificate-policy factory provides it;
fixture factories without a callback report `TLS_POLICY_OK`. It remains
separate from the terminal status and upstream error and exposes no peer text.
Engine record/message size errors and a `TLS_POLICY_LIMIT` callback reason
classify as `TLS_LIMIT`, including an incomplete or invalid certificate after
a policy budget refusal.
The upstream error remains available in state.
`os64_tls_error_description` translates a public state snapshot into static
display text, including certificate-policy reasons and named peer alerts.

Close stops new plaintext writes, flushes accepted output, and waits for the
caller to drain already buffered authenticated input before starting engine
closure. After closure starts, subsequently arriving application records are
discarded by both engines. Closing during the initial handshake cancels it.
Transport EOF forbids further ciphertext input and plaintext writes; buffered
authenticated plaintext and pending output can drain. Accepted plaintext is
flushed into ciphertext before truncation is reported, including when the
caller has not explicitly flushed it. A peer close reply can
finish cleanly after EOF; a bare FIN or incomplete record yields truncation.
Terminal errors are sticky, including across a later abort. Fatal-alert output
may be drained when upstream supplies it; abort disables all I/O.

## Validation

The TLS 1.2 host harness uses fixture-only deterministic entropy and pinned-key
validators against imported BearSSL sample servers. The TLS 1.3 harness drives
OpenSSL memory BIOs through the public byte API and real certificate policy.
Both use explicit fixture time and fresh deterministic entropy per connection.

Run from the repository root:

```sh
python3 tools/test_tls_profile_host.py
python3 tools/test_tls_engine_host.py
python3 tools/test_tls13_engine_host.py
python3 tools/fuzz_tls13_host.py --seconds 1800 --output /tmp/tls13-fuzz
```

Each command can first run the foundation harness. They accept
`--foundation /path/to/adapted/core.a` to reuse a successful foundation build
from the same pin/configuration. ASan/UBSan and leak detection remain enabled.
If LeakSanitizer cannot attach, diagnose with
`LSAN_OPTIONS=verbosity=1:log_threads=1` and inspect `TracerPid`: its generic
ptrace hint does not establish that a tracer exists. A sandbox attachment
refusal calls for a leak-enabled run outside that sandbox. Disable leak checks
only when a supported run is unavailable and record the missing evidence.
The engine fixture additionally counts owned
allocations and checks that their bytes are zero before freeing them.

The TLS 1.2 fixtures cover:

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
cleanup through the [OS-input constructor](../../docs/design/completed/TLS_PRODUCTION_INPUTS.md).
The certificate-policy harness supplies additional negative and handshake-gate
coverage. Guest network acceptance is tracked in TLS13 S3. The public library
audit checks its selected production source list against the linked objects
and excludes test-only parser bypasses.

The TLS 1.3 fixture covers the three suites, RSA/ECDSA leaves, X25519/P-256
and P-384 HRR against OpenSSL; transfer fragmentation, tickets, KeyUpdate and
both close orders; real TLS 1.2 selection and fresh Bear fallback; and
valid-tag malformed messages through the public byte API. It also checks early
responses with untaken or partially taken upload output, incoming KeyUpdate
and application data while a reply is pending, and simultaneous close alerts.
These cases retain the queued output and verify its later delivery. A scripted
legacy response followed by a real TLS 1.3-capable OpenSSL peer checks fallback
downgrade refusal before plaintext access, with explicit TLS12 as a control.
Mutated captures
exercise both raw records and authenticated handshake parsing. The fuzz runner
retains its fixture keys, corpus, executable, log and next-case seed in the
output directory. `--replay DIRECTORY` reproduces that case with the retained
binary. Replay artifacts contain fixture-only secrets and are trusted local
host files, not a format for untrusted inputs.
