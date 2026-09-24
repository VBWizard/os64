# TLS production inputs

This private adapter connects the byte engine to os64's random-byte service
and UTC clock. It installs no public ABI and owns no network transport.

`os64_tls_engine_create_os` accepts hostname/ALPN and a borrowed sealed trust
snapshot. It supplies the certificate-policy factory, reads UTC with
`os64_time`, and creates a fresh engine. The engine retains the snapshot;
the caller may release its reference after creation. Failure leaves the
output NULL. The caller serializes access to the configuration and snapshot.
The adapter provides no entropy, time, or validator override.

Creation opens `/dev/random` in read mode and fills the engine's 32-byte seed.
Positive short reads advance the destination; zero, negative, and oversized
returns refuse creation with `TLS_ENTROPY_UNAVAILABLE`. An opened handle is
closed on success or failure; a close error also refuses creation. There is
no retry after a read refusal, no waiting for seeding, and no fallback source.
The existing engine injects a complete successful seed and wipes its scratch
on success and failure. The adapter creates no additional seed copy.

The device contract supplies bounded reads and refuses unseeded requests.
The generic read ABI has no separate interruption reason, so a negative read
maps to entropy unavailability. Cancellation/deadlines during network progress
belong to the transport slice. The locally administered `/dev/random` path is
trusted under the OS execution model; the adapter does not authenticate a
replacement filesystem node or assess the kernel generator's entropy quality.

Clock errors return `TLS_BAD_TIME`; the engine checks its supported epoch
range. The raw UTC epoch is passed unchanged, without applying timezone or
subsecond fields. This does not establish that the machine clock is accurate
or synchronized. The OS clock is an administrator-controlled input.

## Validation

`tools/test_tls_inputs_host.py` compiles the production adapter and engine with
injected os64 I/O and clock functions under ASan/UBSan. It checks input errors,
short reads, failure at seed offsets, close errors, allocation cleanup, clock
boundaries, snapshot lifetime and TLS policy behavior with controlled peers.
Deterministic seeds and clock overrides exist in the host fixture only.

Run it with `python3 tools/test_tls_inputs_host.py`. A matching foundation
build can be reused with `--foundation /path/to/adapted/core.a`; `--output`
retains artifacts in a new directory. The foundation README documents the
LeakSanitizer exception for hosts running under tracing.

In os64, `testrun tlsinputtest` creates independent clients using the real
random device and clock, with an in-memory fixture trust snapshot. It checks
ClientHello production, distinct handshakes, snapshot ownership and cleanup.
It makes no network connections, edits no trust configuration and prints no
seed bytes. An unavailable random service or unusable clock fails the fixture.
This tests service integration; it does not certify randomness quality or
prove a successful authenticated network connection.
`/tests/tlsinputtest --license` prints the embedded upstream notice, using the
foundation fixture's existing notice helper.

The platform object remains outside the pure BearSSL/client archive. The ELF
audit (`python3 tools/audit_bearssl.py`) checks its OS dependencies separately
and links the combined private objects against libos64. Inspect the optimized
engine's seed wipe on success and refusal; the adapter does not own a scratch
seed. Use the isolated-disk procedure in [VERIFICATION.md](VERIFICATION.md)
for guest tests, with a freshly built kernel and userland. Public library
wiring is described in [TLS_PUBLIC_LIBRARY.md](TLS_PUBLIC_LIBRARY.md).
Transport deadlines, independent network interoperability, public roots and
HTTPS integration remain separate.
