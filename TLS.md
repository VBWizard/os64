# TLS: the BearSSL port and its proof boundary

Design and implementation plan, 2026-09-07. This document specifies the library slice of the
[browser arc](BROWSER.md), its tests, and the prerequisites for native HTTPS.
The foundation import and test fixture live in [userland/libtls](userland/libtls/README.md).
The dependent [private client engine](userland/libtls/CLIENT_ENGINE.md) implements
connection ownership and byte transfers, with fixture-key handshake tests.
The [certificate-policy slice](TLS_CERTIFICATE_POLICY.md) adds bounded DER
inspection and sealed trust snapshots behind the private validator factory.
The [trust-store loader](TLS_TRUST_STORE.md) selects and validates complete
PEM bundles, with a guest `tlstrusttest` fixture for parsing and replacement.
The [production-inputs adapter](TLS_PRODUCTION_INPUTS.md) supplies `/dev/random`
and UTC time to the private engine, with a guest `tlsinputtest` creation fixture.
The [public byte library](TLS_PUBLIC_LIBRARY.md) exposes `<tls/tls.h>` and
`libtls.so`; native HTTPS is not ready. Trust-store
selection uses the approved configuration and replacement policy below;
public root-bundle selection and network integration remain open.

## Intended result and scope

Build `/lib/libtls.so`: an os64-owned client interface around a pinned BearSSL
implementation. Applications supply transport bytes and receive authenticated
plaintext. Cryptography and the TLS protocol stay in userland. The public
header is `<tls/tls.h>`, with `os64_tls_*` names; the shared-object name does
not imply compatibility with OpenBSD's libtls API.

The first independently useful deliverable is a freestanding port with
upstream vectors, differential host tests, and a guest vector runner. Native
HTTPS then needs a tested certificate policy, a production entropy provider,
and a transport adapter. HTTP integration and libfetch extraction are separate
changes, consistent with BROWSER.md's division of work. JPEG is separate too.

Borrow the cryptography and protocol implementation. Keep upstream changes
small, explained, and mechanically diffable. Do not introduce TLS into the
kernel or add a general cryptographic API to libos64 as part of this port.

## Integration interfaces

| Area | Evidence and consequence |
|---|---|
| Shared libraries | `userland/GNUmakefile` builds PIC `libos64.so`, `libgzip.so`, and `libpng.so`, with selective app dependencies, prelink slots, debug symbols, and header dependencies. Follow that machinery. |
| Transport | `os64_dial`, `os64_read_for`, `os64_write_for`, and `os64_close` provide handle I/O for a TCP adapter. Finite read/write timeouts are distinct from EOF; `os64_write` supplies infinite patience. |
| TCP progress | `os64_write_for` bounds waiting for TCP ring space and reports queued progress. The adapter must retain its suffix and enforce its total budget across calls. See [write's patience](abi/include/os64/syscall_numbers.h). |
| Time | `os64_time()` supplies signed UTC epoch seconds. `os64_ticks()` supplies monotonic ticks and their rate. Certificate dates and deadlines use different clocks. |
| Randomness | The production adapter reads `/dev/random`; an unseeded service refuses with `-1`. A CPUID feature bit does not establish service readiness. |
| Configuration | `os64_conf_find()` / `os64_conf_find_from()` resolve basenames along the configured ladder. `kernel/src/conf.c` rejects slashes in the requested name. Reuse the ladder for `tls.conf`, not a nested certificate path. |
| Existing HTTPS | `os64get` uses a terminating proxy. The port does not change proxy routing or publication of downloaded files. |

Relevant interfaces: [raw I/O](userland/libos64/include/os64/io.h),
[wall time](abi/include/os64/time.h), [monotonic time](abi/include/os64/ticks.h),
[configuration](docs/conf_path.md), and [shared-library build](userland/GNUmakefile).

## Upstream baseline and import discipline

The design inspected the [BearSSL 0.6 release archive](https://bearssl.org/bearssl-0.6.tar.gz).
Its downloaded SHA-256 is:

```
6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14
```

This identifies the inspected bytes; it is not an independent authenticity
attestation. The foundation pins official upstream commit
`7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`, including the subsequent decoder,
record-length, and arithmetic fixes. The [import manifest](userland/libtls/README.md)
records hashes, the local patch, configuration, and validation. The
[upstream review](userland/libtls/UPSTREAM_REVIEW.md) records the release delta.

Proposed source layout:

```
userland/libtls/
    include/tls/tls.h       public os64 interface
    upstream/              pinned BearSSL sources, license, and provenance
    port/                  private libc compatibility and build configuration
    tls.c                  engine ownership and byte interface
    trust.c                in-memory trust-store construction
    policy.c               certificate-policy adapter, separately tested
tools/test_tls_host.sh      host vectors, differential tests, sanitizers
tools/test_tls_host.c       deterministic engine and API fixtures
userland/tests/tlstest/     guest vectors and explicit TLS test probes
```

The public wrapper is `userland/libtls/api.c`, with its header under
`include/tls/`. The foundation has `upstream/`, `port/`, a private archive,
and `test/` with `/tests/bearssltest`. The shared build installs `libtls.so`
and `/tests/tlslibtest`. Record source URL, commit,
archive/tree hashes, license, selected files, configuration, and local patches
in the import manifest. Preserve upstream paths. Ship its license with source
and binary distributions. Keep upstream test data with its attribution.
Use the upstream-generated C; record the associated T0 sources and generator
procedure so a generated-file change cannot conceal its real source change.

The foundation archive compiles the core for reference testing and dependency
auditing. Its full list is not a production algorithm allowlist.
The production source manifest `userland/libtls/public_sources.mk` selects
the client, constant-time algorithms, X.509, and required codecs; the ELF
audit checks its members against the linked archive closure. Server tooling, benchmarks, and
unselected algorithms can remain reference-test inputs without being linked
into `libtls.so`. No build-time download or automatic tracking of upstream.

### Freestanding configuration

Use a private compatibility include directory and explicit build defines.
The inspected core requires `memcpy`, `memmove`, `memset`, `memcmp`, and
`strlen`; use existing os64 implementations where their contracts match and
a private adapter where one is missing. Do not export generic libc symbols
or `br_*` symbols from the shared object. Do not replace upstream
constant-time comparisons with ordinary `memcmp`.

Disable hosted entropy/time discovery explicitly:
`BR_USE_GETENTROPY=0`, `BR_USE_URANDOM=0`, `BR_USE_WIN32_RAND=0`, `BR_USE_UNIX_TIME=0`, and
`BR_USE_WIN32_TIME=0`. Disable upstream's implicit `BR_RDRAND` path as well:
entropy policy must be visible to os64. Start with scalar constant-time
implementations (`BR_AES_X86NI=0`, `BR_SSE2=0` for BearSSL's optional intrinsic
paths). The foundation also disables 128-bit backends and unaligned union
loads; see its configuration for the complete set. This does not change
os64's ordinary x86-64 ABI or its compiler's SSE2 baseline. Audit the selected
integer backend and compiler support routines.

The port must link without host libc, OS syscalls hidden behind compatibility
stubs, process exit, thread-local `errno`, constructors, or unsupported ELF
relocations. Inspect undefined symbols, exports, relocations, `DT_NEEDED`,
and the link map. Add libtls to library slot assignment, image installation,
debug symbol generation, and `-MMD -MP` dependencies. Selectively link its
consumers; libos64 does not depend on libtls.

## Library boundary and ownership

```
HTTP client / future libfetch
        | plaintext                       | ordinary TCP handle
        v                                 v
    libtls.so <---- encrypted bytes ---- transport adapter
        |
        v
    libos64.so  (allocation and memory helpers)
```

The engine layer does no socket I/O, DNS, file lookup, sleeping, or clock
sampling. It receives a validated server name, an immutable trust snapshot,
explicit validation time, and entropy through a provider. Host fixtures can
therefore control every external input. The private
[OS-input constructor](TLS_PRODUCTION_INPUTS.md) obtains randomness and UTC
through os64 and binds a caller-supplied sealed trust snapshot to the engine.
The transport adapter drives byte progress separately.

Use opaque connection and trust-store types. Creation allocates the complete
connection working set, copies the server name, and retains a reference to
the immutable trust snapshot. Failure leaves the output pointer NULL and
releases partial allocations. No connection allocation is proportional to a
peer-supplied length. Each connection has one serialized owner; callbacks
cannot reenter it. Independent connections share no mutable protocol state.
Trust-snapshot reference counts must be synchronized for concurrent creation
and destruction of independent connections; a plain shared counter is not
sufficient.

Keep BearSSL's full-size, separate input/output record buffers, sized using
the pinned upstream constants. Do not depend on a server accepting reduced
record sizes. Contexts, certificate scratch, and record buffers belong on the
heap, not a user thread's stack. Measure actual working-set and stack usage
before assigning a connection cap; upstream's allocation-free engine does
not make our wrapper allocation-free.

The public byte interface copies into/out of application buffers. That small
copy cost keeps BearSSL's borrowed-buffer invalidation rules private: its
`ack()` operations can invalidate other previously obtained buffer pointers.
Inside the wrapper, acquire one view, copy, acknowledge the exact positive
count, then discard the view before any further engine operation.

### Public byte operations

| Operation | Contract |
|---|---|
| `trust_create` / `trust_add_der` / `trust_seal` / `trust_free` | Build an owned, bounded CA snapshot. A failed addition does not partly add an anchor. Sealed stores are immutable and remain alive while connections retain them. |
| `client_create` | Validate parameters, sample OS UTC and acquire fresh OS entropy before starting a new handshake. Retain a sealed trust snapshot; no session reuse or input overrides. |
| `state` | Report handshake completion, available byte directions, closure progress, and terminal error separately. Availability can change after an operation. |
| `feed_ciphertext` | Copy and acknowledge an accepted prefix. The caller retains the unaccepted suffix. Does not accept data after transport EOF. |
| `take_ciphertext` | Copy pending encrypted output and transfer responsibility for those bytes to the caller. The transport must retain an unwritten suffix across short writes. |
| `write_plaintext` | Accept a prefix only after the handshake and certificate policy succeed. Accepted means copied into TLS, not sent or acknowledged by the peer. |
| `read_plaintext` | Return an authenticated prefix. Exhaustion while the connection is live is a need-for-progress result, not EOF. |
| `flush` | Request records for buffered plaintext; completing this call does not prove transport delivery. |
| `transport_eof` | Record that no more encrypted input can arrive. A bare TCP FIN does not become successful TLS EOF. |
| `begin_close` | Stop new plaintext writes and initiate orderly TLS closure after accepted plaintext is flushed. Continue driving encrypted I/O. |
| `abort` / `free` | Abort stops I/O with a sticky terminal reason. Free wipes secrets and releases state without blocking. Neither closes a caller-owned TCP handle. |

All names in that table receive the `os64_tls_` prefix. Transfer operations
return both a status and `size_t transferred`; if processing an accepted
prefix reveals an error, its consumed count remains meaningful. Zero-length
calls acknowledge no BearSSL buffers. Zero progress
returns a named result, and callers inspect the state before retrying.

Client status distinguishes OK, NEED_PROGRESS, CLEAN_EOF, BAD_ARGUMENT,
NO_MEMORY, LIMIT, ENTROPY_UNAVAILABLE, BAD_TIME, CERTIFICATE, UNSUPPORTED,
PROTOCOL, TRUNCATED, TRANSPORT, TIMEOUT, and CANCELLED. Trust loading uses
a separate store-status enum with config, format and file-I/O reasons. Retain
the upstream error/alert and a bounded policy-reason code
as diagnostic detail, without making upstream integer values our public ABI.
Peer-controlled certificate strings are not copied raw into diagnostics.
Terminal failures are sticky; reconnect means a new context and fresh entropy.

For clean EOF, require a successful TLS close exchange as reported by the
engine, after buffered authenticated plaintext is drained. If TCP ends while
the engine still needs input, report TRUNCATED. If the peer's close alert has
arrived but our reply is pending, permit output draining before deciding the
terminal result; no public dependence on BearSSL's private closure fields.
Fatal-alert output may be sent best-effort, but a dead transport cannot keep
failure cleanup waiting indefinitely.

## TLS and identity policy

The initial profile is a **TLS 1.2 client**. BearSSL's published
[TLS 1.3 status](https://bearssl.org/tls13.html) says that TLS 1.3 is not
implemented. A TLS-1.3-only origin is outside this profile; a handshake failure
must not trigger HTTP downgrade or certificate bypass.

Proposed cipher suites: ECDHE_RSA and ECDHE_ECDSA with AES-128-GCM,
AES-256-GCM, or ChaCha20-Poly1305. Configure an explicit allowlist rather than
inheriting the upstream full profile. Use its constant-time EC and RSA
implementations; support P-256/P-384/P-521 certificate keys and the selected
upstream ECDHE curves. Verify every offered combination in the harness.
Disable TLS 1.0/1.1, static RSA/ECDH key exchange, CBC/RC4/3DES/NULL suites,
renegotiation, session resumption, and client certificates for this slice.
Client-auth-required peers fail without a retry that weakens policy.

Accept SHA-256/384/512 certificate and handshake signatures supported by the
pinned engine. Refuse SHA-1 signatures in the validation profile. Require
actual RSA modulus bit lengths of at least 2048 and at most the selected
upstream limit, including trust anchors. The upstream minimum-RSA setter
uses byte length and does not cover anchors, so it cannot alone establish
that bit-length policy. Unsupported certificate algorithms fail explicitly.

Require a nonempty ASCII DNS hostname, checked for embedded NUL/control bytes,
label/total length, and valid DNS label syntax. Copy it into the context and
use the same identity for SNI and verification. It comes from the requested
origin, not the DNS result, a reverse lookup, proxy address, or a redirect's
previous origin. IDNA conversion belongs above this boundary; preconverted
ASCII A-labels are accepted. IP-literal verification is deferred and such
inputs return UNSUPPORTED rather than disabling name verification.

The engine accepts an explicit bounded ALPN list. The eventual HTTP/1.x
adapter offers `http/1.1` only and accepts absence of ALPN; it must not offer
`h2` before an HTTP/2 implementation exists. A server-selected unexpected
protocol is an error.

### Certificate verification is a separate acceptance gate

The inspected `src/x509/x509_minimal.t0` verifies signatures, dates, chain
links, Basic Constraints, Key Usage, and names, with important limits. It
does not build alternate paths or fetch intermediates. It does not perform
revocation checks. Its name matching permits CN fallback. Extended Key Usage
(EKU) is not one of its handled extensions: a noncritical EKU can be ignored.
Reference equality with that implementation does not prove a browser's
server-identity policy. See upstream's [X.509 description](https://bearssl.org/x509.html).

The private [policy adapter](TLS_CERTIFICATE_POLICY.md) wraps the upstream
X.509 vtable. It observes certificate bytes while forwarding the original
chain until policy refusal, and makes policy failure override upstream success
before the TLS engine can expose application data. It adds no cryptographic
primitives. Its DER parsing and policy decisions have a generated regression
corpus of signed certificates; review, fuzzing, and production integration
remain validation gates.

Required policy outcomes:

- Require DNS SAN identity; do not accept a CN-only leaf. Use upstream's name
  verification together with the SAN-presence requirement. Restrict wildcards
  to one complete leftmost label. Public-suffix-aware wildcard restrictions
  require a separate policy decision; do not claim full browser equivalence.
- Where an EKU restriction appears in the leaf or an intermediate, require
  explicit `serverAuth`; conservatively refuse an `anyExtendedKeyUsage`-only
  restriction. Absence of EKU adds no restriction. Critical EKU may still be
  refused by the pinned engine even when serverAuth is present; document that
  compatibility refusal rather than suppressing critical-extension errors.
- Refuse Name Constraints or other unimplemented restrictive extensions,
  including noncritical encodings that would otherwise be ignored. The
  policy slice defines its handled/rejected extension table, rejects duplicate
  OIDs, and checks bounded DER structure before accepting a certificate.
- Apply the corresponding checks when loading anchors: converting a
  certificate into a name/public-key pair must not silently discard a scope
  restriction. Initial anchors are explicit, unrestricted CA anchors, not
  direct-trust end-entity keys or arbitrary downloaded intermediates.
- Keep unknown critical extensions fatal. Do not override upstream failure
  to improve compatibility. Test each advertised certificate-policy claim.

No OCSP, CRL fetching, Certificate Transparency, or general path building is
promised. The design is a constrained HTTPS client, not parity with a modern
browser PKI implementation. Revocation/update policy must be stated when a
public root bundle is selected. A verified library port can land with fixture
roots before that public-trust decision is settled.

## Trust-store location and lifecycle

The approved policy resolves a config basename through the conf ladder;
the existing resolver does not accept `certs/roots.pem`. Resolve
`tls.conf` through `os64_conf_find()` and read a `trust_store` setting naming
an absolute PEM-bundle path. The default, when the config or setting is absent,
is `/etc/certs/roots.pem`. A personal `tls.conf` can name a bundle under `/home`.
Reject empty/relative paths and malformed configuration; perform no shell or
environment expansion. No kernel lookup change is needed.

The selected bundle replaces the entire store. Do not silently union stores:
that would make removing trust in a replacement file ineffective.

Load and parse one bounded PEM bundle through the resolved file handle into
a fresh trust snapshot. Accept certificate blocks and ignore bounded ASCII
explanatory text outside them, including certificate names and underline rows.
As specified in TLS_TRUST_STORE.md, hyphen-led lines are reserved for armor;
malformed/unsupported armor, invalid block contents, control/non-ASCII bytes,
read errors, and an empty result fail the load. A bad explicitly selected bundle
does not cause a retry with the default bundle. Report both the selected
config path, if any, and the bundle path. The resolver can conflate some inaccessible-path
cases with absence, so do not promise stronger lookup guarantees than it has.

Proposal limits: 2 MiB PEM input, 256 anchors, 32 KiB DER per certificate,
1 MiB decoded anchor storage. These are refusal limits, not allocation sizes;
check arithmetic and enforce them during parsing. Tune them against the
chosen bundle before freezing the API. Install/update the bundle through a
separately reviewed, provenance-recorded process; no automatic online root
fetching during a handshake. No public roots are selected by this document.

A failed reload preserves the previous snapshot. Existing connections retain
their snapshot; new connections use a successfully published replacement.
Trust-store mutation is serialized by its owner. V1 does not need a global
mutable cache. Local configuration is trusted under os64's current execution
model; TLS cannot defend its trust anchors against a local actor permitted to
rewrite them.

## Entropy: OS service prerequisite

Agreed ownership: Fable builds the entropy pool and `/dev/random` interface;
Quinn builds the CPU survey and BearSSL port. The production TLS adapter reads
that OS interface. The survey reports availability and operational failures;
service readiness and failure behavior require separate integration evidence
on QEMU, VirtualBox, the Bosgame P5 (Ryzen 5 6600H), and the Ryzen 9 3900X.

The reusable **OS random-byte service** is designed and reviewed as a separate
prerequisite, backed by a seeded CSPRNG. It needs an explicit contract
for initialization readiness, source failure, bounded waiting/cancellation,
concurrency, and reseeding. TLS asks for 32 fresh bytes per connection from
that provider and injects them into BearSSL before starting the handshake.
The provider must supply cryptographic unpredictability, not merely produce
32 bytes. BearSSL cannot assess the quality of caller-supplied entropy.

The service read contract refuses with `-1` before the pool is seeded rather
than parking the reader: the device read executes where it may not sleep.
The production adapter maps that refusal to `ENTROPY_UNAVAILABLE` and does
not start the handshake or silently retry in a busy loop. A later attempt
requires a fresh connection. The [input fixture](TLS_PRODUCTION_INPUTS.md)
covers short reads and refusal at each seed offset. The generic read ABI
does not distinguish interruption from other negative returns.

Candidate sources are hardware RNG facilities on supported machines and a
hypervisor entropy device under QEMU. Their availability and trust assumptions
need a hardware survey; neither is implemented by this design. A persistent
seed can supplement initialization only with a reviewed protection/rotation
and VM-cloning story. It is not a fixed secret shipped in the disk image.
Do not treat ticks, RTC values, MAC addresses, task IDs, or a deterministic
PRNG seeded from those values as a production fallback.

Upstream's automatic RDRAND path stays disabled. Source selection and failure
policy belong to the OS service; the TLS adapter does not bypass an unavailable
service by executing CPU RNG instructions itself.

The engine's entropy callback is invoked synchronously during creation;
callbacks return success or a named failure and cannot retain engine pointers.
The production adapter supplies the approved provider. Deterministic providers
are linked only into test executables, with no production seed environment
variable, test switch, or deterministic fallback. Wipe temporary seed bytes.
Do not reuse live contexts across fork/snapshot restore as though their random
state were fresh; OS randomness must address VM cloning if that is supported.

The port, crypto vectors, and controlled host/guest engine tests can proceed
without the OS service. Production network handshakes remain gated on the
approved service. This document authorizes neither
kernel changes nor an entropy implementation.

## Time, limits, and transport progress

Sample `os64_time().epoch` once for certificate validation at connection
creation. Use UTC directly, ignoring the timezone offset. Convert with signed
floor division: `days = floor(epoch / 86400) + 719528`, with the nonnegative
remainder as seconds within the day. Check the destination range and reject
unrepresentable dates. Unit-test negative epochs, midnight, leap days, and
dates beyond 2038. A successful clock syscall does not attest to clock
correctness; os64's RTC/system clock is the trust assumption. Do not replace
an implausible clock with the build date or disable validity checks.

Use monotonic time for waits. Proposed adapter defaults: 30 seconds total for
the handshake and 2 seconds for orderly shutdown. Application read deadlines
are supplied by the caller. Dribbling bytes or warning alerts do not restart
the handshake deadline. The nonblocking engine itself reports progress and
does not own a scheduler timer.

Bound an incoming chain to 8 certificates, 32 KiB per certificate, and
256 KiB total DER, enforced at the X.509 callback boundary before copying.
Permit at most 1 MiB of received TLS bytes during the initial handshake;
count across calls so empty records/warnings cannot bypass the bound.
Use checked counters and report LIMIT rather than malformed input for a valid
but over-budget chain. Allocate fixed certificate-policy scratch at creation.
Exact memory and stack budgets must be measured with the pinned build.

### Bounded TCP progress

`os64_read_for()` and `os64_write_for()` provide finite waits. Write queues
while room exists, then reports progress or timeout at an expired full-ring
wait. Its [patience contract](abi/include/os64/syscall_numbers.h) leaves the
connection open on timeout. `os64_write()` supplies infinite patience, and
owned TCP persist state has no overall lifetime bound; a transport adapter
must use a finite write patience.

The adapter owns one total budget across reads, writes and engine progress.
Ready ring space may produce a successful write even after its wait deadline,
so the adapter must check its own clock between calls. Kernel waits do not
bound scheduling latency or copying/submission work. Do not add an ad hoc
writer thread that shares the engine or races handle close to simulate
cancellation.

Adapter byte ownership is explicit: a ciphertext chunk taken from the engine
stays in one bounded pending-output buffer until fully written. A short write
advances its offset; an error aborts the connection. Never regenerate already
accepted plaintext to retry an encrypted suffix. Retain unconsumed input too.
The driver alternates ready directions and drains authenticated plaintext
without unbounded buffering. On zero progress it waits for the required event
or returns to its caller; it does not busy-loop or insist on writing forever
while the peer needs its input drained.

TLS EOF remains distinct from HTTP body completion. The future HTTP adapter
must not let a raw transport FIN finish a close-delimited HTTPS body. Any
compatibility policy for missing `close_notify` after an independently framed
complete HTTP body belongs to that later integration design. Preserve
os64get's existing provisional-output and gzip-final-validation guarantees.

## Evidence plan

### 1. Upstream vectors and a separately built reference

Run the pinned upstream `test_crypto`, `test_math`, and `test_x509` suites on
the host. Record the tested algorithms and any deliberate exclusions. Keep
tests for disabled production algorithms separate from claims about what the
production profile offers. Adapt representative vectors into `/tests/tlstest`
so the cross-compiled guest exercises the same production code paths.

Build two host executables: pristine upstream under a matching explicit
algorithm/platform profile, and the os64 adaptation. Independent builds avoid
accidentally resolving both sides to one set of symbols. Use fixed fixture
entropy, certificates, time, peer configuration, and input schedules.
Compare crypto outputs, record bytes and plaintext where deterministic, plus
consumed lengths, normalized states, and failure classes. Test-only seeds and
key material are labeled public fixtures.

Match record-fragmentation/flush schedules before demanding wire equality.
Different valid packetization or randomized signatures are not defects; use
semantic comparison there. Document expected policy differences explicitly.
The reference detects port divergence, while known-answer vectors and an
independent TLS implementation provide evidence against shared upstream bugs.

### 2. Host protocol, policy, and fault fixtures

Use an independent local TLS peer (OpenSSL or Python's SSL adapter), with
generated fixture CAs and a controlled verification time. Required cases:

- Each enabled cipher/authentication combination; version/ALPN mismatch;
  fragmented and coalesced records; one-byte input/output capacities; flush
  behavior; backpressure; multiple independent contexts.
- Wrong hostname, CN-only leaf, SAN/CN disagreement, wildcard boundaries,
  embedded NUL, untrusted root, expired/not-yet-valid chain, invalid
  signature, malformed DER, CA/path-length violations, wrong Key Usage/EKU,
  unknown critical extensions, constrained anchors, and unsupported keys.
- Tampered ciphertext/tag, replayed/reordered records, premature application
  records, renegotiation requests, abrupt EOF at each handshake/record stage,
  clean close with buffered plaintext, peer close while output is pending,
  and fatal alerts whose transport cannot be flushed.
- Partial writes/reads, timeout distinct from EOF, cancellation, failed
  entropy acquisition, allocation failure at each allocation site, trust
  reload failure, size limits, and overflow boundaries.

Failure fixtures assert the useful invariant: no unauthenticated plaintext,
no secret-dependent fallback, no lost/duplicated accepted bytes, no invalid
free, and bounded memory/work. A success-only handshake is insufficient.

### 3. Fuzzing and sanitizers

Run ASan/UBSan and coverage-guided fuzz targets for ciphertext ingestion,
certificate-policy parsing, trust-bundle decoding, and public API operation
sequences. Mutate valid handshake fixtures and fragment schedules as well as
raw bytes. Transcript mutations naturally fail Finished/MAC checks; use a
controlled peer and direct parser targets to reach deeper states too.
Persist crashes as small regressions with seed and tool versions. Track
corpus, iterations/time, coverage, and resource ceilings; report the actual
campaign rather than saying the library is simply "fuzzed".

Run the adapted build through the production private memory helpers during
host tests. Sanitizer substitutions must not accidentally bypass the port.
If the environment prevents LeakSanitizer, record that limitation separately
from ASan/UBSan results. Timing measurements can detect gross regressions but
do not establish constant-time behavior by themselves.

### 4. Guest and later network evidence

Strict cross-build, symbol/relocation audit, guest vector runner, and repeated
create/fail/free cycles precede a TLS network claim. Once entropy and bounded
transport prerequisites are met, run controlled TLS peers through QEMU with
fragmentation, resets, slow input, and the project's network chaos harness.
Verify the actual boot root and linked libtls artifact. Compare fetched bytes
against the host reference. Real-site smoke tests supplement controlled
fixtures; changing Internet behavior is not the regression oracle.

## Reviewable slices and decisions

| Slice | Deliverable | Completion gate |
|---|---|---|
| A: provenance and dependency spike | Selected commit, source/patch manifest, freestanding object build, explicit profile | Upstream delta reviewed; unsupported dependencies identified; no production-readiness claim |
| B: engine port | `libtls.so`, opaque byte API, private adapters, host reference harness, guest vectors | Matching vectors, ownership/failure tests, strict build and guest evidence |
| C: trust and certificate policy | Bounded bundle loader, policy adapter, identity regression corpus | Explicit policy outcomes tested independently; trust-store selection ruled |
| D: production inputs and transport | Approved entropy provider and bounded I/O adapter | Entropy/failure and end-to-end deadline/cancellation evidence; prerequisites landed through their own reviews |
| E: HTTPS integration | HTTP caller adoption, routing and publication tests | Separate integration design reviewed against the merged tree |

The source pin is settled for the foundation. The public byte library preserves the
reviewed engine operations and profile. OS randomness ownership, unseeded-read
refusal, and root-store selection/replacement are agreed. Production integration
must verify the service behavior. Further decisions are public root-bundle
selection and acceptance of the documented TLS 1.2/PKI compatibility limits.

Port vectors do not depend on those later integration decisions. Work on the
pure library can proceed once its design is agreed; no live HTTPS claim is
made until the relevant gates have evidence.
