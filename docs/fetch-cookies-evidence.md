# libfetch cookie/Referer hook evidence

Branch: `codex/fetch-cookies`, based on `codex/fetch-post` (`22da1a68`).
This is userland work; the kernel is unchanged.

- Strict full `make -j8`: passed (the existing legacy ELF fixture RWX
  linker warnings remain).
- `tools/test_fetch_host.sh`: 465 checks per seed, two seeds, ASan/UBSan
  with leak detection. Includes the POST suite, separate Set-Cookie values
  with Expires commas, callback ordering across origins, static credential
  filtering, malformed/duplicate/unterminated callback output, both Referer
  sources over plain/TLS/proxy transports, omission of interim/trailer and
  oversized cookie fields, late head failure, callback cancellation, and
  a TLS protocol failure accompanying an authenticated prefix.
- `tools/test_http_host.sh`: passed. The general header hook is compared
  with Python `http.client` across read chunks 1, 2, 3, 7, 17, 255 and 8192,
  including duplicate Set-Cookie fields and an unknown header. Existing
  HTTP framing/URL/body differential cases still pass.
- `git diff --check` and `tools/stale_refs.sh`: clean.

## Guest

Eight-core QEMU, e1000/slirp, rebuilt userland and the existing kernel.
Run `python3 tools/httptestd.py --port 58080` on the host, then:

```text
/tests/fetchhooktest http://10.0.2.2:58080
fetchhooktest: PASS (0 failures)
```

Server observations:

```text
GET /set-cookie HTTP/1.1
GET /needs-cookie HTTP/1.1
cookie accepted=True
GET /referer HTTP/1.1
GET /referer HTTP/1.1
GET /needs-cookie HTTP/1.1
cookie accepted=False
POST bytes=3 crc32=8d623081 type=application/x-www-form-urlencoded
POST /303-after-post HTTP/1.1
GET /post-echo HTTP/1.1
POST bytes=3 crc32=8d623081 type=application/x-www-form-urlencoded
POST /307-after-post HTTP/1.1
POST bytes=3 crc32=8d623081 type=application/x-www-form-urlencoded
POST /post-echo HTTP/1.1
```

The guest also checks response contents: both Referer probes are empty,
303's follow-up has no body metadata, and 307's has the same bytes/type.
`/sys/net/tcp` after completion showed nine connections opened, all nine
CLOSED/detached, no queued/inflight data, zero retransmits, zero fast
retransmits, zero local drops, and zero resets received.

This fixture holds a fixed test cookie; it is not a cookie jar. It is an
explicit network test, not in unattended testrun, because it requires the
fixture server. Live TLS is not exercised by this guest run; TLS transport
choices and failure handling are covered by scripted host tests. No P5
validation has been claimed.

Rebuild and deploy libfetch and its callers together: options structs grew.
The inherited 2048-byte response-line buffer omits oversized cookies whole;
request callback output is bounded to 1024 bytes. Those limits are explicit
in the API contract and tested. Cookie storage/matching/referrer policy
belong to the navigator, and connection reuse is a separate feature.

## PR #141 review follow-up

The cookie callback receives `bool encrypted` after the originating URL.
The value comes from the actual transport, false for HTTPS URLs fetched
through a plain proxy. The jar and Secure-cookie acceptance policy remain
libway's responsibility. Rebuild callback implementations for this signature.
The parent POST review fixes are merged into this branch.

- Combined host suite: 777 checks per seed, two seeds under ASan/UBSan with
  leak detection. Secure-cookie values arrive unchanged with true for direct
  TLS and false for direct HTTP or HTTPS-via-plain-proxy, across full,
  one-byte and random reads. Inherited early-response tests also pass.
- HTTP differential/observer and transport suites passed; strict full build,
  `git diff --check` and stale-reference checks passed.
- Eight-core QEMU: `fetchhooktest` passed on direct HTTP and again on an
  HTTPS URL using `--proxy` with `$https_proxy` pointing to the fixture.
  Both response callbacks asserted false encryption; cookie redirect order,
  Referer stripping, missing-cookie refusal and POST redirects still passed.
- `fetchposttest` passed on the combined build. The early-413 connection
  sent 8318 bytes total, abandoning the remainder of the 70000-byte body;
  the 100-Continue upload arrived complete with CRC32 `634f3d0d`.
- TCP snapshot: 20 connections opened, 18 reaped, remaining rows CLOSED and
  TIME_WAIT, detached with no queued/inflight sends. Zero retransmits,
  resets, connect timeouts or local drops.

This adds plain-proxy guest evidence, not native-TLS guest or P5 coverage.
