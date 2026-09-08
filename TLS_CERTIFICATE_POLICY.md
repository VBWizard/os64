# Certificate policy and trust snapshots

This private slice supplies the owned validator factory used by
`userland/libtls/port/client_engine.h`. It builds on the engine and the pinned
BearSSL foundation. It installs no public API or root bundle. The PEM/config
loader, production entropy adapter, transport, and HTTPS integration are
separate slices in TLS.md.

## Acceptance gate

The wrapper forwards the original certificate stream to BearSSL minimal X.509
until policy refusal and inspects each complete certificate in a fixed heap
buffer. `end_chain`
requires both upstream success and policy success; `get_pkey` returns NULL
until that gate succeeds. Policy errors cannot turn an upstream failure into
success. Inspection continues through certificates supplied after upstream
has found an anchor. A malformed or restricted trailing certificate therefore
refuses the connection, even if upstream would ignore it.

The factory copies the expected DNS hostname and uses explicit validation time.
It configures the same SHA-256/384/512, RSA i31, and EC m31 algorithms as the
client profile. RSA moduli must have 2048 through 4096 actual bits, including
anchors; exponents must fit the upstream key buffer, be odd, and be at least 3.
EC keys use uncompressed, valid P-256/P-384/P-521 points. Certificate signature
identifiers must be SHA-256/384/512 with RSA PKCS#1 v1.5 or ECDSA, with matching
inner/outer identifiers. This conservatively also applies to supplied roots.

The leaf must contain a matching DNS SAN. DNS names use ASCII labels of 1–63
characters and a maximum total length of 253. Wildcards occupy a complete
leftmost label and match one label. CN does not supply identity. There is no
public suffix database or IP identity support. Non-DNS GeneralNames do not
supply identity; malformed DNS names refuse the certificate even if a sibling
name matches. The leaf cannot be a CA; intermediates must be CAs. Key Usage,
when present, requires digitalSignature in the leaf and keyCertSign in CAs.
BearSSL remains responsible for signatures, chain links, validity periods,
and intermediate path-length enforcement.

## Extension table

Extension OIDs must be unique within a certificate. DER envelopes, lengths,
OIDs, booleans, integers, and bit strings are checked for canonical encodings.
The parser checks the schema of policy-bearing fields; it is not a general
ASN.1 schema validator for every informational extension.

| Extension | Rule |
|---|---|
| Basic Constraints | Parse CA and pathLen; upstream enforces chain pathLen. Anchors require CA and refuse pathLen. |
| Key Usage | Parse canonical named bits; require the role's signing bit when present. |
| Subject Alternative Name | Parse GeneralNames; require a matching DNS identity for the leaf. |
| Extended Key Usage | Nonempty unique OID list; require explicit serverAuth in leaf/intermediates. anyExtendedKeyUsage alone fails. Anchors refuse EKU because the converted anchor would lose it. Critical EKU remains an upstream compatibility refusal. |
| Subject/Authority Key Identifier | Accept noncritical metadata. |
| Authority/Subject Information Access | Accept noncritical metadata; no fetching. |
| CRL Distribution Points, Freshest CRL | Accept noncritical metadata; no revocation claim. |
| Certificate Policies | Accept noncritical metadata; no policy-tree claim. |
| Name Constraints, Policy Constraints, Policy Mappings, Inhibit Any Policy | Refuse, including noncritical forms. |
| Other extensions | Refuse, including noncritical forms. Expanding the allowlist needs its own policy justification and fixtures. |

Critical informational extensions are refused, including ones ignored by
the pinned upstream engine. ASN.1 inside accepted informational values gets
bounded structural DER checks but no semantic interpretation. This deliberately
trades compatibility for a small, explicit acceptance surface. Revocation,
Certificate Transparency, alternate path building, and public-browser parity
are not provided. The table follows the restriction semantics in
[RFC 5280](https://www.rfc-editor.org/info/rfc5280/) and the DNS SAN identity
boundary described in TLS.md. The pinned source, rather than the broader
[BearSSL overview](https://bearssl.org/x509.html), defines upstream behavior.

## Ownership and bounds

Trust construction accepts explicit DER CA certificates. Anchors must be
self-issued (identical encoded issuer/subject), with no EKU or pathLen. This
is an administrative trust input, not automatic promotion of downloaded
intermediates. A self-signature is not the source of trust and is not verified
at import. Anchor validity dates are metadata, not a validation-time constraint;
the trust decision and subsequent removal belong to the store owner.

Addition copies the subject DN and public key after policy checks. A failed
addition leaves the builder unchanged. Sealing requires a nonempty store and
forbids later additions. Validators retain sealed snapshots with atomic
reference counts; the owner must hold a live reference while creating a
validator. Construction/mutation/publication is serialized by the caller.
Releasing an old owner reference cannot invalidate existing validators.

| Resource | Refusal limit |
|---|---|
| DER certificate | 32 KiB |
| Supplied chain | 8 certificates, 256 KiB total |
| Extensions per certificate | 32 |
| EKU entries | 32 |
| DER nesting / elements | 16 / 4096 per certificate |
| Trust anchors | 256 |
| Copied anchor DN/key bytes | 1 MiB |

Connection creation allocates a fixed validator working set, including one
32 KiB certificate buffer. Certificate lengths never drive allocations during
the handshake. Diagnostics use bounded enum reasons plus the upstream error;
they do not copy certificate strings. The types remain private and may change
when the public libtls boundary is introduced.

## Validation

`python3 tools/test_tls_policy_host.py` builds an OpenSSL-backed certificate
corpus using Python cryptography and exercises the production factory under
ASan/UBSan. Like the engine harness it accepts `--foundation PATH` to reuse a
matching adapted archive. Python cryptography and the OpenSSL command-line
tool are host test dependencies; generation and validation need no network.
Use `--output /tmp/new-directory` to retain the generated corpus and executable.

The corpus covers 81 chain cases and 15 anchor cases, with one-byte,
37-byte, and whole-certificate delivery. It checks successful EC/RSA chains,
SAN/CN/wildcard boundaries, leaf/intermediate EKU, critical-extension refusals,
restrictions after upstream trust success, signature/date/pathLen failures,
RSA sizes from 1024 to 4097 bits, invalid EC points, malformed DER, duplicate
extensions, and resource limits. Each proper prefix of a valid leaf is refused.
Direct upstream comparisons demonstrate otherwise accepted policy negatives.
OpenSSL independently validates the positive chain with server purpose and DNS
identity enabled.

Five complete or rejected TLS handshake probes use the policy factory and
the pinned BearSSL server. They verify successful authentication and refusal
before plaintext access for CN-only, unsuitable EKU, critical EKU, and a
restricted trailing certificate. This is not independent-peer TLS
interoperability. Allocation-failure injection, copied-input lifetime, owner
release before validation, and four concurrent validator owners exercise
snapshot ownership. Handshake certificate processing performs no allocations.

The x86-64 validator allocation is 36,264 bytes; a trust builder/snapshot is
18,456 bytes plus copied DN/key bytes. The private sources compile with the
existing freestanding PIC rule and `-Werror`. Compiler stack reports are
per-function measurements, not a bound on the crypto call chain.

ASan/UBSan are enabled in the harness. On hosts where LeakSanitizer cannot
run under tracing, use `ASAN_OPTIONS=detect_leaks=0`; the fixture's allocation
counts still check owned cleanup. Guest execution, a sustained fuzzing campaign,
public-root compatibility and updates, bundle/config loading, and independent
TLS-peer interoperability remain separate validation and integration work.
