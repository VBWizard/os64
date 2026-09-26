# libfetch POST evidence

Branch: `codex/fetch-post`. Userland change; no kernel changes.

- Strict full `make -j8`: passed.
- `tools/test_fetch_host.sh`: 383 checks per seed, two seeds, ASan/UBSan
  and leak detection. Byte-exact 70,000-byte binary uploads over plain
  and simulated TLS transport, full/one-byte/random response reads,
  partial writes, zero-length POST, five redirect codes, same-URL
  POST -> GET, invalid options, conflicting framing, cancellation and
  failed writes without retry. TLS is a scripted test double here.
- `tools/test_http_host.sh`: passed, including request-header differential
  against Python `http.client` for lengths 0, 1, 17, 8192 and 70000 and
  urlencoded/multipart content types, plus the existing response corpus.
- `tools/test_wend_host.sh`: 177954 checks, zero failures, zero live
  blocks; saved page layouts unchanged.
- `tools/test_fetch_transport_host.sh`: passed.
- `git diff --check` and `tools/stale_refs.sh`: clean.

## Guest

QEMU, eight cores, e1000/slirp, current kernel and rebuilt userland.
`wend http://10.0.2.2:58080/post-form`, then activate button 1.
The server observed:

```text
GET /post-form HTTP/1.1
POST bytes=31 crc32=10d64684 type=application/x-www-form-urlencoded
POST /post-echo HTTP/1.1
```

Wend displayed the echoed result:

```text
method=POST
content-type=application/x-www-form-urlencoded
bytes=31
snack=chocolate+donuts&send=yes
```

After quitting wend and syncing, `/sys/net/tcp` showed the POST connection
`CLOSED`, detached, 164 received bytes / 287 transmitted bytes, zero
retransmits, zero inflight bytes and zero queued sends. Global fast
retransmits and local drops were zero.

This is guest evidence, not P5 evidence. Cookies and connection reuse are
separate work. Reload and Back use GET and do not replay a submitted body.
Rebuild/deploy libfetch and its callers together because the options and
hop and head structures grew.

## PR #140 review follow-up

All three findings addressed: early upload responses, explicit unencrypted
form replay consent, and method-aware download advice.

- Host fetch suite: 641 checks per seed, two seeds under ASan/UBSan with
  leak detection. Early 401/413 while TLS output is pending and over plain
  TCP; 100/103 followed by an upload-dependent final response; interim and
  final heads together; exact upload prefixes/resumption without duplicates;
  full/one-byte/random reads. Final method checked across all five redirects.
- Transport, HTTP parser and wend host suites passed. Wend: 177954 checks,
  zero failures or live blocks. Strict full build and whitespace/stale-reference
  checks passed.
- Eight-core QEMU: `fetchposttest http://10.0.2.2:58080` passed with zero
  failures. A 413 sent before the server reads the request body remained
  readable; 100 Continue resumed a 70000-byte upload (CRC32 `634f3d0d`).
- Wend screens checked: POST PDF says it cannot display/save this POST
  response; POST -> 303 -> GET PDF still offers the GET download command.
  A 307 from an HTTPS URL asked `Resend form data unencrypted to 10.0.2.2?`;
  answering no produced no request to the redirect target. The HTTPS URL
  used the supported plain proxy for this controlled fixture, not native TLS.
- Guest TCP snapshot: nine connections opened/reaped, no retransmits,
  no local drops, no inflight or queued sends on the remaining CLOSED row.
  One reset/refusal was counted during the early-rejection run.

Native TLS upload timing is covered by the scripted host seam; this round
has no new native-TLS guest or P5 evidence.
