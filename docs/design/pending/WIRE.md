# WIRE.md — what yonder's requests look like on the wire, and how many connections they take

*Two briefs, two PRs, in this order, written by Fable on 2026-10-23 from
the 2026-10-22 surf: Google and Microsoft answered yonder's Chrome
string with "unusual traffic" pages, and a storefront that fetched 172
scripts and sheets arrived after 64 seconds with the main loop busy ten
percent of the time. Both are libfetch and libway, both wire-facing: each
PR gets a QEMU `make run-net` page load before it opens, the three passes,
and a Codex round. Neither is a Modern Web slice; they are the browser's
own, and they go before the next slice because Google is where searching
starts. No host names from the corpus in either PR: counts and headers.*

## 1. Identity: a preset is a browser, not a string

**The problem.** The agent preset (`userland/apps/yonder/agent.c`) sets one
header. A real Chrome 154 request carries, beside its `User-Agent`, the
client hints (`sec-ch-ua`, `sec-ch-ua-mobile`, `sec-ch-ua-platform`), the
Fetch Metadata headers (`Sec-Fetch-Site`, `Sec-Fetch-Mode`,
`Sec-Fetch-Dest`, and `Sec-Fetch-User` on a navigation a person asked
for), `Accept-Language`, and `Upgrade-Insecure-Requests: 1` on a
navigation. libfetch's fixed block (`http.c`, the request writer) sends
`Host`, `User-Agent`, `Accept`, `Accept-Encoding: gzip, identity`, the
cookies, the referrer and `Connection: close`. A bot gate that scores
"claims Chrome, sends none of Chrome's headers" is right about us, and
that is the score Google and Microsoft gave.

**The rule.** A preset is a whole identity: the string AND the headers that
every request from that browser carries. The headers come in two kinds:

- **Static, the preset's own.** The `agent.c` table gains them beside the
  string, verbatim as the browser sends them. Chrome on Windows (156, as
  captured: "W1, as built" below):
  `sec-ch-ua: "Not:A-Brand";v="8", "Chromium";v="156", "Google
  Chrome";v="156"` (the UA-CH brand list, the GREASE brand included
  because Chrome always sends one), `sec-ch-ua-mobile: ?0`,
  `sec-ch-ua-platform: "Windows"`, `Accept-Language: en-US,en;q=0.9`.
  Firefox on Windows and Safari on iPhone send no client hints and the
  same `Accept-Language`; yonder's own preset sends no hints. The builder
  captures each browser's current set from the browser itself (a request
  to a local listener, or the published header lists) and cites the
  capture in the PR, because the brands and the version move.
- **Per request, the request's own.** Fetch Metadata (the standard of that
  name) is computed from facts libway already holds: `Sec-Fetch-Site`
  from the request's origin against the current URL's (`same-origin`,
  `same-site` by `way_origin_same_site`, `cross-site`, `none` for a
  typed address), `Sec-Fetch-Mode` from the request's mode (`navigate`
  for a document, else `cors`, `no-cors`, `same-origin`),
  `Sec-Fetch-Dest` from the destination the caller names (`document`,
  `script`, `style`, `image`, `font`, `empty` for fetch and XHR), and
  `Sec-Fetch-User: ?1` only on a navigation the person started (typed,
  clicked, a bookmark), never on a script's. `Upgrade-Insecure-Requests:
  1` on a document navigation only. These live in libway (`fields_of`
  in the fetch job, and the navigation's equivalent in `trip.c`), not in
  the preset, since every browser sends them and they describe the
  request, not the browser. A preflight carries `Sec-Fetch-Mode: cors`
  and `Sec-Fetch-Dest: empty`, as browsers do.
- **What is NOT claimed.** `Accept-Encoding` keeps `gzip, identity`: there
  is no brotli decoder, and claiming `br` would be a header that lies. No
  HTTP/2, no TLS-fingerprint mimicry: a gate that scores the handshake
  itself will keep flagging yonder, and that is the honest outcome for a
  browser that is not pretending to be another engine. DIVERGENCES.md
  row, one line.

**Where.** `agent.c` (the table), libway (the Fetch-Metadata computation
is pure and lives beside `way_origin_header` in `origin.c`, where its
inputs already are; the fetch job's per-hop fields and the navigation's),
and libfetch's per-hop callback, which takes the lines because they
change from hop to hop (as built, below: the brief's first answer, the
extra-headers block, is fixed for a whole fetch).

**Proof.** Host cases: each preset's static block byte for byte against
the capture; `Sec-Fetch-Site` for the four answers, `Mode` for the four
modes, `Dest` for each destination the callers name, `User` present on a
person's navigation and absent on a script's, and on a redirect's later
hops as the standard says (it says present on a person's: as built,
below); the preflight's pair; `Upgrade-Insecure-Requests`
on documents only. A mutant per header. QEMU `make run-net`: a page load
and a cross-origin script fetch, the wire read back (the slirp side's
capture or the fetch test's listener) showing the full set. On the P5:
a search on the engine that refused us answering with results, said in
the PR body by outcome, not by name; if it still refuses, the PR says so
and the gate has moved to the handshake, which is the divergence row.

### W1, as built (2026-10-24, Opus)

The values come from a capture, not from this brief: Chris's Chrome 156
and Firefox 158 on Windows, each making a typed navigation, a clicked one,
a script, a sheet, a picture, a same-origin fetch, a cross-site fetch and
its preflight to a local listener (the W1 capture files 02-10 for Chrome,
19-27 for Firefox, raw request bytes, cited in the PR). Where the capture
or a standard disagreed with § 1 above, the build follows them:

- **Chrome is 156, and its brand list puts the GREASE brand first:**
  `"Not:A-Brand";v="8", "Chromium";v="156", "Google Chrome";v="156"`
  (capture-02). The agent string is the reduced `Chrome/156.0.0.0`.
  Firefox is `rv:158.0` / `Firefox/158.0` (capture-19). Safari is not
  captured: no client hints, and the language list MDN gives.
- **A preflight carries no client hints** (capture-08); it does carry
  `Accept-Language`, and its Fetch Metadata is cors/empty with the
  request's site.
- **`Sec-Fetch-User` stays on every hop of a person's navigation.** HTML's
  "create navigation params by fetching" sets the request's
  user-activation once, and its redirects are the same request; Fetch
  Metadata § 2.4 reads only that flag. A page's own navigation (a
  refresh, a script's `location`, `click()` or `submit()`) never carries
  it, on any hop.
- **Fetch Metadata and the client hints go only to a potentially
  trustworthy URL** (Fetch Metadata § 3 step 1; the hints are a
  secure-context feature). A plain-http hop carries
  `Upgrade-Insecure-Requests`, `Accept-Language` and `Priority` and
  nothing else of this. The QEMU proof's typed `http://` address,
  redirected to `https://`, shows both hops.
- **`localhost` is not trustworthy in yonder** (the DIVERGENCES row from
  slice 2: os64 cannot promise the name resolves to loopback), so
  `http://localhost` hears no Fetch Metadata where the captured browsers
  sent it. The capture's cross-site fetch from `localhost` to
  `127.0.0.1` is `cross-site` in yonder too.
- **`Accept` is not copied.** Chrome's navigation `Accept` names JPEG XL,
  AVIF, APNG and signed exchanges, and its `Accept-Encoding` is `gzip,
  deflate, br, zstd`; yonder keeps its own lists, which name only what it
  decodes (DIVERGENCES, "The browser's identity").
- **A font is asked for as `no-cors`.** Browsers fetch fonts in cors mode,
  and yonder runs no CORS check on one, so it does not say it does.
- **`Priority` (RFC 9218) is the preset's, per destination.** Firefox sends
  one on every request: `u=0, i` for a document, `u=2` for a style or a
  script, `u=5, i` for an image, `u=4` for a fetch and its preflight
  (capture-19..27); no font was asked for, so a font goes without. Chrome
  sends none over HTTP/1.1, and Safari's is not captured. It describes the
  request's urgency, not a capability, so it is no header that lies.
- **yonder's own preset sends `Accept-Language: en-US,en;q=0.9`**, a real
  browser asking for English (a server that hears none guesses a locale);
  Netscape 4 and Lynx keep their strings alone, and a typed agent claims
  nothing beyond itself.
- **libfetch's per-hop callback takes the lines, not the extra-headers
  block.** The extra headers are fixed for a whole fetch, and these lines
  are not: a typed `http://` address that redirects to `https://` must
  send nothing of Fetch Metadata on the first hop and all of it on the
  second, and `Sec-Fetch-Site` grows with every URL a redirect adds.
  `headers_for` accepts the ten names (Accept-Language, the three
  `sec-ch-ua` lines, the four `Sec-Fetch-` lines,
  `Upgrade-Insecure-Requests`, `Priority`) beside Cookie and Referer, each
  once; none is a credential. `OS64_FETCH_HOP_FIELDS_MAX` holds 512 bytes
  for them beside the cookies' kilobyte (`WAY_IDENTITY_MAX`; Chrome's
  longest block is 294 bytes); `OS64_FETCH_EXTRA_MAX` is not involved.
- **Header order is libfetch's, not each browser's**, booked in
  BROWSER_DEBTS beside § 2, whose change to the `Connection` line touches
  the same request writer.

## 2. Keep-alive: one connection carries many requests

**The problem.** libfetch speaks `Connection: close` (`http.h`'s own
comment says keep-alive is not spoken), so every script, sheet, picture
and font is a new TCP connection and, on https, a new TLS handshake. A
page with 172 sub-resources paid 172 handshakes, serially where the
parser blocked on each script, and the P5 spent a minute mostly waiting.
This is the largest speed-up yonder has left, and the one Chris sees as
"unresponsive, then paints fine" on every big site.

**The design.** A connection pool inside libfetch, below the fetch
object, invisible to callers:

- **Key**: scheme, host, port, the proxy in use, and the TLS
  configuration's identity (a connection made with one trust store or
  one client setting is never reused under another). IPv6 literals and
  ports are part of the host:port as the URL grammar gives them.
- **Capacity**: at most 6 idle connections per key (Chrome's per-host
  figure) and 32 in all, least recently used evicted first; a connection
  idle longer than 60 s is closed when next seen (servers close sooner,
  which the retry rule below absorbs). The numbers are two constants in
  one place with the reasoning beside them.
- **When a connection may be kept (RFC 9112 § 9.3, every clause):** the
  request and the response were both HTTP/1.1; neither carried
  `Connection: close`; the response was framed (a `Content-Length` read
  to its end, a chunked body read through its last-chunk and trailers,
  or a bodiless status: HEAD, 204, 304, 1xx handled); and the caller
  consumed the whole body. A response the caller abandons mid-body, a
  framing refusal, a transport error, a TLS failure, or a response with
  no framing (read-to-close) closes the connection, never pools it. The
  decision is one function, `connection_reusable`, pure over the
  response's facts, so a case can hold every clause.
- **The retry rule (RFC 9110 § 9.2.2):** a request sent on a reused
  connection that fails BEFORE any byte of the response arrived (the
  server closed its idle side) is retried once on a fresh connection,
  and only when the method is idempotent (GET, HEAD, OPTIONS, PUT,
  DELETE); a POST is not retried, and the failure is the caller's as
  today. A request on a fresh connection is never retried. The retry is
  invisible to the caller except as time, and the fetch's reason string
  says "retried once on a fresh connection" when it happened, so a
  record can carry it.
- **Ownership and threads:** yonder's jobs fetch from a pool of threads,
  so the pool has one lock, held only to take or put a connection; a
  connection belongs to exactly one fetch between take and put, and a
  fetch closed by its owner mid-use (cancel, the window's stop) closes
  the connection rather than returning it. The idle deadline per read
  (`FETCH_IDLE_MS_DEFAULT`, 30 s) is unchanged: it is the read's
  patience, not the connection's life.
- **Proxies:** through a plain proxy the connection is to the proxy, keyed
  by it, and reused for any host the proxy serves (RFC 9112 § 9.3
  applies to the hop); `Proxy-Connection` is not sent. TLS through a
  proxy (`CONNECT`) is keyed by the tunnelled host as well and reused
  only for it.
- **Not in this PR:** TLS session resumption (tickets are consumed and
  discarded today, DEBTS § TLS 1.3), which would cut the first-connection
  cost further; HTTP/2; pipelining (nobody speaks it). Each stays a row,
  and the first is the next worth paying once this lands.

**What stops sending.** `Connection: close` leaves the fixed block; the
request writer sends nothing for `Connection` on HTTP/1.1 (persistent is
the default). A caller that wants a one-shot connection (none today)
would ask through the options, not by header.

**Proof.** libfetch host cases (`tools/test_fetch_host.c`), each with a
mutant: reuse after a Content-Length body; after a chunked body with
trailers; after HEAD and 204 and 304; no reuse after `Connection:
close` from either side; after an abandoned body; after a framing
refusal; after a TLS failure; the idle expiry; the per-key and total
caps evicting the least recent; the retry once for GET on a connection
the fake server closed, the failure for POST, no retry on a fresh
connection, and the reason string; two threads taking from one key
never sharing a connection (the sweep under the sanitizers). QEMU `make
run-net`: a page of many same-host resources, the slirp side showing one
connection carrying them. The measurement that justifies it: the kept
record's `arrived after` on the storefront page before and after, in the
PR body, with the connection count the wire showed; the expectation is
seconds where there were sixty.

## 3. Order and ownership

PR 1 (identity) then PR 2 (keep-alive), before the next Modern Web slice,
because Google is where a search starts and the second is what every big
site waits on. Each is a builder's with the three passes; Fable reads the
pool's locking and the retry rule in PR 2 before Codex (concurrency and
a rule that re-sends a request are the two places a wrong answer hides).
Rows that result (brotli, HTTP/2, the fingerprint, session resumption)
land in DEBTS.md and DIVERGENCES.md with their PR.
