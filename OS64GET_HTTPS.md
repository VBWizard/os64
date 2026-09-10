# Native HTTPS in os64get

Application integration slice, based on merged `userland` commit `8500f14`
(PR #87). This is the application integration described by slice E of
[TLS.md](TLS.md). Public root-bundle selection and distribution are separate;
acceptance uses an explicitly selected controlled fixture bundle.

## Result and scope

`os64get https://HOST/PATH [DEST]` can fetch directly through `libtls.so`.
It verifies the requested hostname and certificate chain before sending HTTP,
then uses the existing HTTP framing, gzip decoder and staged publication.
Each redirect creates a new connection and validates the new origin.

The connection implementation is private to `userland/apps/os64get`.
Gopher's os64get status translation also recognizes TLS failures. No kernel changes,
new syscalls or libfetch extraction are needed. Valet GET/LIST, replacement
backups, URL destination selection and filesystem publication stay with their
existing owners. Public root installation is not part of this slice.

## Routing decisions

The routing choices are settled with Chris:

1. **Explicit HTTPS proxies:** retain the explicitly configured terminating
   proxy and its disclosure. Direct HTTPS uses os64's TLS. CONNECT tunneling
   is deferred until needed; Chris normally has no proxy configured.
2. **HTTPS-to-HTTP redirects:** refuse automatic downgrade, explain that the
   target uses unencrypted HTTP, and display it for an explicit new command.
   The warning is visible even with `-q`.

Routing is:

| URL and selected route | Connection and request |
|---|---|
| HTTP direct | Plain TCP to origin; origin-form request |
| HTTP explicit proxy | Existing plain TCP proxy; absolute-form request |
| HTTPS direct, including no_proxy bypass | TLS to origin; origin-form request |
| HTTPS explicit proxy | Existing terminating proxy route and disclosure |

The environment is evaluated for each redirect target, using the existing
scheme-specific settings and no_proxy matching. A direct TLS failure does
not trigger a retry through a proxy or cleartext. Invalid explicit settings
remain errors, rather than being silently bypassed.

## Existing seams and changes

| Owner | Integration |
|---|---|
| `url_ask` in `os64get.c` | Keep redirect/status policy; authenticate a direct HTTPS hop before writing its request |
| `url_io_read` and `url_io_t` | Own plain/TLS byte reads and retained error detail |
| `http_stream_init` in `http.c` | Keep its existing callback seam; parser receives HTTP plaintext |
| `receive_url_body` | Keep framing-before-gzip and provisional writes; preserve transport error detail separately |
| `fetch_url` | Centralize connection teardown on its failure and success exits; check HTTP completion before publication |
| `install.c` | Keep preparation, sync/reread verification, publication and cancellation rules |
| `userland/GNUmakefile` | Link os64get with libtls and retain its existing libgzip dependency |

Add private `url_io.c` / `url_io.h` for connection ownership, request writes,
blocking plaintext reads and bounded cleanup. This keeps TLS pumping out of
redirect policy and keeps HTTP syntax out of the TLS library. It is an app
module, not a new public networking abstraction.

A URL reply owns one connection object. That object contains either the raw
TCP handle or the TLS transport, plus error and timeout detail. The HTTP
stream references it in place; do not copy a live reply. Close/free clears
ownership before returning, so each failure path can share the same cleanup.

`url_ask` retains dial-error reporting and its first-hop/redirect distinction.
The I/O owner takes responsibility for an established handle. TLS creation
adopts it only on success; the I/O owner closes it when creation fails.
Subsequent cleanup uses the TLS transport's free operation, not a second raw
handle close.

## Trust and identity

Load one immutable trust snapshot lazily on the first direct HTTPS hop,
using `os64_tls_trust_reload` and its existing tls.conf selection policy.
Retain it through the redirect chain and release it on invocation cleanup.
An HTTP-only invocation need not open a trust store. A later invocation sees
an updated bundle; redirects within one invocation use the same snapshot.

Use the current URL's hostname for TLS configuration and SNI, never the
resolved address or the previous hop's identity. Offer only `http/1.1` in
ALPN; an absent selection remains supported by the existing TLS profile.
IP-literal HTTPS and names outside the public TLS hostname policy are refused
by name. The test rig supplies a DNS-name mapping for its LAN address.

Report selected bundle/config paths and store error detail on trust-load
failure. There is no embedded test root, automatic root download, missing-root
fallback or certificate-verification bypass in os64get. Test configuration
selects its own fixture bundle through tls.conf.

## Requests, waiting and cancellation

Use the transport's retained 30-second handshake budget and 2-second shutdown
budget. DNS/dial remain outside them, as the merged API specifies; this slice
does not claim to fix the booked dial-interruption limitation.

A request gets one finite 30-second write budget across accepted prefixes,
flush and pending ciphertext submission. Advance only by accepted plaintext
counts; ciphertext suffixes belong to the adapter. The plain HTTP path uses
finite write patience too. Do not restart the budget after a short write or
caught signal, regenerate an accepted request prefix, or discard incoming
application bytes to make room for output. The small GET request is bounded
by HTTP_LINE_MAX; unexpected response backpressure during request submission
must return a named failure rather than spin indefinitely.

A plaintext source read retains one 30-second idle deadline until it can
return authenticated HTTP bytes. TLS record/alert progress alone does not
restart that deadline. Returning HTTP bytes allows the next source read to
start its own idle interval, preserving the existing slow-download policy.
Pass the remaining interval to `step`; the adapter supplies short alternating
waits only when both network directions need service.

Inspect `install_cancelled()` before and after blocking work and after
NEED_PROGRESS. A caught signal with no cancellation request resumes under the
same deadline. A requested cancellation explicitly aborts TLS, closes owned
resources and returns GET_CANCELLED (130). Publication retains install.c's
existing deferred-cancellation boundary.

## HTTP completion and TLS closure

The HTTP parser remains responsible for message boundaries. A positive
plaintext count accompanied by terminal TLS status may be handed to the
parser, but retain the status for the next source read and final verdict.
Never convert a TLS failure to ordinary source EOF.

| HTTP body framing | Completion requirement |
|---|---|
| Content-Length | Exactly the declared body bytes, plus successful content decoding |
| Chunked | Completed chunk framing and trailers, plus successful content decoding |
| Connection close | Clean TLS EOF, plus successful content decoding |

For independently framed complete bodies, missing peer close_notify does not
by itself invalidate the already authenticated message. For close-framed
bodies it does: raw FIN is truncation and the destination is not published.
This distinction follows [RFC 9112 section 9.8](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.8)
and [section 8](https://www.rfc-editor.org/rfc/rfc9112.html#section-8).

On normal completion, request TLS closure and drive it within the shutdown
budget. Extra plaintext after the selected HTTP message is not appended to
the file; teardown must remain bounded even if the peer continues sending.
An absent close notification or cleanup-only timeout/reset after an
independently complete message can end cleanup without rejecting that message.
Certificate, protocol/authentication failures observed during response
processing, incomplete framing, gzip failure and cancellation prevent
publication. Failure and redirect cleanup may abort immediately; do not drain
an unbounded redirect body or add a shutdown wait to every failed hop.

Preserve the existing gzip final-input rule, trailer CRC/size checks,
expansion limits, staged-file sync and reread verification. A TLS failure
cannot bypass `body.result == HTTP_BODY_DONE` or publish_run preparation.

## Errors and documentation

Keep the existing request/header/body/publication exit categories for plain
HTTP. Add a named TLS failure exit category (value 16) for trust,
identity and handshake failures, with stable TLS/store reason names. During a
response, report TLS detail alongside a failed/incomplete HTTP read. Explicit
cancellation takes precedence over a less useful network error.

The help text, OS64GET.md, and Gopher status messages describe native HTTPS
and its trust failures. The terminating helper remains optional; CONNECT is
outside this slice. The ELF audit covers os64get alongside the other TLS
consumers.

## Validation and review

1. Host seams exercise real URL I/O code with short transfers, delayed
   plaintext, interruption/resumption, cancellation, clock failures, retained
   write/read budgets and exact ownership cleanup.
2. Existing HTTP framing, gzip and os64get staging/publication suites remain
   passing. Extend whole-app cases for TLS errors, redirects and preservation
   of an existing destination after a failed transfer.
3. A standard-library Python HTTPS peer uses the existing public fixture key
   and an explicitly installed fixture root. It runs on Windows for P5
   acceptance and on Linux for QEMU. Test length, chunked and close-framed
   identity/gzip replies; empty bodies; truncated headers/bodies; bad
   certificate identity; wrong roots; handshake/read stalls; cancellation;
   redirect targets; and the agreed proxy/downgrade policy.
4. Check exact downloaded bytes and exit codes, replacement preservation,
   cleanup of owned scratch files, and repeated success/failure resource use.
   Include a complete framed response without close_notify and an incomplete
   close-framed response with the same transport ending.
5. Strict userland build, public ELF audit, diff and stale-reference checks;
   isolated QEMU and P5 checkout against the controlled peer.

Review the integration design before implementation is submitted for review.
Fable reviews architecture before Codex's implementation review. Public-site
acceptance and root-bundle provenance/update policy belong to the separately
selected public-trust work.

## Controlled checkout

Build from `.worktrees/os64get-https` on `codex/os64get-https`. Transfer the
rebuilt os64get, Gopher, and changed libraries together using the normal deployment
workflow. Host validation commands:

```sh
bash tools/test_os64get_io_host.sh
bash tools/test_os64get_host.sh
ASAN_OPTIONS=detect_leaks=0 bash tools/test_http_host.sh
ASAN_OPTIONS=detect_leaks=0 bash tools/test_gzip_host.sh
make -C userland
python3 tools/audit_tls.py
```

The HTTPS peer needs only standard Python. On the Windows host, PowerShell can
run it directly from WSL (no virtual environment or cryptography package):

```powershell
py '\\wsl$\Ubuntu-big2\home\yogi\src\os64\.worktrees\os64get-https\tools\test_os64get_https_peer.py' --bind 0.0.0.0 --fixtures "$env:TEMP\os64get-https-fixtures"
```

The output directory contains `roots.pem` and `expected.bin`. Copy the root to
os64 as `/home/https-fixture-roots.pem`; keep `expected.bin` on the host for
exact comparisons if you copy downloads back.
Select this controlled root with `trust_store = /home/https-fixture-roots.pem`
in the `tls.conf` selected by your configuration ladder. Keep a copy of any
previous configuration to restore after checkout. In `/home/hosts`, map the
Windows host's reachable LAN address to both fixture names:

```text
192.168.x.y example.test wrong.test
```

Unset `https_proxy` for direct-TLS checkout. The normal peer uses port 18270;
18271 accepts TCP but stalls the handshake, and 18272 serves plain HTTP.
For QEMU on Linux, use `10.0.2.2` in the hosts entry and run the same peer with
`python3 -S tools/test_os64get_https_peer.py`.

```sh
os64get https://example.test:18270/gzip-chunked /home/https-got.bin
wc -c /home/https-got.bin
```

The byte count should be 65536; this is a size check, not an exact comparison.
The isolated guest acceptance also compares each downloaded byte against
`expected.bin`.

Repeat with `/length`, `/chunked`, `/close`, `/gzip-length`, `/gzip-close`,
`/bare-length` (complete length, no TLS closure), and `/redirect`. They must
produce the same 65536 bytes. `/empty` must produce an empty file.

For failures, first put a known file at the destination; check that its bytes
remain intact and that managed scratch files are removed:

| Request | Expected status |
|---|---|
| `/cut-length`, `/cut-chunked`, `/cut-close` | 7 |
| `/bad-gzip` | 8 |
| `/headers-cut` | 6 |
| `/downgrade` | 15, explanation and HTTP target visible even with `-q` |
| `https://wrong.test:18270/length` | 16, certificate failure |
| `https://example.test:18271/length` | 16 after the handshake budget |
| `/stall` | 6 after 30 seconds waiting for headers |
| `/cancel`, then Ctrl+C | 130 with original preserved |

Also select a nonexistent root bundle and confirm status 16, then restore the
fixture configuration. An explicitly configured terminating proxy should
retain its disclosure and existing behavior; `no_proxy` bypass should take
the native TLS route. These fixtures prove integration against a controlled
peer. They do not establish public-site compatibility or production trust.
