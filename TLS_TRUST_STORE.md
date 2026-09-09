# TLS trust-bundle loading

This slice builds on the private client and certificate-policy factory. It
loads administratively selected roots into that factory's sealed snapshots.
It does not choose public roots or enable native HTTPS.

## Selection and publication

Resolve the basename `tls.conf` with `os64_conf_find`. With no resolved config,
or no `trust_store` setting, use `/etc/certs/roots.pem`. A setting names one
absolute path; there is no union with defaults, expansion, or network fetch.
Report the resolved config path and selected bundle path on success or failure.
The resolver conflates some inaccessible/oversized paths with absence, as
documented in TLS.md; this slice cannot strengthen that kernel contract.

Read the resolved config with the existing bounded `os64_slurp` helper and its
open/read/error contract (the helper does not propagate close errors). Parse
the complete returned bytes, not a NUL-terminated prefix. The shared
`os64_conf_read` is unsuitable for trust selection: its tolerant parsing skips
empty keys and stops at embedded NUL. This slice therefore has a narrow strict
parser for the single allowed `trust_store` setting, using the existing
`key = value` dialect: spaces/tabs around fields, `#` comments, case-insensitive
keys, and the last valid occurrence wins. Unknown keys, malformed lines, empty
or relative values, NUL/control bytes, and oversized config/path values refuse
the load. A bad earlier setting is an error even if a later line overrides it.
There is no quoting or escaping; spaces inside a path are literal.

Unknown keys are a deliberate departure from the shared reader's tolerant
behavior: a typo such as `trust_stroe` must not silently select the default
roots. This trust-selection setting favors a visible refusal over forward
compatibility with unknown settings; os64 userland is refreshed as one image.

Open the selected PEM bundle once and read it through that handle. The parser
consumes a bounded stream into a fresh builder, then seals it after clean EOF.
An open/read/close error, invalid certificate, or malformed suffix discards the
fresh builder. A successful reload swaps the owner's pointer and releases its
old reference; failure leaves that pointer intact. Existing validators retain
the old snapshot. Reload and factory acquisition are serialized by the owner;
this is not a global cache or lock-free publication API.

## Accepted PEM form and limits

Accept ASCII `BEGIN CERTIFICATE` / `END CERTIFICATE` blocks. Ignore blank lines
and explanatory text outside blocks, including `#` comments, certificate names,
and `=` underline rows in curl's Mozilla-derived `cacert.pem` format.
[RFC 7468 sections 2 and 5.2](https://www.rfc-editor.org/rfc/rfc7468.html#section-5.2)
permit explanatory text. After trimming spaces/tabs, lines beginning with `-`
are reserved for armor: malformed, truncated, unmatched or unsupported armor
refuses the load, including after a valid certificate. Boundaries are complete
lines; surrounding spaces/tabs are permitted. Line endings are LF or CRLF,
with an optional final newline. Body lines contain standard Base64 plus spaces/tabs. Require complete
quartets, canonical padding/unused bits, matching boundaries, and nonempty
decoded certificates. Refuse other block types, nested blocks, headers,
comments or other explanatory text inside blocks, and truncated blocks.
Control bytes other than the documented whitespace, non-ASCII bytes and size
violations are refused even in ignored explanatory text. A file containing
only explanations still fails because it supplies no roots.

This supports curl's outer text format; individual roots still must pass
`TLS_CERTIFICATE_POLICY.md`. It does not establish that the complete curl or
Debian root set satisfies that policy.

| Resource | Limit |
|---|---|
| Config content | 8191 bytes (the shared config reader's content cap) |
| Config or bundle path | 255 bytes plus NUL |
| PEM input | 2 MiB; probe EOF at the exact cap |
| PEM line | 1024 bytes, excluding LF |
| Decoded certificate | 32 KiB |
| Anchors / copied DN and key bytes | 256 / 1 MiB, enforced by the policy builder |

The streaming parser owns a fixed certificate/line/read workspace. It does
not allocate the 2 MiB input limit. Short reads are normal; negative reads
are errors, and a complete valid prefix is not a successful load. The reader
callback is synchronous, returns at most the requested byte count, and must
not reenter the parser. The file adapter supplies ordinary os64 reads; no
network or file-read deadline is promised by this private interface.

## Validation boundary

Run `python3 tools/test_tls_store_host.py` for ASan/UBSan coverage. Like the
other TLS harnesses, it accepts `--foundation PATH` to reuse a matching adapted
archive and `--output /tmp/new-directory` to retain artifacts. On hosts where
LeakSanitizer cannot run under tracing, set `ASAN_OPTIONS=detect_leaks=0`.
Fixture allocation counts independently check owned cleanup.

The host tests compile the real `os64_slurp` reader with injected file I/O.
They cover annotated two-root bundles, malformed armor and Base64, read splits
from one byte through full reads, I/O failures at each byte of a fixture, exact input/config/path/DER caps,
config selection and no fallback, allocation and bundle-close failures,
whole-store replacement, and old-validator lifetime.

In os64, run `testrun tlstrusttest`. The guest runs the shared parser/config
checks, then writes fixture bundles in an exclusively created
`/tmp/tlstrust-<taskid>` directory. It verifies root A being replaced by root B,
an existing validator retaining root A, and a malformed reload preserving root
B. It removes its files/directory on completion. It does not edit `tls.conf`
or the real trust store. Config-ladder fault cases are exercised on the host;
the guest file test uses explicit-path loading. The fixture verifies selected
certificate paths, not a TLS exchange or the full policy-negative corpus.

`tools/generate_tls_trust_fixtures.py` regenerates the checked-in public roots
and leaf certificates from deliberately trivial test keys; it requires Python
cryptography. The ordinary host and guest tests use the checked-in bytes and
do not generate keys or consult the wall clock. These fixtures are not a root
bundle for deployment. Public-root compatibility, provenance/update policy,
and production entropy/transport integration remain separate work.
