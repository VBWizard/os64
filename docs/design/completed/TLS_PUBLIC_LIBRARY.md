# Public TLS byte library

`/lib/libtls.so` exposes the byte operations and trust snapshots described in
TLS.md through `<tls/tls.h>`. It depends on `libos64.so`; applications select
it explicitly. The implementation uses the private engine, policy, loader,
and production-input constructor. Byte clients own no TCP handle or transport
timer. The separate [transport API](TLS_TRANSPORT.md) in the same shared
library owns those resources around a byte client.

The public header contains os64 enums, value structs and opaque client/trust
types, with no BearSSL headers or entropy/time/validator injection callbacks.
The private headers alias those value types so layouts and status values have
one definition. Public declarations gain default visibility when compiling
shared-library objects; the foundation/test objects keep hidden visibility.
A linker export list restricts the shared ABI to the declared operations.

Creation borrows a sealed trust snapshot and hostname/ALPN config, copies the
names, and retains its own trust reference. It samples OS UTC and reads fresh
OS entropy using the production-input constructor. Each client has one
serialized owner. A failed creation clears the output and releases partial
state. The caller may release its snapshot after successful creation.

Transfers preserve the engine's accepted-prefix count, including when that
prefix reaches an error. Plaintext is available only after policy acceptance.
The caller owns pending ciphertext after taking it and must retain any suffix
across short writes. EOF, orderly close, abort and destruction preserve the
reviewed engine behavior. No byte operation waits for network I/O; a caller
must supply bounded transport progress before claiming a handshake deadline.

The public trust builder, PEM reader and reload operations use the existing
bounded loader. Reload is serialized by the owner, publishes only a complete
sealed snapshot and leaves the old store untouched on failure. Connections
retain the snapshot they acquired. Local config is trusted; there is no new
root bundle or certificate-policy override.

The shared build uses a separate object directory and selects core sources in
`userland/libtls/public_sources.mk`. The audit checks archive membership
against the objects extracted by the shared link. Test vectors, fixture keys and deterministic
providers are excluded from the library. `os64_tls_license()` returns the
upstream notice generated from the pinned LICENSE.txt, without fixture code.
The prelinked base is assigned together with the other shared libraries;
root image rules install the library for FAT and ext2.

Validation includes the existing engine/input/store host suites after the
shared type-header change, exact ELF exports/imports and dependency checks,
and a guest consumer compiled against the public header and shared library.
`testrun tlslibtest` covers construction, trust lifetime, byte-prefix progress,
pre-authentication plaintext refusal and sticky aborts. A controlled host TLS
peer exercises the public wrappers across authenticated application traffic.

The TCP API provides finite read and write
[patience](abi/include/os64/syscall_numbers.h). The [transport driver](TLS_TRANSPORT.md)
combines them with retained handshake/shutdown budgets and preserves pending
ciphertext across short writes. Public-root distribution and HTTPS
integration remain separate work. This library makes the byte API usable
without claiming that an os64 application can yet make a bounded native
HTTPS request.

Run `python3 tools/audit_tls.py` for the public ELF boundary and
`python3 tools/audit_bearssl.py` for the private foundation boundary.
`tools/test_tls_inputs_host.py --public` exercises the public entry points;
the same command without `--public` exercises the private constructor.
Both support the foundation archive reuse and sanitizer options documented
in the foundation README. `tools/test_tls_engine_host.py` and
`tools/test_tls_store_host.py` cover the shared value types through the
existing engine and store regression suites.

## Error display

`os64_tls_error_description(status, policy_reason, upstream_error)` translates
one state snapshot into static, allocation-free display text. It prioritizes a
local certificate-policy refusal, otherwise explains recognized upstream
certificate failures and received fatal alerts. Alert names follow
[RFC 5246 appendix A.3](https://www.rfc-editor.org/rfc/rfc5246.html#appendix-A.3)
and the pinned engine's extension alerts. Unknown diagnostics retain a bounded
generic description. The wording is not a machine-readable ABI; applications
keep using the status and policy enums for control flow. `os64get` prints this
explanation alongside its existing numeric detail. A `protocol_version` alert
means the peer rejected the offered version; it does not by itself establish
which other versions the peer supports.
