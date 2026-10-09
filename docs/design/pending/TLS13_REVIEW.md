# Design review of TLS13.md

Latest result: **round 3, CLEAN — no actionable findings**. The original
eight findings and the round-2 regression are addressed in the design.
See [Round 3](#round-3). Earlier rounds are retained as the review record.

Quinn, 2026-10-09, for Fable and Chris. Review of the untracked
[TLS13.md](TLS13.md) draft against the implementation at `32ae9d8e` on
`userland`. Draft SHA-256:
`8299553862099b83264fcde3dc22cd445ed638af9842d652f21352bc3461ae74`.
Line references below identify that draft, before revisions.

Keep the second engine behind the existing byte interface, the shared
certificate-policy gate, and the three slices. Revise the builder's brief
before implementation. Chris's six rulings remain constraints; this review
does not replace them. Finding 1 identifies a conflict between the proposed
wire format and the promised fallback behavior, rather than a request to
abandon fallback.

There are eight findings: one P1 and seven P2s. Finding 1 has an OpenSSL
reproduction. Findings 2–7 follow from the specified behavior, RFC rules,
and the retained validator; finding 8 follows from the existing wrapper.
These are design findings, not claims of bugs observed in a TLS 1.3 engine
that has already been built. No implementation or design changes accompany
this review.

## 1. [P1] The ClientHello does not deliver the specified TLS 1.2 fallback

**Location:** profile lines 149–169; version selection lines 211–233;
acceptance lines 505–508. **Slice:** S2.

Version-aware servers select from `supported_versions`, independently of
the cipher-suite list. [RFC 8446 §4.2.1](https://www.rfc-editor.org/rfc/rfc8446.html#section-4.2.1)

The proposed hello offers only `0x0304`. On this host, OpenSSL 3.0.13 pinned
to TLS 1.2 returns fatal `protocol_version`, not a ServerHello. Including
the six TLS 1.2 suites does not change that. The caller therefore cannot
reach `PEER_CHOSE_TLS12`, and the stated Python `maximum_version` acceptance
case fails as designed. This is a regression for a peer our Bear engine can
currently use.

**Correction:** reconcile the version advertisement and engine handoff with
the approved sentinel-free ServerHello retry. Do not silently broaden retry
to alerts. The two-version control below establishes the cause; it does not
by itself prove a complete replacement negotiation design. A genuinely old
TLS 1.2 implementation that ignores the extension is a separate peer case.

**Check:** exercise both a modern implementation configured for TLS 1.2 and
a legacy TLS 1.2 implementation. Keep the downgrade-sentinel refusal case.

## 2. [P2] The hello advertises certificate signatures the validator rejects

**Location:** profile lines 160–169. **Slice:** S2.

Without `signature_algorithms_cert`, `signature_algorithms` also advertises
certificate-signature support. [RFC 8446 §4.2.3](https://www.rfc-editor.org/rfc/rfc8446.html#section-4.2.3)

The proposed list includes RSA-PSS. However,
[`signature_algorithm()`](../../../userland/libtls/port/certificate_der.c)
accepts RSA PKCS#1 and ECDSA signature OIDs, and
[`minimal_init()`](../../../userland/libtls/port/certificate_policy.c)
installs PKCS#1 RSA verification. Adding PSS verification for
CertificateVerify does not change either certificate path. A server can
select a PSS-signed chain in response to our advertised capability and then
be rejected by the retained validator.

**Correction:** add a certificate-signature allowlist matching the retained
certificate policy, while keeping PSS in the CertificateVerify offer.
Document PSS-signed certificate chains as a profile limitation unless that
validator support receives its own implementation work.

**Check:** inspect both advertised lists; distinguish an RSA leaf with a
PKCS#1 certificate signature and PSS CertificateVerify from a PSS-signed
certificate chain.

## 3. [P2] The CCS window rejects valid retry handshakes

**Location:** record layer lines 255–260; acceptance lines 515–516.
**Slices:** S1 record handling, S2 handshake integration.

The permitted plaintext CCS window starts after the first ClientHello and
ends at the peer's Finished. [RFC 8446 §5](https://www.rfc-editor.org/rfc/rfc8446.html#section-5)

The draft instead rejects CCS before ServerHello. In an HRR exchange,
`HelloRetryRequest → CCS → final ServerHello` can therefore fail before
the requested P-384 exchange happens. HRR uses the ServerHello wire type,
but acceptance must not depend on reaching the final ServerHello state.

**Correction:** use the ClientHello-to-peer-Finished window, including the
retry states. Preserve refusal of malformed or encrypted CCS.

**Check:** CCS before the first server handshake message, after HRR, after
final ServerHello, and after Finished; include encrypted and malformed CCS.

## 4. [P2] HelloRetryRequest incorrectly requires a group change

**Location:** handshake step 2a, lines 363–372. **Slice:** S2.

A cookie-only HRR is legal; `key_share` is optional in HRR.
[RFC 8446 §4.1.4](https://www.rfc-editor.org/rfc/rfc8446.html#section-4.1.4)

The draft requires a named group and replaces the shares with a newly
generated share for it. That branch cannot handle a server requesting only
a cookie, even though the existing shares are acceptable.

**Correction:** retain the original shares for cookie-only HRR and add the
cookie. Apply the requested-group checks and replacement only when HRR
contains `key_share`. Preserve the single-retry restriction.

**Check:** cookie-only and cookie-plus-group HRRs, unchanged shares in the
cookie-only second ClientHello, and refusal of an HRR that changes nothing.

## 5. [P2] EncryptedExtensions rejects a legal supported_groups response

**Location:** handshake step 3, lines 373–379. **Slice:** S2.

Servers may return `supported_groups` in EncryptedExtensions.
[RFC 8446 §4.2.7](https://www.rfc-editor.org/rfc/rfc8446.html#section-4.2.7)

We offer that extension, but the draft permits only SNI and ALPN in the
response. A conforming peer's preference announcement would terminate an
otherwise usable connection.

**Correction:** validate and accept the group list. The client need not
retain it for later connections or change the already selected group.

**Check:** complete a handshake with this extension present, including
groups outside our own allowlist; reject malformed list framing.

## 6. [P2] Receive limits need the complete inner-plaintext bound

**Location:** record layer lines 251–269. **Slice:** S1.

The full TLSInnerPlaintext, including type and padding, is limited to
16,385 bytes. [RFC 8446 §5.4](https://www.rfc-editor.org/rfc/rfc8446.html#section-5.4)

The draft states the generic ciphertext bound and a plaintext-record bound,
but does not distinguish content length after padding removal from the
complete decrypted buffer. Checking only unpadded content permits excessive
padding inside the larger generic ciphertext allowance.

**Correction:** enforce the full inner-plaintext limit before stripping
padding or exposing content. Keep the outer length check too.

**Check:** valid-tag records at and above the inner limit, including a short
content payload whose padding alone puts it over the limit. Oversized
records must fail without releasing plaintext.

## 7. [P2] Handshake framing needs explicit record-boundary checks

**Location:** handshake framing lines 342–346; KeyUpdate lines 416–425.
**Slice:** S2, with the record interface retaining boundary information.

Handshake fragments cannot interleave other record types; messages preceding
key changes must end at record boundaries.
[RFC 8446 §5.1](https://www.rfc-editor.org/rfc/rfc8446.html#section-5.1)

The draft's prohibition on mixing types within one encrypted record does
not cover an application record between two fragments of a handshake
message. Its prohibition on a message spanning a key change also needs an
explicit check for trailing bytes after a key-changing message. For
example, KeyUpdate followed by another handshake message in the same
old-key record must not be processed as an ordinary coalesced flight.

**Correction:** retain the record-boundary information in the handshake
parser and check both conditions before advancing state or installing keys.

**Check:** a fragmented post-handshake message interrupted by application
data, trailing handshake bytes after KeyUpdate, and trailing bytes after
ServerHello or Finished. Preserve valid fragmentation and coalescing cases.

## 8. [P2] The unchanged wrapper cannot report the promised LIMIT status

**Location:** unchanged-wrapper commitment near lines 101–107; message caps
lines 429–437. **Slice:** S2.

The draft promises `BR_ERR_TOO_LARGE → LIMIT`. Existing
[`classify()`](../../../userland/libtls/port/client_engine.c) maps that error
to `TLS_PROTOCOL`: its limit branch recognizes a validator policy limit,
not an engine message-size refusal. Dispatching the new engine through the
same unchanged classifier cannot meet the public acceptance requirement.

**Correction:** explicitly include engine-limit classification in the
wrapper delta, with a deliberate mapping for the new refusal paths. Keep
classification owned by the shared wrapper.

**Check:** exceed the 8 KiB non-streamed-message cap through the public byte
interface and assert `OS64_TLS_LIMIT`, retained upstream detail, accepted
prefix count, and sticky terminal behavior. Retain the certificate-limit
and pre-handshake-input-limit regressions.

## Reproduction for finding 1

Run the Python block below from the repository root. It requires Python's
`ssl` module and the host fixture dependency `cryptography`. It uses
MemoryBIO rather than sockets, the repository's public fixture certificate
and key, and fixed test-only ephemeral keys. It tests the server's first
response, not a completed authenticated connection.

Observed with `OpenSSL 3.0.13 30 Jan 2024`:

| Offered versions | Server result |
|---|---|
| `0x0304` only, as designed | `UNSUPPORTED_PROTOCOL`; alert record `15030300020246` |
| `0x0304, 0x0303` control | ServerHello; waiting for the client's next flight |
| Extension omitted control | ServerHello; waiting for the client's next flight |

The alert payload is `02 46`: fatal, `protocol_version` (70). The controls
change only the supported-versions extension; the cipher suites, other
extensions, random, session ID, and shares stay fixed. Server-generated
random bytes and later flight bytes vary between runs.

```python
import ast, base64, re, ssl, tempfile
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ec, x25519
from cryptography.hazmat.primitives import serialization
ROOT = Path.cwd()  # Run from the repository root.
u16 = lambda n: n.to_bytes(2, 'big')
v16 = lambda b: u16(len(b)) + b
ext = lambda n, b: u16(n) + v16(b)
xkey = x25519.X25519PrivateKey.from_private_bytes(bytes(range(32)))
xpub = xkey.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
pkey = ec.derive_private_key(3, ec.SECP256R1())
ppub = pkey.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
def hello(versions):
    host = b'localhost'
    exts = ext(0, v16(b'\x00' + v16(host)))
    exts += ext(10, v16(b''.join(u16(n) for n in (29, 23, 24))))
    exts += ext(13, v16(b''.join(u16(n) for n in (0x0403,0x0503,0x0603,0x0804,0x0805,0x0806,0x0401,0x0501,0x0601))))
    exts += ext(16, v16(b'\x08http/1.1'))
    if versions is not None:
        vs = b''.join(u16(v) for v in versions)
        exts += ext(43, bytes([len(vs)]) + vs)
    exts += ext(51, v16(u16(29) + v16(xpub) + u16(23) + v16(ppub)))
    suites = b''.join(u16(n) for n in (0x1301,0x1302,0x1303,0xcca9,0xcca8,0xc02b,0xc02f,0xc02c,0xc030))
    body = b'\x03\x03' + bytes(range(32)) + b'\x20' + bytes(range(32,64)) + v16(suites) + b'\x01\x00' + v16(exts)
    msg = b'\x01' + len(body).to_bytes(3,'big') + body
    return b'\x16\x03\x03' + v16(msg)
source = (ROOT/'userland/libtls/test/trust_vectors.c').read_text()
body = re.search(r'tls_fixture_a_leaf\[\] = \{(.*?)\};', source, re.S)[1]
der = bytes(int(h,16) for h in re.findall(r'0x([0-9a-f]{2})', body))
tree = ast.parse((ROOT/'tools/test_tls_transport_peer.py').read_text())
key = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id == 'FIXTURE_KEY_PEM' for t in n.targets))
print(ssl.OPENSSL_VERSION)
with tempfile.TemporaryDirectory() as d:
    certpath, keypath = Path(d)/'cert.pem', Path(d)/'key.pem'
    certpath.write_bytes(b'-----BEGIN CERTIFICATE-----\n'+base64.encodebytes(der)+b'-----END CERTIFICATE-----\n')
    keypath.write_bytes(key)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers('ECDHE-ECDSA-AES128-GCM-SHA256')
    ctx.load_cert_chain(certpath,keypath)
    for label, versions in [('design: TLS 1.3 only',[0x0304]), ('control: TLS 1.3 + 1.2',[0x0304,0x0303]), ('control: extension omitted',None)]:
        incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
        server = ctx.wrap_bio(incoming,outgoing,server_side=True)
        incoming.write(hello(versions))
        try:
            server.do_handshake()
            result = 'complete'
        except ssl.SSLWantReadError:
            result = 'waiting for client flight'
        except ssl.SSLError as e:
            result = str(e)
        wire = outgoing.read()
        print(label, '\n ', result, '\n  response prefix:', wire[:12].hex(), 'bytes:',len(wire))
```

## Round 2

Quinn, 2026-10-09. Re-read the revised design in full and checked the eight
original findings against the changes. Revised draft SHA-256:
`c66c95c3a37dad0d6cd28ce1be87747de8500f6ccd0f347e7752a610e74a4a3d`.
The line references in this section identify that revision.

| Original finding | Disposition in the revision |
|---|---|
| 1. Version advertisement prevents fallback | Corrected to offer 1.3 and 1.2. The separate signature-offer regression below still blocks a class of TLS 1.2 peers. |
| 2. Certificate-signature capability mismatch | Separate certificate list now matches the validator. Removing PKCS#1 from the other list introduces R2.1 below. |
| 3. CCS window | Corrected to run from the first ClientHello through the peer's Finished, including HRR. |
| 4. Cookie-only HRR | Explicit branch retains the original shares. |
| 5. supported_groups in EncryptedExtensions | Accepted with framing validation and no change to the selected group. |
| 6. Full inner-plaintext bound | Checked before padding removal; boundary cases added. |
| 7. Handshake record boundaries | Boundary information and checks are explicit; negative cases added. |
| 8. Engine-limit classification | Shared classifier changes and public-status checks are explicit. |

These dispositions concern the design. Implementation acceptance remains
the host and guest evidence specified for the slices.

### R2.1 [P1] Retain PKCS#1 in the dual-version signature offer

**Location:** [TLS13.md](TLS13.md), profile lines 167–176, especially the
new “no PKCS#1” rule at lines 170–173. **Slice:** S2.

The certificate-list split fixes the original finding 2, but the revision
also removes `rsa_pkcs1_sha256/384/512` from `signature_algorithms`.
That extension is consumed by the TLS 1.2 server before we can receive its
ServerHello and reconnect with Bear. A peer with an RSA certificate and
PKCS#1 handshake signatures now has no shared signature algorithm. Putting
PKCS#1 in `signature_algorithms_cert` does not restore its handshake offer.
The result is another alert before ServerHello, so the approved fallback
cannot fire.

TLS 1.2 uses the signature-algorithms offer for handshake signature
selection. TLS 1.3 forbids PKCS#1 CertificateVerify but explicitly permits
PKCS#1 values in the offer for TLS 1.2 compatibility. These are distinct
rules. [RFC 5246 §7.4.1.4.1](https://www.rfc-editor.org/rfc/rfc5246.html#section-7.4.1.4.1),
[RFC 8446 §4.2.3](https://www.rfc-editor.org/rfc/rfc8446.html#section-4.2.3)

The pinned Bear server has the same relevant limitation: its
[`ssl_hs_server.t0`](../../../userland/libtls/upstream/src/ssl/ssl_hs_server.t0)
filters out ECDHE_RSA suites when the offered signature/hash combinations
leave it no usable RSA signature. A scripted legacy peer that unconditionally
returns a chosen ServerHello would miss this negotiation failure.

**Measured evidence:** a local C harness using OpenSSL 3.0.13 and memory
BIOs, configured with:

```c
SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
SSL_CTX_set_cipher_list(ctx, "ECDHE-RSA-AES128-GCM-SHA256");
SSL_CTX_set1_sigalgs_list(ctx, "rsa_pkcs1_sha256");
```

The peer used a generated RSA-2048 certificate and key. Both input hellos
offered `{0x0304, 0x0303}`, the same suites, the same shares, and the revised
`signature_algorithms_cert` list. The control changed only
`signature_algorithms`, restoring the PKCS#1 SHA-256/384/512 trio.

| Input | First response |
|---|---|
| Revised offer without PKCS#1 | OpenSSL `no shared signature algorithms`; alert `15030300020228` |
| Control with PKCS#1 restored | ServerHello; `SSL_ERROR_WANT_READ` awaiting the client's next flight |

The alert payload `02 28` is fatal `handshake_failure` (40). This probe
establishes the first-response failure and its cause; it is not a complete
TLS 1.2 or TLS 1.3 authentication test.

**Correction:** restore the PKCS#1 trio to `signature_algorithms`, retain
the separate certificate list, and retain the explicit TLS 1.3
CertificateVerify rejection for PKCS#1. The dual-version ClientHello needs
to describe both versions' negotiation capabilities, just as the revised
version list does. No alert-triggered retry or certificate-policy change
is needed.

**Acceptance:** use a real RSA TLS 1.2 peer restricted to PKCS#1 signatures
and prove that its ServerHello reaches `PEER_CHOSE_TLS12`, followed by a
successful fresh Bear connection. Retain the TLS 1.3 PSS CertificateVerify
success case and a PKCS#1 CertificateVerify refusal case. Check the actual
ClientHello lists as well as the resulting statuses.

The review leaves the architecture, slice order, and Chris's six decisions
intact. No implementation work was started during this round.

## Round 3

Quinn, 2026-10-09. Re-read the revised design in full, checked the earlier
dispositions, and reviewed the profile and acceptance changes that address
R2.1. Reviewed draft SHA-256:
`c8bc9b1291ca19e8bceeb96d957b910c11dd61d46d508fd621f6119eb6c0939a`.

**Result: CLEAN. No actionable findings.** R2.1 is resolved:

- `signature_algorithms` again includes the PKCS#1 SHA-256/384/512 trio
  needed by the TLS 1.2 peer, alongside the TLS 1.3 signature schemes.
- `signature_algorithms_cert` retains the separate certificate-policy
  allowlist, excluding unsupported PSS-signed certificate chains.
- The TLS 1.3 CertificateVerify path explicitly refuses PKCS#1 despite
  its presence in the dual-version offer.
- Acceptance now includes a real PKCS#1-only RSA TLS 1.2 peer, a fresh
  Bear fallback connection, and the TLS 1.3 PKCS#1 refusal case.

Re-ran the round-2 OpenSSL memory-BIO probe after checking its input bytes
against the revised version and signature lists. The RSA TLS 1.2 peer
restricted to `rsa_pkcs1_sha256` returned ServerHello and
`SSL_ERROR_WANT_READ`, awaiting the client's next flight. This confirms
the first-response regression is removed; complete fallback and TLS 1.3
authentication remain implementation acceptance tests.

The design review is complete. S1 can proceed under the documented scope;
this result does not replace the vector, host-engine, audit, or guest
validation gates. No implementation was started during the review.
