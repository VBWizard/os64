# TLS13.md — a TLS 1.3 client engine beside BearSSL

Status: design and builder's brief, written 2026-10-09 by Fable at Chris's
request ("design it now ... GO!"), revised the same day for Quinn's design
review ([TLS13_REVIEW.md](TLS13_REVIEW.md): round 1, one P1 and seven P2;
round 2, one P1 against the round-1 fix of finding 2 — all applied; its
line numbers refer to the drafts before each revision). **Quinn builds it** (Chris, 2026-10-09;
Opus is on the yonder bug hunt). It pays the DEBTS.md row
**"No TLS 1.3: a 1.3-only server refuses us"**, filed after Chris met the
`protocol_version` alert clicking through real sites with yonder on
2026-10-05. Fable reviews every PR; whichever of Opus and Quinn did not build
a slice is the second reader on it; Chris schedules any outside round.

| Slice | Builder | Reviewer | PR |
|---|---|---|---|
| S1 key schedule + record layer, RFC 8448 vectors on the host | Quinn | Fable | branch from `userland`, PR against `userland` |
| S2 client handshake engine, host harness against OpenSSL, public ABI, libfetch fallback | Quinn | Fable, then Opus | stacked on S1 |
| S3 guest fixtures, QEMU acceptance, real sites, DEBTS and the as-built record | Quinn | Fable | stacked on S2, or folded into S2 if S2 is still open |

This file is the design and the acceptance. When a slice is built, its "as
built" section is appended at the foot of THIS file, in the shape DOM.md
uses, and the file moves to `docs/design/completed/` with the last slice.
TLS.md stays the TLS arc's root; its "TLS and identity policy" section gets
one sentence pointing here when S2 merges, because its "the initial profile
is a TLS 1.2 client" stops being the whole truth that day.

## Read first, in this order

1. [TLS.md](../completed/TLS.md) whole — the arc's rules (no http downgrade,
   no certificate bypass, explicit allowlists, the evidence plan in § Evidence
   plan). Everything here is subordinate to it.
2. [CLIENT_ENGINE.md](../../../userland/libtls/CLIENT_ENGINE.md) and
   `userland/libtls/port/client_engine.c` / `client_engine.h` — the private
   seam this engine must fit behind, and the contract (prefix copies, a
   count beside a terminal status, zero-length calls acknowledge nothing,
   sticky terminal errors, the 1 MiB pre-handshake input cap).
3. [TLS_PUBLIC_LIBRARY.md](../completed/TLS_PUBLIC_LIBRARY.md) and
   `userland/libtls/include/tls/tls.h` — the public byte API. It has no
   1.2 in it; that is why this slice touches no consumer's read/write code.
4. [TLS_CERTIFICATE_POLICY.md](../completed/TLS_CERTIFICATE_POLICY.md) and
   `port/certificate_policy.c` — the validator the Certificate message is
   streamed into. It is a `br_x509_class`; this engine drives it exactly as
   BearSSL's engine does (`start_chain`, `start_cert`/`append`/`end_cert`
   per certificate, `end_chain`, `get_pkey`).
5. [TLS_TRANSPORT.md](../completed/TLS_TRANSPORT.md) and
   `userland/libtls/transport.c`; then `userland/libfetch/fetch.c` around
   `os64_dial` and `fetch_transport_open` — where the version fallback lives.
6. RFC 8446 (TLS 1.3), RFC 8448 (its worked example traces, the vectors S1
   is proven against), RFC 7748 (x25519), RFC 8017 § 8.1 (RSASSA-PSS). The
   builder reads the RFC, not a summary of it; where this file and the RFC
   disagree, the RFC is right and this file gets fixed in the same PR.
7. CLAUDE.md's opening rules: the comment is part of the code, the truth
   pass before every submit, `tools/stale_refs.sh` before a rename.

## Why in-house, and why beside Bear

BearSSL implements TLS 1.2 and will not implement 1.3 (Pornin's tls13 page
has said "later version" since 2018). Chris's ruling, 2026-10-09: keep 1.3
in-house. The three roads compared that morning — replace Bear with mbedTLS,
write our own engine on Bear's primitives, or import picotls with a Bear
crypto backend — and the reasons the middle one won are in the commit
message of the PR that adds this file, where history belongs.

The shape that makes in-house cheap: **every cryptographic primitive 1.3
needs is already in the pinned Bear tree, constant-time and host-tested**:
SHA-256/384, HMAC, HKDF, AES-GCM (`br_gcm_*` over `br_aes_ct64_ctr_vtable`
and `br_ghash_ctmul64`), ChaCha20-Poly1305 (`br_poly1305_ctmul_run`, which
is the whole RFC 8439 AEAD in one call), x25519 and P-256/384/521
(`br_ec_all_m31`, `br_ec_keygen`, `br_ec_compute_pub`), ECDSA verify
(`br_ecdsa_i31_vrfy_asn1`), RSA-PSS verify (`br_rsa_i31_pss_vrfy`), HMAC-DRBG
(`br_hmac_drbg_*`), and the X.509 validator our policy already wraps. What
is NOT there is the protocol: the 1.3 key schedule, the 1.3 record layer
(Bear's is 1.2-shaped: explicit nonce, content type in the clear) and the
1.3 handshake. That is what we write — about 2,500 lines of plain C, no T0.
**We write no cryptography.** A line of arithmetic on a secret that is not
a call into Bear is a review finding.

## Architecture: the second engine wears Bear's buffer interface

```
os64_tls_*  (api.c, transport.c — unchanged contract)
      |
client_engine.c  — the private seam: hostname/ALPN copies, epoch, entropy,
      |            validator factory, the flags (handshake/closing/eof/
      |            aborted), sticky terminal, classify(), the 1 MiB cap
      |
      +-- protocol TLS12:  br_ssl_client_context  (Bear, as today)
      +-- protocol TLS13:  tls13_engine           (new, port/tls13_*.c)
```

**The 1.3 engine presents the SAME eight buffer verbs and the same state
bits as Bear's engine**, because `client_engine.c` already wraps that
contract and must not grow a second one ("share, don't mirror"):

| Bear | tls13 |
|---|---|
| `br_ssl_engine_recvrec_buf/ack` | `tls13_recvrec_buf/ack` |
| `br_ssl_engine_sendrec_buf/ack` | `tls13_sendrec_buf/ack` |
| `br_ssl_engine_sendapp_buf/ack` | `tls13_sendapp_buf/ack` |
| `br_ssl_engine_recvapp_buf/ack` | `tls13_recvapp_buf/ack` |
| `br_ssl_engine_current_state` → `BR_SSL_SENDREC/RECVREC/SENDAPP/RECVAPP/CLOSED` | `tls13_current_state`, same bits, same meanings |
| `br_ssl_engine_last_error` → `BR_ERR_*` | `tls13_last_error`, **Bear's numbering** (below) |
| `br_ssl_engine_flush(eng, 0)` | `tls13_flush` |
| `br_ssl_engine_close` | `tls13_close` |
| `br_ssl_engine_get_selected_protocol` | `tls13_selected_protocol` |

So `advance()` and `transfer()` in client_engine.c become a two-way dispatch
on `c->protocol` and nothing else about them changes. Whether the builder
writes that as a switch or a small ops table is the builder's call; the
rule is that every semantic the wrapper owns today (the drain-before-close
order, EOF → TRUNCATED, the cap, `fail()` stickiness) stays in
client_engine.c and is owned ONCE. The buffer-view discipline from TLS.md
(acquire one view, copy, ack the exact count, drop the view) is the same.

The engine struct is a union of the two contexts under the common header.
`BR_SSL_BUFSIZE_BIDI` is 16384+325+16384+85 bytes; the 1.3 context budgets
the same order (one 16709-byte receive record buffer, one 16469-byte send
record buffer, an 8 KiB handshake reassembly buffer, the schedule, the
multihash, the DRBG) — target under 64 KiB, measured in the as-built. The
whole connection stays on the heap, zeroed at birth and wiped at destroy
by the existing `wipe`.

**Error vocabulary is Bear's, extended, not duplicated.** `upstream_error`
reaches `os64_tls_error_description` in api.c, which already names every
alert and every `BR_ERR_*`. The 1.3 engine reports in that numbering:
received alerts as `BR_ERR_RECV_FATAL_ALERT + alert`, sent alerts as
`BR_ERR_SEND_FATAL_ALERT + alert`, and `BR_ERR_BAD_MAC`, `BR_ERR_BAD_SIGNATURE`,
`BR_ERR_BAD_FINISHED`, `BR_ERR_BAD_VERSION`, `BR_ERR_BAD_CIPHER_SUITE`,
`BR_ERR_BAD_HANDSHAKE`, `BR_ERR_UNEXPECTED`, `BR_ERR_TOO_LARGE`,
`BR_ERR_LIMIT_EXCEEDED`, `BR_ERR_EXTRA_EXTENSION`, `BR_ERR_BAD_CCS`,
`BR_ERR_BAD_ALERT`, `BR_ERR_NO_RANDOM`, the `BR_ERR_X509_*` range (what the
validator's `end_chain` returns, passed through) where those are exactly the
condition. Conditions 1.2 never had get codes in a private range that Bear
cannot collide with — `TLS13_ERR_BASE 768` (Bear uses 1..63, 256+alert,
512+alert): downgrade sentinel seen, second HelloRetryRequest, session id
not echoed, server picked a group we sent no share for, CertificateVerify
with a PKCS#1 scheme, and the rest as the builder meets them. Each gets a
line in `os64_tls_error_description`. **Two strings there say "this client
supports TLS 1.2" today and become false the moment S2 lands** (the
`protocol_version` alert text and the `BR_ERR_BAD_VERSION` text) — they
change in S2, not later.

## The profile (what we offer, and nothing else)

An explicit allowlist, as TLS.md requires. The ClientHello:

- `legacy_version` 0x0303; `random` 32 bytes from the DRBG;
  `legacy_session_id` **32 random bytes** (middlebox compatibility mode is
  ON — RFC 8446 Appendix D.4; it is why the final 1.3 looks like 1.2 on the
  wire, and the real-world reason it gets through); `legacy_compression_methods`
  = {0}.
- `cipher_suites`: `TLS_AES_128_GCM_SHA256` (0x1301), `TLS_AES_256_GCM_SHA384`
  (0x1302), `TLS_CHACHA20_POLY1305_SHA256` (0x1303), **followed by the six
  TLS 1.2 suites `client_profile.c` offers**. Not to negotiate them — this
  engine never speaks 1.2 — but for the one kind of server that reads
  nothing else: a LEGACY 1.2 implementation that does not understand
  `supported_versions` picks its version from `legacy_version` and its
  suite from this list, and with no 1.2 suite present it would answer
  `handshake_failure`, which we could not tell from a dozen other
  failures. With one present it answers a 1.2 ServerHello we recognise.
  (A modern server pinned to 1.2 never looks at this list for the
  decision; it is handled by `supported_versions`, next.) See § Version
  selection.
- Extensions, in this order: `server_name` (0) with the normalized
  hostname; `supported_groups` (10): x25519 (29), secp256r1 (23),
  secp384r1 (24) — **a table, not a switch**, because the next group to
  arrive is the post-quantum hybrid and it must be one row, not a design;
  `signature_algorithms` (13), the HANDSHAKE-signature list — and because
  this hello is read by 1.2 servers too, it must describe BOTH versions'
  capabilities, exactly as the version list does: `ecdsa_secp256r1_sha256`
  0x0403, `ecdsa_secp384r1_sha384` 0x0503, `ecdsa_secp521r1_sha512`
  0x0603, `rsa_pss_rsae_sha256/384/512` 0x0804/0x0805/0x0806, AND
  `rsa_pkcs1_sha256/384/512` 0x0401/0x0501/0x0601. The PKCS#1 trio is
  there for the 1.2 server, which picks how to sign its ServerKeyExchange
  from this list (RFC 5246 § 7.4.1.4.1), and a 1.2 server with an RSA
  certificate from before PSS existed has nothing else to pick: without
  the trio it answers `handshake_failure` before any ServerHello, and the
  fallback cannot fire (Quinn, round 2 R2.1, measured against OpenSSL
  pinned to 1.2 with `rsa_pkcs1_sha256` as its only signature algorithm:
  alert `02 28` without the trio, a ServerHello with it). RFC 8446
  § 4.2.3 permits the values in the offer for precisely this reason while
  forbidding them in 1.3 handshake signatures, so the two rules live
  apart: **a 1.3 CertificateVerify naming a PKCS#1 scheme is refused**
  with a `TLS13_ERR_*` code, whatever the hello offered;
  `signature_algorithms_cert` (50), the list for
  **certificate signatures**, which must say exactly what the retained
  validator verifies and nothing more: the ECDSA trio above plus
  `rsa_pkcs1_sha256/384/512` 0x0401/0x0501/0x0601 — NOT PSS, because
  Bear's `x509_minimal` and `certificate_der.c`'s `signature_algorithm()`
  accept PKCS#1 and ECDSA certificate signatures only, and without this
  extension the first list would also advertise certificate support
  (§ 4.2.3), inviting a server with a PSS-signed chain to send one we then
  refuse (Quinn, design review finding 2; PSS-signed chains are a profile
  limitation until the validator learns them); `application_layer_protocol_negotiation`
  (16) when the caller gave names; `supported_versions` (43): **0x0304
  then 0x0303, in that preference order** — see § Version selection for
  why both, and for the rule that this engine still never continues a 1.2
  handshake; `key_share` (51): shares for x25519 AND secp256r1, both
  generated with `br_ec_keygen` + `br_ec_compute_pub` from the
  connection's DRBG. Two shares cost ~100 bytes and make a key_share
  HelloRetryRequest unlikely from any real server, since every 1.3 server
  supports x25519; a cookie-only HRR remains possible and is handled.
- Not offered, and therefore refused if the server sends them anyway:
  `pre_shared_key`, `psk_key_exchange_modes`, `early_data`,
  `post_handshake_auth`, `status_request`, `signed_certificate_timestamp`,
  `compress_certificate`, `record_size_limit`, `max_fragment_length`,
  `encrypted_client_hello`, `padding`. A server extension we did not offer
  is `BR_ERR_EXTRA_EXTENSION` → `unsupported_extension` alert.

Signature schemes bind curve to hash in 1.3: `ecdsa_secp256r1_sha256` is
valid only for a P-256 key with SHA-256, and so on. The verifier checks the
key's curve against the scheme before calling Bear. RSA-PSS verification is
`br_rsa_i31_pss_vrfy(sig, len, hf, hf, hash, hf->desc >> BR_HASHDESC_OUT_OFF
& BR_HASHDESC_OUT_MASK, pk)` — same hash for data and MGF1, salt length =
hash length, as `rsae` schemes require. The key comes from the validator's
`get_pkey(ctx, &usages)` and must carry `BR_KEYTYPE_SIGN`; a key without it
is `BR_ERR_X509_FORBIDDEN_KEY_USAGE`.

Deliberately absent, booked as one DEBTS row ("TLS 1.3 engine boundaries",
in the shape of the existing "SSH implementation boundaries" section):
session resumption and PSK (tickets are consumed and discarded), 0-RTT,
client certificates (a `CertificateRequest` is answered with an EMPTY
`Certificate` message — the conformant reply of a client with nothing to
show; if the server then alerts, that is the failure, and TLS.md's "no retry
that weakens policy" holds), post-handshake authentication, record padding
on send (we accept it on receive — stripping trailing zeros is the content
type search), OCSP stapling, SCTs, certificate compression, ECH, the
post-quantum groups (a table row waiting for ML-KEM, which would be the
first crypto we write ourselves — its own design when it comes).

## Version selection and the 1.2 fallback

Bear generates its own ClientHello, so one hello cannot serve both engines:
a 1.2 answer to OUR hello cannot be handed to Bear mid-flight. The design:

1. A connection is created with `protocol = OS64_TLS_PROTOCOL_DEFAULT`
   (= 0, meaning TLS 1.3) or `OS64_TLS_PROTOCOL_TLS12` (Bear, exactly as
   today). The field is new in `os64_tls_config_t`; zero keeps every
   existing caller's behaviour minus one thing: they now try 1.3 first.
2. **The hello offers `supported_versions` = {0x0304, 0x0303}.** The first
   draft offered 0x0304 alone, reasoning that this engine cannot speak
   1.2; Quinn's design review (finding 1, reproduced on this host with
   OpenSSL 3.0.13) showed what that costs: a MODERN server pinned to 1.2
   obeys RFC 8446 § 4.2.1 — when the extension is present it is the ONLY
   input to version selection — finds no version it supports, and answers
   a fatal `protocol_version` alert. No ServerHello, so no fallback, so a
   regression against every such server Bear reaches today. And an alert
   must never trigger a retry (anyone on the path can inject one). So the
   hello speaks for the CLIENT, which does speak both versions, not for
   the engine that happens to be parsing the answer. The measured answers
   (2026-10-09, Python `ssl` over MemoryBIO, same hello but for the
   extension):

   | Server | Hello offers | Answer |
   |---|---|---|
   | pinned to 1.2 | 0x0304 only | alert `protocol_version` |
   | pinned to 1.2 | 0x0304, 0x0303 | 1.2 ServerHello, random tail random |
   | pinned to 1.2 | extension absent (legacy client) | 1.2 ServerHello, random tail random |
   | 1.3 capable | 0x0304, 0x0303 | 1.3 ServerHello |
   | 1.3 capable | 0x0303 only (a stripped hello) | 1.2 ServerHello, random tail `DOWNGRD\x01` |

3. The 1.3 engine reads the ServerHello. If `supported_versions` names
   0x0304 — proceed. If the extension is ABSENT (the server negotiated
   1.2 the old way, whether it is modern-and-pinned or legacy), then:
   - if the last eight bytes of `ServerHello.random` are the downgrade
     sentinel `44 4F 57 4E 47 52 44 01` ("DOWNGRD\x01", RFC 8446 § 4.1.3):
     the server is 1.3-capable and chose 1.2 anyway, which a server that
     saw our 0x0304 never does — something between us rewrote the hello —
     fatal, `illegal_parameter`, `TLS13_ERR_DOWNGRADE`, terminal
     `PROTOCOL`, no fallback ever;
   - otherwise terminal status **`OS64_TLS_PEER_CHOSE_TLS12`** (a new
     status appended after `CANCELLED`; `os64_tls_status_name` and
     `os64_tls_error_description` learn it). The engine sends no alert and
     does NOT continue the 1.2 handshake it was offered; the caller is
     expected to close the TCP connection and start over with Bear.
   The `DOWNGRD\x00` sentinel (1.1 or below chosen) is likewise fatal, and
   so is a ServerHello whose `supported_versions` names anything but 0x0304.
4. **The fallback is libfetch's, because libfetch owns the dial.** In
   `fetch.c`, when `fetch_transport_open` fails and the TLS snapshot's
   status is `PEER_CHOSE_TLS12` and the config was DEFAULT: redial the same
   peer ONCE with `protocol = TLS12`, a fresh TCP connection and (by
   construction — new client, new entropy) fresh randomness, and record
   `detail.tls_fallback = true` so yonder's diagnostics can say "TLS 1.2
   (fallback)". No other status triggers a redial — not CERTIFICATE, not an
   alert, not a timeout; a fallback that fired on anything else would be
   the downgrade attack 1.3 was designed to end. The transport adapter
   itself does not change: it has no dial to retry with.
5. `os64_tls_state_t` gains `uint16_t version` (0 until the ServerHello,
   then 0x0303 or 0x0304) so a consumer can show what it got. os64get's
   verbose output and yonder's diagnostics print it; nothing else is asked
   of the consumers in this arc.

The residual risk, stated plainly: an on-path attacker who can forge a
plaintext 1.2 ServerHello can make us reconnect with 1.2. The reconnected
handshake is still a fully authenticated ECDHE+AEAD TLS 1.2 handshake with
the same certificate policy, so what is lost is 1.3's properties, not the
connection's authenticity. This is the trade every browser made during the
1.3 rollout and dropped only when 1.2-only servers became rare enough not
to need it. When ours become rare enough, the fallback goes; the knob to
turn it off is `protocol`, and whether yonder's Settings should expose it
is Chris's call for a later slice.

## The record layer (`port/tls13_record.c`)

- Outer header is 5 bytes: type, `legacy_record_version` (we SEND 0x0303
  always, and 0x0301 is permitted for the first ClientHello; we IGNORE the
  field on receive, as the RFC says), length. A received record whose length
  exceeds 2^14 + 256 is `record_overflow`; a plaintext record over 2^14 too.
- Until the ServerHello (and after an HRR, until the second one) the
  server's records are plaintext: handshake (22) and alert (21).
  `change_cipher_spec` (20) with body `01` is accepted and IGNORED at any
  point from our first ClientHello until the server's Finished
  (compatibility mode; RFC 8446 § 5 forbids it only before the first
  ClientHello and after the peer's Finished). That window INCLUDES the
  gap between a HelloRetryRequest and the second ServerHello, where a
  compatibility-mode server sends its dummy CCS (Quinn, finding 3; the
  first draft closed the window at "the ServerHello" and would have failed
  every HRR exchange). Any other body, a CCS after the server's Finished,
  or an ENCRYPTED CCS, is `BR_ERR_BAD_CCS` → `unexpected_message`.
- **The complete inner plaintext is bounded BEFORE padding is stripped**:
  `TLSInnerPlaintext` — content, type byte and padding together — may not
  exceed 2^14 + 1 bytes (RFC 8446 § 5.4), and a record that decrypts to
  more is `record_overflow` with nothing released, even though its
  ciphertext fitted the 2^14 + 256 outer bound (Quinn, finding 6: the
  outer allowance alone lets padding smuggle 255 bytes past the inner
  limit). Three checks, in order: outer length, inner length after
  decryption, then strip.
- **The record layer hands the handshake parser record boundaries**, not a
  byte stream: each delivered fragment carries "this is where a record
  began/ended" and the key epoch it was read under, because two handshake
  rules are about records, not messages (§ Handshake, framing).
- Encrypted records: outer type is always `application_data` (23). AAD is
  the 5-byte outer header. Nonce is the 12-byte static IV XOR the 64-bit
  record sequence number left-padded to 12 bytes; the sequence number is
  per direction, starts at 0 and RESETS to 0 at every key change. After
  decryption the inner plaintext is `content || type || zero padding`: strip
  trailing zeros, the last nonzero byte is the type, and an inner plaintext
  that is ALL zeros is `unexpected_message`. Inner types we accept:
  handshake, alert, application_data (only after the handshake). A type
  0x14 inside an encrypted record is `BR_ERR_BAD_CCS`.
- AES-GCM via `br_gcm_init(ctx, &aes_ctr.vtable, br_ghash_ctmul64)`, then
  per record `br_gcm_reset(iv,12)`, `br_gcm_aad_inject(header,5)`,
  `br_gcm_flip`, `br_gcm_run(encrypt, data, len)`, `br_gcm_get_tag` /
  `br_gcm_check_tag`. ChaCha20-Poly1305 via one `br_poly1305_ctmul_run(key,
  nonce, data, len, aad, 5, tag, br_chacha20_ct_run, encrypt)`, as Bear's
  own `ssl_rec_chapol.c` does; copy its composition, not its nonce rule.
  A failed tag is `BR_ERR_BAD_MAC` → `bad_record_mac`, fatal.
- We SEND at most 2^14 bytes of content per record and no padding; a
  record the engine builds never exceeds its send buffer by construction
  (static-assert the buffer against 5 + 2^14 + 1 + 16).
- Alerts: `close_notify` → the peer's write side is closed (see § Close);
  `user_canceled` at level warning is ignored; EVERY other alert is fatal
  in 1.3 regardless of its level byte (RFC 8446 § 6), recorded as
  `BR_ERR_RECV_FATAL_ALERT + description`.

## The key schedule (`port/tls13_schedule.c`)

All of it is HMAC with the suite's hash, and **every output here is at most
one hash length long, so HKDF-Expand never needs a second block**: write
`HKDF-Extract(salt, ikm) = HMAC(salt, ikm)` and `HKDF-Expand-Label(secret,
label, context, L) = HMAC(secret, HkdfLabel || 0x01)` truncated to L, with a
`_Static_assert`/runtime check that L ≤ hash length. (`br_hkdf_*` is there
if the builder prefers it for Extract; the one-block fact still holds.)

```
HkdfLabel = uint16 L || opaque<7..255> ("tls13 " + label) || opaque<0..255> context
Derive-Secret(S, label, msgs) = Expand-Label(S, label, Transcript-Hash(msgs), Hash.len)

early_secret      = Extract(0^Hash.len, 0^Hash.len)
derived           = Derive-Secret(early_secret, "derived", "")
handshake_secret  = Extract(derived, ECDHE shared secret)
c_hs_traffic      = Derive-Secret(handshake_secret, "c hs traffic", CH..SH)
s_hs_traffic      = Derive-Secret(handshake_secret, "s hs traffic", CH..SH)
derived2          = Derive-Secret(handshake_secret, "derived", "")
master_secret     = Extract(derived2, 0^Hash.len)
c_ap_traffic      = Derive-Secret(master_secret, "c ap traffic", CH..server Finished)
s_ap_traffic      = Derive-Secret(master_secret, "s ap traffic", CH..server Finished)
key = Expand-Label(traffic_secret, "key", "", key_len)   iv = Expand-Label(.., "iv", "", 12)
finished_key = Expand-Label(base_key, "finished", "", Hash.len)
verify_data  = HMAC(finished_key, Transcript-Hash(up to the message before Finished))
next_traffic_secret = Expand-Label(traffic_secret, "traffic upd", "", Hash.len)   (KeyUpdate)
```

`exporter_master_secret` and `resumption_master_secret` are not derived: no
consumer asks for exporters and we keep no tickets. The ECDHE shared secret
is the x25519 output (32 bytes; an all-zero result is fatal, RFC 7748 § 6.1)
or the x-coordinate of the P-256 product (bytes 1..32 of Bear's uncompressed
point). Bear's `br_ec_c25519_m31.mul` clamps the scalar itself and takes it
big-endian; `br_ec_keygen` produces it in that form.

**The transcript hash runs both SHA-256 and SHA-384 from the first byte**
with `br_multihash` (set both impls, `br_multihash_init`, `update` with
every handshake MESSAGE — header included, record headers excluded,
CCS excluded), because the suite, and so the hash, is unknown until the
ServerHello is parsed; `br_multihash_out(ctx, id, dst)` yields the running
digest without finalizing, which is exactly what Derive-Secret needs at
four different points. **HelloRetryRequest replaces the transcript**: on
HRR the transcript becomes the synthetic
`message_hash(254) || 00 00 Hash.len || Hash(ClientHello1)` followed by the
HRR itself (RFC 8446 § 4.4.1), and the hash is now known — the builder may
drop the other at that point.

Secrets live as long as they are needed and not a record longer:
handshake traffic secrets and `finished_key`s are wiped once both Finished
are done; the application traffic secrets stay (KeyUpdate derives from
them) and are replaced in place on update; the ECDHE private keys and the
shared secret are wiped as soon as `handshake_secret` exists. Destroy wipes
the whole context; the host harness's allocator already asserts every freed
byte is zero.

## The handshake (`port/tls13_handshake.c`)

The client's state machine, in order. Every message is framed
`type(1) || length(3)`; messages may span records and a record may hold
several messages. Four framing rules, all `unexpected_message`, and the
parser needs the record boundaries from the record layer to check the
last two (RFC 8446 § 5.1; Quinn, finding 7):

- an encrypted record holds one content type — handshake bytes do not
  share a record with application data or an alert;
- a handshake message split across records may not have ANY other record
  type between its fragments: a partial message buffered + a
  non-handshake record arriving = fatal;
- a message that changes keys — ServerHello, Finished, KeyUpdate — must be
  the LAST thing in its record: trailing bytes after it in the same record
  are fatal, and are checked BEFORE the new keys are installed, so a
  `KeyUpdate` followed by another message under the old key is never
  processed as a coalesced flight;
- a handshake message may not span a key change.

1. **ClientHello** built into the send record buffer at `tls13_init`;
   `SENDREC` is set, nothing else is. The hello bytes are kept until the
   ServerHello is parsed (the transcript needs them; HRR needs them again).
2. **ServerHello** (plaintext). Checks, each with its error: legacy_version
   0x0303 (`BR_ERR_BAD_VERSION`); random — HRR magic
   `CF 21 AD 74 E5 9A 61 11 BE 1D 8C 02 1E 65 B8 91 C2 A2 11 16 7A BB 8C 5E
   07 9E 09 E2 C8 A8 33 9C` means this is a HelloRetryRequest (step 2a);
   downgrade sentinels (§ Version selection); session id echoed exactly
   (`TLS13_ERR_SESSION_ID`); cipher suite is one of our three
   (`BR_ERR_BAD_CIPHER_SUITE`); compression 0; extensions: exactly
   `supported_versions` = 0x0304 and `key_share` with a group we sent a
   share for (`TLS13_ERR_KEY_SHARE`), nothing else (`BR_ERR_EXTRA_EXTENSION`).
   Compute the shared secret, derive the handshake secrets, install the
   server handshake read key. From here every record from the server is
   encrypted.
   - 2a. **HelloRetryRequest**: accepted ONCE (`TLS13_ERR_SECOND_HRR` on a
     second). `supported_versions` must be 0x0304; the suite must be one
     of ours. Then two OPTIONAL extensions, and at least one must be
     present and must change something, or it is `illegal_parameter`
     (RFC 8446 § 4.1.4; Quinn, finding 4 — the first draft demanded a
     group and could not handle a cookie-only retry):
     - `key_share`, if present, names ONE group from our
       `supported_groups` that we did not send a share for (we sent
       x25519 and P-256, so in practice secp384r1; naming a group we
       already offered a share for, or one we never listed, is
       `illegal_parameter`). The second hello replaces BOTH shares with a
       single fresh one for that group.
     - `cookie`, if present, is echoed verbatim in the second hello,
       which keeps its ORIGINAL shares when `key_share` was absent.
     Everything else in the second ClientHello is byte-identical to the
     first — same random, same session id (RFC 8446 § 4.1.2 lists what
     may change; nothing else may). The second ServerHello must not be
     another HRR, must select the same suite, and must select the group
     the HRR named or, for a cookie-only retry, one we sent a share for.
3. **EncryptedExtensions** (first encrypted message): for what we offered,
   three extensions are legal here — `server_name` (empty ack),
   `application_layer_protocol_negotiation`, and `supported_groups`, which
   a server MAY return as its preference list (RFC 8446 § 4.2.7; Quinn,
   finding 5): its framing is validated, its contents — which may name
   groups we do not have — are ignored, and nothing about the already
   selected group changes. The selected ALPN name must be one we sent (Bear
   has no code for this condition — it is `TLS13_ERR_ALPN`, sent as the
   `no_application_protocol` alert), copied into engine-owned storage for
   `tls13_selected_protocol`. Absent ALPN when we offered is allowed, as
   today (`BR_OPT_FAIL_ON_ALPN_MISMATCH` fails only an UNOFFERED selection).
4. **CertificateRequest** (optional): parsed for framing (context, the
   `signature_algorithms` extension must be present), remembered, answered
   in step 8 with an empty Certificate.
5. **Certificate**: `certificate_request_context` must be empty (server
   auth). Then a `CertificateEntry` list, each `cert_data<1..2^24-1>` then
   `extensions<0..2^16-1>`. **Stream it**: `start_chain(hostname)` once,
   then per entry `start_cert(len)`, `append` as bytes arrive across
   records, `end_cert`. The policy validator already bounds count and size
   (`TLS_CHAIN_MAX`, `TLS_CERTIFICATE_MAX`), so the engine needs no
   certificate buffer of its own — only the transcript hash sees the whole
   message, and it hashes as it streams. Per-entry extensions must be
   EMPTY: we offered neither `status_request` nor SCT, so anything there is
   `BR_ERR_EXTRA_EXTENSION`. At the end, `end_chain()`; a nonzero return is
   the terminal error (classify() already maps the X.509 range to
   CERTIFICATE and honours the policy's LIMIT), and the handshake does not
   proceed to a signature check on a chain that failed.
6. **CertificateVerify**: `SignatureScheme(2) || signature<0..2^16-1>`. The
   scheme must be in our offered list, must not be PKCS#1, and must match
   the EE key (`get_pkey` with `BR_KEYTYPE_SIGN` in usages). The signed
   content is `0x20 × 64 || "TLS 1.3, server CertificateVerify" || 0x00 ||
   Transcript-Hash(CH..Certificate)`; hash it with the SCHEME's hash (not
   the suite's), then `br_ecdsa_i31_vrfy_asn1` or `br_rsa_i31_pss_vrfy`.
   Failure is `BR_ERR_BAD_SIGNATURE` → `decrypt_error`.
7. **Finished** (server): `verify_data` compared **constant-time** (a
   ten-line OR-of-XORs; Bear exports no public memcmp for this) against
   HMAC(server `finished_key`, Transcript-Hash(CH..CertificateVerify)).
   Mismatch is `BR_ERR_BAD_FINISHED` → `decrypt_error`. Then derive the
   application traffic secrets over CH..server Finished and install the
   server APPLICATION read key: the next record from the server is already
   under it (NewSessionTicket arrives there).
8. **Client flight**: the dummy CCS record (compatibility mode), then —
   under the client HANDSHAKE write key — an empty `Certificate` if step 4
   happened, then client `Finished` = HMAC(client `finished_key`,
   Transcript-Hash(CH..server Finished [..client Certificate])). Then
   install the client APPLICATION write key. `handshake` is now complete:
   `SENDAPP` and `RECVAPP` may be set, exactly as Bear's engine sets them.
9. **Post-handshake messages**, any time after step 8, always encrypted:
   `NewSessionTicket` — framed and DISCARDED (stream-discarded, since a
   ticket may be large; nothing is stored); `KeyUpdate(update_requested |
   update_not_requested)` — derive the server's next read secret and reset
   its sequence number; if `update_requested`, we MUST send our own
   `KeyUpdate(update_not_requested)` under the CURRENT write key and then
   switch our write key, before any further application data; the engine
   also sends an unrequested KeyUpdate of its own when its send sequence
   number reaches 2^24 (a limit that no os64 transfer will reach, but the
   path exists and is tested through the requested case). Any other
   handshake type here — including `CertificateRequest`, since we did not
   offer `post_handshake_auth` — is `unexpected_message`.

Message size caps: the reassembly buffer holds one non-streamed message
of at most 8 KiB (ServerHello, HRR, EncryptedExtensions,
CertificateRequest, CertificateVerify, Finished, KeyUpdate all fit with
room; a 4096-bit RSA signature is 512 bytes). A declared length above the
cap is `BR_ERR_TOO_LARGE` → terminal `LIMIT` — **and that mapping does not
exist in the wrapper today**: `classify()` in client_engine.c sends every
Bear error outside the X.509 range and `NO_RANDOM` to `PROTOCOL`, and its
only `LIMIT` branch is the validator's policy limit (Quinn, finding 8).
The wrapper delta therefore adds one rule, owned once and applied to BOTH
engines: `BR_ERR_TOO_LARGE` and `BR_ERR_LIMIT_EXCEEDED` classify as
`LIMIT`, because "a resource bound was hit" is what the public status
means and was always the truer name for Bear's cases too. The 1.2 host
harness is checked for any case that asserted `PROTOCOL` on those two
codes; if one exists its expectation changes in the same PR, and the
change is named in the PR body. Certificate and
NewSessionTicket are the two streamed messages and have no engine cap of
their own (the validator bounds the one, the other is discarded as it
arrives). The wrapper's 1 MiB pre-handshake ciphertext cap stays and
applies unchanged.

## Close, EOF, abort — the wrapper's semantics, kept

`tls13_close` sends `close_notify` under the current write key and marks
the write side closed; `CLOSED` is reported once the peer's `close_notify`
has ALSO been received (or, if the peer's arrived first, once our reply has
been produced), which is what `advance()` turns into `CLEAN_EOF`. After a
peer `close_notify`, further application records are discarded, as Bear
does. Transport EOF with no `close_notify` stays the wrapper's
`TRUNCATED`. Abort is the wrapper's. A fatal alert is PRODUCED into the
send buffer (best effort, as today) and the engine goes terminal; nothing
after a fatal alert is decrypted.

## Public ABI delta (S2)

In `tls.h`: `OS64_TLS_PEER_CHOSE_TLS12` appended to `os64_tls_status_t`;
`typedef enum { OS64_TLS_PROTOCOL_DEFAULT, OS64_TLS_PROTOCOL_TLS12 }
os64_tls_protocol_t;` and a `protocol` field at the END of
`os64_tls_config_t`; `uint16_t version` in `os64_tls_state_t`. No new
exported function, so `exports.map` and `audit_tls.py`'s import set do not
change; the audit's manifest check WILL fail until `public_sources.mk`
lists the new port files and the newly reached upstream members
(`aead/gcm.c`, `ec/ec_keygen.c`, `ec/ec_pubkey.c`, `rsa/rsa_i31_pss_vrfy.c`,
`rsa/rsa_pss_sig_unpad.c`, plus whatever the link audit finds — the audit
is the oracle, not this list). In `client_engine.c`: the protocol
dispatch, and the one-line `classify()` change above. libfetch: `fetch_detail` gains
`tls_fallback` and `tls_version`; the fallback redial in `fetch.c`.
`CLIENT_ENGINE.md` and `TLS_PUBLIC_LIBRARY.md` get the delta in the same PR.

## Proof

**S1 — RFC 8448 on the host, no network.** `tools/tls13_vectors.py`
embeds the vectors from RFC 8448 § 3 (simple 1-RTT) and § 5
(HelloRetryRequest) into `userland/libtls/test/tls13_vectors.h` with
attribution, the way `bearssl_vectors.py` does for Bear's; nothing is
fetched at build time. `tools/test_tls13_vectors_host.{py,c}` builds
`tls13_schedule.c` + `tls13_record.c` with plain `cc` under ASan/UBSan
against the foundation archive and checks, byte for byte: every secret and
every key/IV the RFC prints, given the RFC's ClientHello/ServerHello bytes
and x25519 private key; the decryption of each of the RFC's server records
to the handshake message the RFC prints; the client Finished
`verify_data`; the HRR synthetic transcript; and a KeyUpdate derivation.
Then the record layer alone: a round trip under each AEAD, a flipped bit
in body/tag/header each refused, padding stripped, an all-zero inner
plaintext refused, the sequence-number nonce (record 0, 1, 2^32, 2^64−1).

**S2 — the engine against OpenSSL on the host.**
`tools/test_tls13_engine_host.{py,c}` builds `client_engine.c` + the three
`tls13_*.c` + the policy validator against the archive, and drives real
sockets to a peer the Python side runs: Python `ssl` (OpenSSL 3.0.13 on
this host, 1.3 capable) for most cases, `openssl s_server` as a subprocess
where Python lacks the lever (`-groups P-384` for a deterministic HRR,
its `K` stdin command for a requested KeyUpdate, `-num_tickets 0` and the
default 2 for the ticket paths). Fixture certificates are the policy
harness's generated CAs (`tools/generate_tls_trust_fixtures.py`), an
ECDSA P-256 leaf and an RSA-2048 leaf (PSS-signed CertificateVerify, which
is what every RSA server does in 1.3). Entropy is the fixture callback;
time is explicit. Cases, each a function, each named after the sentence it
proves:

- the ClientHello's bytes: version, random, 32-byte session id, suite
  list including the six 1.2 codes, every extension and its order, two
  key shares, `supported_versions` = {0x0304, 0x0303}, the two signature
  lists each saying exactly what § The profile says, and nothing more;
- complete handshakes for all three suites × both leaf types × x25519 and
  P-256 negotiated (Python `set_ecdh_curve`), 70,013 bytes each way with
  ciphertext fragments from 1 to 4096 bytes and one-byte plaintext reads,
  as CLIENT_ENGINE.md's fixtures do for 1.2; an RSA leaf whose
  CERTIFICATE signature is PKCS#1 and whose CertificateVerify is PSS (the
  common web case) passes; a PSS-SIGNED chain is refused by the validator
  with a certificate status, not a crash, proving the two lists differ
  for a reason; a 1.3 CertificateVerify carrying a PKCS#1 scheme (scripted
  peer) is refused even though the hello offered that scheme;
- HRR to P-384 (`-groups P-384`), including the server's dummy CCS
  arriving between HRR and the second ServerHello; a cookie-only HRR
  (scripted peer) answered with the original shares plus the cookie; an
  HRR that changes nothing refused; a server that HRRs twice refused;
- `PEER_CHOSE_TLS12` from a MODERN server pinned to 1.2 (Python
  `maximum_version`), from an RSA 1.2 server restricted to PKCS#1
  signatures (`openssl s_server -tls1_2 -sigalgs rsa_pkcs1_sha256` with
  the RSA fixture — a REAL peer, because a scripted one that returns a
  canned ServerHello cannot fail the signature negotiation this case is
  about), each followed by a successful fresh Bear connection to the same
  peer; from a LEGACY peer (scripted: ignores `supported_versions`,
  answers a 1.2 ServerHello choosing one of the six 1.2 suites); and
  `PROTOCOL` from a 1.3-capable Python server reached
  through a stripping proxy (`tools/tlsproxy.py`'s shape) that rewrites
  our `supported_versions` to {0x0303} — the server then writes
  `DOWNGRD\x01` itself, as measured above, and the engine must refuse;
- EncryptedExtensions carrying `supported_groups` with groups we lack
  completes; a malformed list is refused;
- a valid-tag record whose inner plaintext is 2^14 + 2 bytes (one byte of
  content, the rest padding) is refused as `record_overflow` with no
  plaintext released; 2^14 + 1 passes;
- an application-data record between two fragments of a NewSessionTicket
  refused; trailing bytes after KeyUpdate, after ServerHello and after
  Finished in the same record refused, keys uninstalled; ordinary
  fragmentation and coalescing still pass;
- wrong hostname, untrusted chain, expired leaf — each `CERTIFICATE` with
  the policy reason the 1.2 harness expects, proving the SAME validator is
  doing the SAME job behind the new engine;
- CertificateVerify tampered (flip a signature byte through a raw-socket
  replay) → `BAD_SIGNATURE`; a replayed flight with the server Finished
  altered → `BAD_FINISHED`; a flipped ciphertext byte → `BAD_MAC`;
- CCS tolerated between ServerHello and Finished; a CCS after Finished
  refused; an encrypted CCS refused; `-no_middlebox` server works too;
- NewSessionTicket ×2 after the handshake consumed and discarded while
  application data flows; KeyUpdate requested and unrequested, data
  correct across the switch in both directions;
- `close_notify` in both orders, EOF mid-handshake (`TRUNCATED` or
  `PROTOCOL` as the wrapper rules say), EOF with buffered plaintext, abort
  while output is pending; a `CertificateRequest` answered with the empty
  Certificate and the server's subsequent alert surfaced;
- every allocation failed in turn at create; every freed byte zero; two
  interleaved connections independent when one aborts; the 8 KiB message
  cap and the 1 MiB pre-handshake cap each hit and refused as `LIMIT`
  THROUGH THE PUBLIC BYTE INTERFACE, with the upstream detail retained,
  the accepted prefix counted and the status sticky — the classify()
  change is what makes the first of those true.

**The fuzz pass TLS.md still owes, time-boxed.** The handshake harness
can REPLAY a captured server flight against a deterministic client:
fixture entropy fixes our ephemeral key and the server's public share is in
the flight, so the handshake keys reproduce and the ciphertext decrypts.
`tools/fuzz_tls13_host.py` captures N flights (one per suite × leaf × HRR
or not), then for a budget the builder records (start at 30 minutes of
wall clock) mutates them two ways and feeds each mutant to the engine
under ASan/UBSan: raw ciphertext bytes (reaches the record layer and the
AEAD refusal), and handshake-message bytes through a test-only hook that
bypasses decryption (reaches the parsers — the deep states TLS.md § 3
asks for). The invariant checked on every mutant is TLS.md's: no plaintext
is ever readable without a verified Finished, no accepted byte is lost or
duplicated, every path ends in a named terminal status or a live engine,
nothing the sanitizers can see. Iterations, coverage of the handshake
switch, and any crash (kept as a regression with its seed) go in the
as-built, in numbers.

**S3 — the guest.** `/tests/tlstransportprobe` learns `13` as a protocol
argument and `tools/test_tls_transport_peer.py` gains 1.3 modes, and the
QEMU acceptance procedure in VERIFICATION.md § TLS transport acceptance is
run again for 1.3 (slirp to the host peer), including the fallback case
with the peer pinned to 1.2. Then `os64get -v` and yonder against the
1.3-only sites from the web census that refused us on 10/5 — the fetched
bytes compared against `curl`'s. The audit (`python3 tools/audit_tls.py`),
`make fsck-ext2`, the full suite and every host harness green. The P5 run
is Chris's, as always; the token for nothing travels there and nothing
here needs one.

## Comments, naming, debts — the house rules this slice is likeliest to trip

- Names mean things: `tls13_` prefix for the engine, `TLS13_ERR_` for its
  private codes, labels spelled exactly as RFC 8446 § 7.1 spells them.
- A comment says WHY and HOW now. The RFC section number is the right
  citation for a rule ("RFC 8446 § 4.2.1: ignore legacy_version"); the
  history of why there are two engines is in this file and the commit, not
  in the code.
- No inventories in code: "the two streamed messages" is fine here, not in
  a comment that outlives the next streamed message.
- `tools/stale_refs.sh` before any commit that renames; the truth pass
  over `error_description`'s strings, `CLIENT_ENGINE.md`'s "selects TLS
  1.2", TLS.md § TLS and identity policy, and the DEBTS row this pays
  (struck in S3's PR, with a "TLS 1.3 engine boundaries" row replacing it).
- A deferral discovered mid-build is discussed with Chris before the code
  is written, not found in review.

## Decided here, ratified by Chris 2026-10-09

Walked through one by one that morning; all six agreed (the sixth "with
your gut"). They are settled, not open for re-litigation in review.
Quinn's design review the same day ([TLS13_REVIEW.md](TLS13_REVIEW.md),
eight findings, all applied above) changed the MECHANISM of the first and
the PURPOSE of the third without touching the rulings: the hello now
offers both versions, because a 1.3-only offer draws an alert from a
modern 1.2 server instead of the ServerHello the fallback keys on; and the
1.2 suite codes serve the legacy server that reads no extension, not the
modern one.

1. Try 1.3 first, reconnect once with Bear on a sentinel-free 1.2 answer
   (the hello offers 1.3 and 1.2 so that answer can arrive).
2. Compatibility mode on: random 32-byte session id, dummy CCS sent, CCS
   tolerated.
3. The six 1.2 suite codes ride in the 1.3 ClientHello so a LEGACY 1.2
   server's choice is recognisable, never negotiable, in this engine.
4. HelloRetryRequest is supported (once), because a client that cannot
   handle it is not a conformant client and OpenSSL's `-groups` makes it
   trivially testable; two key shares make it rare.
5. Named groups are a table with x25519, P-256 and P-384 today; the
   post-quantum hybrid is a row for a later arc, not a hook designed now.
6. No resumption, no 0-RTT, no client certificates, no exporters, no
   send-side padding — the DEBTS row names them.

## S1, as built

Built by Quinn on `codex/tls13-s1`, from `userland` at `32ae9d8e`.
The reviewed design and its review record ship with this slice. S1 adds
the private key-schedule and record helpers to the foundation archive;
the public library still uses the existing TLS 1.2 engine. S2 owns the
handshake, public dispatch, alert interpretation, and version fallback.

**What the design left to the builder, and what was decided:**

- `port/tls13_internal.h` is the private seam. Immutable suite/group
  descriptors restrict the helpers to the three suites and three named
  groups. Hash state uses Bear's multihash; HRR replaces CH1 with
  `message_hash` and retains the selected hash. Extract, Expand-Label,
  Derive-Secret, Finished, traffic keys and in-place traffic-secret update
  compose Bear primitives. ECDH borrows the peer point, validates the
  product, and preserves the fixed-width shared X coordinate. Local
  scalars use Bear's encoding, including big-endian X25519 scalars.
- A `tls13_record` holds one direction's key epoch and sequence number.
  Rekey wipes the previous key and resets the sequence. The engine can
  query the 2^24-record update threshold; the record helper refuses nonce
  wrap even if the engine fails to update. AES uses ct64/GHASH-ctmul64;
  ChaCha20-Poly1305 uses Bear's scalar implementation.
- Record opening consumes one complete record in place. Header validation
  is separately available to the incremental engine before it accepts a
  payload. Successful opening returns the content type and length within
  that record, so S2 retains record boundaries and owns epoch changes.
  Authentication or protocol failure wipes the supplied bytes and record
  keys, clears output counts, and latches the error. Scratch HMAC, HKDF,
  ECDH and AEAD contexts are wiped before return.
- The plaintext helper takes the engine's CCS-window decision explicitly.
  Application-data readiness is also supplied by the engine. S1 validates
  framing and content types; it has no handshake state from which to infer
  either permission. Sending adds a type byte and tag without padding.
  Receiving checks the outer limit, authenticates, checks the complete
  inner limit, and then removes padding.

**Evidence, 2026-10-09:**

`tools/tls13_vectors.py` embeds the RFC 8448 §3/§5 octets with source
attribution and generates the checked-in header; `--check` passes. The host
harness compares the RFC's six extracts, 39 expansions, 17 wire records,
four Finished values and two ECDH exchanges. It reconstructs the transcript
from complete handshake messages, including the HRR synthetic message.
The RFC's exporter/resumption calculations are fixture checks of generic
HKDF helpers; no production exporter or resumption path is added.

Independent expectations from Python `hashlib`/`hmac` and `cryptography`
cover SHA-256/SHA-384 schedules, all three AEADs, and X25519/P-256/P-384,
including leading-zero NIST shared coordinates and invalid peer points.
KeyUpdate checks derive the new secret, key and IV, reset the sequence,
match a record encrypted independently under the new key, and refuse it
under the old key. The record cases include empty/maximal content,
padding at/above the full inner limit, all-zero inner plaintext,
malformed alerts, encrypted CCS, pre-handshake application data,
header/body/tag damage, replay, wrong sequence and sequence exhaustion.
There are **165 named byte comparisons**, plus the framing/state/wipe
assertions: **33 accepted independent AEAD cases and 27 valid-tag protocol
refusals**. These checks pass under ASan/UBSan against the adapted
foundation archive.

The existing foundation harness passes its pristine/adapted upstream
crypto and X.509 tests and differential fixtures. The freestanding
`tools/audit_bearssl.py` check passes with the new objects in the complete
archive: hidden symbols, supported relocations, retained license, and
the existing six os64 imports. S1 introduces no allocator calls. On
x86-64 a record context is **328 bytes**. GCC 14.2.0's freestanding `-O2`
stack report gives a largest new individual frame of **672 bytes** in
Expand-Label; HMAC's frame is 496 bytes and the AEAD helper's is 256 bytes.
Those are individual frames, not bounds for complete call chains through
Bear. Engine/buffer memory accounting remains S2 work.

Reproduction from the repository root:

```sh
python3 tools/tls13_vectors.py --check
python3 tools/test_bearssl_host.py --output /tmp/tls13-foundation
python3 tools/test_tls13_vectors_host.py \
  --foundation /tmp/tls13-foundation/adapted/core.a \
  --output /tmp/tls13-vectors
python3 tools/audit_bearssl.py
```

The initial sandboxed runs used `ASAN_OPTIONS=detect_leaks=0` after
LeakSanitizer failed at its final scan. A follow-up with verbose diagnostics
showed thread attachment denied with `errno 1` (`EPERM`), while
`TracerPid` was zero: the generic ptrace hint did not establish that the
runner was being traced. The same vector binary passed outside the
sandbox with `ASAN_OPTIONS=detect_leaks=1`, and the diagnostic log confirms
that LeakSanitizer scanned its thread. The existing pristine/adapted
crypto, X.509 and foundation binaries also passed outside the sandbox
with leak detection enabled; their differential outputs still match.
ASan/UBSan remained active throughout. S1's evidence is offline
foundation/vector evidence; live TLS 1.3 interoperability and guest
acceptance remain the S2/S3 gates above.

## S2 as built — client engine and host integration (2026-10-09, Quinn)

Built on `codex/tls13-s2`, stacked on S1 at `10944de5` (PR #241).

`port/tls13_handshake.c` implements the buffer/ack engine beside BearSSL.
The public byte wrapper owns the union and retains the accepted-prefix,
handshake budget, EOF, close and sticky-terminal rules. Its default selects
TLS 1.3; explicit `OS64_TLS_PROTOCOL_TLS12` selects the existing profile.
The certificate validator, trust snapshot, time and entropy adapters are
shared. No new kernel service or public crypto operation was added.

The engine implements the offered profile, single HRR (including cookie-only
retry), streamed Certificate and NewSessionTicket framing, ECDSA/RSA-PSS
CertificateVerify, both Finished messages, directional application secrets,
KeyUpdate and bidirectional close_notify. A separate 16 KiB accepted-output
buffer lets a requested KeyUpdate precede buffered application bytes without
losing them or encrypting them under the old key. The group descriptors drive
both the offered group list and peer-share validation. Record errors remain
latched across attempted rekey, incorporating Fable's S1 review note.

Compatibility CCS follows RFC 8446 appendix D.4: immediately before CH2 on
the HRR path, otherwise before the encrypted client flight. No second CCS is
sent after CH2. This specifies the HRR placement left implicit by step 8
above; the compatibility-mode ruling is unchanged. Outer CCS still takes the
plaintext parser during the permitted encrypted-handshake window.

Libfetch retries once, on a fresh dial to the same peer, only after
`PEER_CHOSE_TLS12`. It keeps the hostname and trust snapshot and obtains fresh
entropy for BearSSL. Its detail struct records the negotiated version and
retry flag. Libway carries those facts to yonder; os64get gains `-v` to show
them. Certificate/protocol errors, peer alerts, timeouts and truncation do not
permit retry. The existing os64get ELF audit had stale dependencies from
before the libfetch migration; it now checks os64get → libfetch → libtls/libgzip.
The public structs grow and their consumers must be rebuilt. The public
library still exports 31 operations and imports the same 15 OS functions; its production archive contains 72 selected sources. The audit
also excludes the parser-fuzz seam from production.

### Memory and build measurements

Measured from the freestanding x86_64 debug information:

| Object | Bytes |
|---|---:|
| TLS 1.3 engine | 59,912 |
| Byte wrapper including the protocol union, hostname and ALPN | 61,320 |
| Owned policy validator | 36,264 |
| Connection plus validator, excluding shared trust | 97,584 |
| Shared trust object, excluding its anchor storage | 18,456 |

The 64 KiB engine target includes the additional accepted-output buffer.
The validator and shared trust are separate allocations; 64 KiB is not a
claim about their combined footprint. The host coverage counter adds eight
bytes to the test-only wrapper (61,328). The engine allocation is checked for
complete wiping before free; the validator and trust contain public
certificate data and their allocation lifetime is checked separately.

The compiler's largest new individual handshake frame is 736 bytes in
`server_hello`; CertificateVerify uses a bounded 608-byte frame. These are
individual frames, not cumulative call-stack bounds. The resulting
`libtls.so` has 157,458 text/rodata bytes, 2,008 data bytes and zero BSS by
`x86_64-elf-size`. The full userland build, including yonder, succeeds.

### Host acceptance

`tools/test_tls13_engine_host.py` generates a fixture RSA root and RSA-2048 /
ECDSA-P256 leaves with PKCS#1 certificate signatures. Its C harness links
OpenSSL **3.0.13** and drives memory BIOs through the actual public byte API,
with the production certificate-policy implementation. Memory BIOs replace
the proposed socket/subprocess peer orchestration: they expose the same
OpenSSL server handshake, group, ticket, KeyUpdate and client-certificate
controls directly, while keeping ciphertext fragmentation deterministic.
Socket/guest acceptance remains S3. The host needs OpenSSL development files
and Python `cryptography` (tested with 41.0.7); host C is GCC 13.3.0.

The passing cases cover:

- Exact ClientHello framing, suites, extension order, signature lists,
  versions, ALPN and X25519/P-256 shares. All **18** suite × leaf × group
  combinations (X25519, P-256, P-384 HRR), 70,013 bytes each way, ciphertext
  fragments of 1/37/4096 bytes and one-byte plaintext reads. Additional real
  handshakes verify ECDSA P-384/P-521 and RSA-PSS SHA-384/SHA-512 with a
  SHA-256 cipher suite. Requested and
  unrequested KeyUpdate carry further data across both directional switches.
- Cookie-only HRR retaining the original hello/shares, duplicate/unchanged/
  unsupported HRR, session echo, changed final suite and trailing bytes after
  ServerHello. Both downgrade sentinels are refused; stripping TLS 1.3 from
  the offer to a real OpenSSL peer produces its own DOWNGRD01 marker and a
  protocol refusal. Real TLS 1.2 peers, including PKCS#1-only RSA, permit a
  fresh authenticated Bear connection.
- Valid-tag tampered CertificateVerify and Finished, forbidden PKCS#1 or
  mismatched signing key, early application data, malformed/unoffered EE
  selections, fragmented headers, alert/ticket interleaving, trailing bytes
  after Finished/KeyUpdate, and KeyUpdate ordering before accepted plaintext.
- For each suite, independently sealed inner plaintext of 16,385 bytes
  passes; 16,386 bytes with padding fails as LIMIT/record_overflow without
  exposing plaintext. Bad tags, encrypted CCS and CCS after Finished fail.
  OpenSSL without compatibility CCS and without tickets also completes.
- Wrong hostname, expiry and untrusted chain fail at the shared certificate
  gate. A scripted valid PSS-signed certificate is refused with the policy
  signature reason; PSS CertificateVerify over the accepted RSA leaf passes.
  Certificate streaming across one-byte records, per-entry extension refusal,
  malformed CertificateRequest and duplicate ticket extensions are covered.
  Optional client authentication sends an empty Certificate; a peer requiring
  one returns certificate_required, surfaced as a terminal alert.
- Both close orders, buffered plaintext and unflushed output at bare EOF,
  application records discarded after local close, abort during pending
  handshake output, independent interleaved clients, failed constructor
  allocations, proactive write-key rollover, and the 8 KiB message / 1 MiB
  accepted-ciphertext caps through the public interface with sticky status.

The surrounding regressions also pass: S1's 165 vector comparisons and
independent record tests; all six TLS 1.2 engine suites; public OS inputs;
367 policy chains, 61 anchor cases and 19 handshake gates; libfetch's 833
checks with two chunk seeds; libway's 248 checks and 126 fetch integration
checks; TLS transport and fetch transport suites. The BearSSL import/vector
checks and both private/public ELF audits pass. Leak detection remains
**enabled**, alongside ASan/UBSan, for the host acceptance and fuzz runs
outside the sandbox; the existing adapted archive from S1 was reused at the
same pin/configuration.

### Fuzz evidence

The raw-flight and authenticated-parser campaigns use twelve captured
flights: three suites × two leaf types × ordinary/P-384 retry. Captures
include CertificateRequest, tickets and requested KeyUpdate. The test-only
parser entry point is absent from the production ELF. Each mutation checks
bounded accepted prefixes, the pre-Finished plaintext gate, live or sticky
terminal state, allocation balance and engine wiping; sanitizers check memory
accesses and leak cleanup. The application-transfer cases independently check
byte-for-byte delivery across fragmentation and key changes.

The completed run used **1,800 seconds**, initial seed **147731**, and
**160,087 mutations**: 80,044 raw-record cases and 80,043 authenticated-parser
cases. It produced 119,516 terminal outcomes and 40,571 live outcomes. The
coverage mask was **0x110a914**, reaching ServerHello/HRR, EncryptedExtensions,
CertificateRequest, Certificate, CertificateVerify, Finished,
NewSessionTicket and KeyUpdate. ASan, UBSan and the final LeakSanitizer scan
completed without findings (exit 0); there was no crash seed to add as a
regression. The local run log is `/tmp/tls13-s2-fuzz-30m.log`.

This run used the initial capture runner with ECDSA-signed fixture chains;
the final acceptance matrix also covers PKCS#1-signed chains and the
PSS-signed-chain refusal. Production engine sources were unchanged during
the run. The subsequent harness retention/replay refinement was separately
smoke-tested; it does not change the production parser or record layer.
`tools/fuzz_tls13_host.py` retains fixture keys, raw flights, native replay
records, executable and log. It writes each case's seed and iteration before
executing it, including for sanitizer failures; `--replay DIRECTORY` reruns
that exact case using the retained executable/corpus. Replay artifacts are
trusted local test files. The retention and exact-case replay path also
passed a separate smoke run.

S2 does not claim guest networking or live-site acceptance. Those checks,
the DEBTS replacement and the final move of this design remain S3; Fable and
then Opus review this engine slice.
