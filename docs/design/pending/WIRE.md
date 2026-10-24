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

### W1, as built (2026-10-24 and -25, Opus)

The values come from a capture, not from this brief: Chris's Chrome 156
and Firefox 158, each making a typed navigation, a clicked one, a script,
a sheet, a picture, a same-origin fetch, a cross-site fetch and its
preflight to a local listener, then the browser's Reload and Back (raw
request bytes, one file a request, cited in the PR). Chrome on Windows is
`capture-01..08-chrome-win-*` (the sequence) with
`capture-25-chrome-win-get-clicked` (the click),
`capture-26-chrome-win-get-clicked` (Reload) and
`capture-27-chrome-win-get-root` (Back); Firefox on Windows is the first
run's `run1-overwritten/capture-09` (the typed navigation), `-02..08` and
`-17` (the click); Firefox's Reload and Back are
`capture-43-firefox-linux-get-clicked` and
`capture-44-firefox-linux-get-root`, which behave as on Windows. Where a
capture or a standard disagreed with § 1 above, the build follows them:

- **Chrome is 156, and its brand list puts the GREASE brand first:**
  `"Not:A-Brand";v="8", "Chromium";v="156", "Google Chrome";v="156"`
  (capture-01-chrome-win-get-root). The agent string is the reduced
  `Chrome/156.0.0.0`. Firefox is `rv:158.0` / `Firefox/158.0`
  (run1-overwritten/capture-09). Safari is not captured: no client hints,
  and the language list MDN gives.
- **A preflight carries no client hints**
  (capture-07-chrome-win-options-fetch-cross-preflight); it does carry
  `Accept-Language`, and its Fetch Metadata is cors/empty with the
  request's site.
- **`Sec-Fetch-User` stays on every hop of a person's navigation.** HTML's
  "create navigation params by fetching" sets the request's
  user-activation once, from the source's transient activation, and its
  redirects are the same request; Fetch Metadata § 2.4 reads only that
  flag. So a script's `click()`, `submit()` or `location=` run inside a
  person's gesture carries `?1` in HTML and the browsers; yonder tracks
  no transient activation, and every navigation a page's script asks for
  (a refresh, `location`, `location.reload()`, `history.go()`, `click()`,
  `submit()`) is nobody's, on every hop: it under-claims (DIVERGENCES;
  transient activation is booked in BROWSER_DEBTS).
- **The browser's Reload and Back replay the entry's own asking.** Both
  browsers' Reload of the clicked link sends its `same-origin` and `?1`
  again (capture-26-chrome-win-get-clicked,
  capture-43-firefox-linux-get-clicked), and Back to the typed entry its
  `none` and `?1` (capture-27-chrome-win-get-root,
  capture-44-firefox-linux-get-root). Each history entry keeps how its
  document's navigation was asked (`way_crumb_t.asked`), and the toolbar's
  Reload, Back and Forward replay it; a page's own reload or traversal is
  the page's. Chrome leads its Reload with `Cache-Control: max-age=0`
  (capture-26), and its preset does; Firefox adds nothing. Both send the
  entry's Referer again on Reload, which yonder does not yet
  (BROWSER_DEBTS).
- **A site nobody can name says nothing.** Without the public-suffix list
  two hosts that share their last two labels (a site's sibling
  subdomains, or two sites under one public suffix) may be one site or
  two, and calling a site's own CDN `cross-site` is what a
  resource-isolation policy refuses. `way_origin_site` says only what is
  certain (`same-origin`; `same-site` for one host on two ports;
  `cross-site` for another scheme, an IP literal, or hosts whose last two
  labels differ; the round's first ruling had another scheme on one host
  as same-site, and was corrected, because HTML's "same site" is
  schemeful and Fetch Metadata uses it) and a request whose site it
  cannot know sends none of the four `Sec-Fetch-` lines, as a browser from
  before Fetch Metadata would. In the kept corpus that is 11-21% of a pull's https scripts and
  sheets (271 of 2420, 470 of 3375, 490 of 2326). The omission ends when
  the public-suffix list lands (DEBTS).
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
  decodes (DIVERGENCES, "The browser's identity"). (W2 replaced this: each
  browser's own `Accept` less what yonder cannot decode, § 2 as built.)
- **A font is asked for as `no-cors`.** Browsers fetch fonts in cors mode,
  and yonder runs no CORS check on one, so it does not say it does.
- **`Priority` (RFC 9218) is the preset's, per destination.** Firefox sends
  one on every request: `u=0, i` for a document, `u=2` for a style or a
  script, `u=5, i` for an image, `u=4` for a fetch and its preflight
  (run1-overwritten/capture-09, -17, -02..08); no font was asked for, so a
  font goes without. Chrome sends none over HTTP/1.1, and Safari's is not
  captured. It describes the request's urgency, not a capability, so it is
  no header that lies. A page's own `Priority` (or `Accept-Language`) on a
  fetch stands alone: the preset's beside it would be a duplicate.
- **yonder's own preset sends `Accept-Language: en-US,en;q=0.9`**, a real
  browser asking for English (a server that hears none guesses a locale);
  Netscape 4 and Lynx keep their strings alone, and a typed agent claims
  nothing beyond itself. A default saved as an earlier yonder's preset
  string (Chrome 154, Firefox 156) is read as that preset's current one.
- **libfetch's per-hop callback takes the lines, not the extra-headers
  block.** The extra headers are fixed for a whole fetch, and these lines
  are not: a typed `http://` address that redirects to `https://` must
  send nothing of Fetch Metadata on the first hop and all of it on the
  second, and `Sec-Fetch-Site` grows with every URL a redirect adds.
  `headers_for` accepts eleven names (Accept-Language, the three
  `sec-ch-ua` lines, the four `Sec-Fetch-` lines,
  `Upgrade-Insecure-Requests`, `Priority`, a Reload's `Cache-Control`)
  beside Cookie and Referer, each once; none is a credential.
  `OS64_FETCH_HOP_FIELDS_MAX` holds 512 bytes for them beside the cookies'
  kilobyte (`WAY_IDENTITY_MAX`; Chrome's longest block, its Reload of a
  clicked link, is about 320 bytes); `OS64_FETCH_EXTRA_MAX` is not
  involved.
- **Header order is libfetch's, not each browser's**, booked in
  BROWSER_DEBTS beside § 2, whose change to the `Connection` line touches
  the same request writer. (Paid by W2: each preset's order per kind of
  request, § 2 as built.)

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
would ask through the options, not by header. (As built, the browser
presets send `Connection: keep-alive`, as their browsers do: § 2 as
built.)

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

### W2, as built (2026-10-24, Opus)

The pool is `userland/libfetch/pool.c`, its rule `connection_reusable`
there too; fetch.c takes before it dials and puts back at close (and at a
redirect it follows itself). Where the build departs from the brief, or
says more than it did:

- **Sixteen kept in all, not thirty-two.** Every kept connection holds one
  of a task's 64 handles (`OS64_TASK_HANDLES`), and thirty-two kept beside
  a dozen in use was most of a browser's table before it opened a file
  (the cold read's finding). The pool keeps at most a quarter of the table,
  asserted at build; and a dial the kernel refuses for want of resources
  lets the least recently kept connection go and dials once more.
- **One clause more: the request went whole.** A server that answers
  before an upload is complete (a 401 or a 413 to a POST) has stopped
  reading a body that never ends, and its connection is not kept
  (`request_whole`). The brief's list is RFC 9112 § 9.3's; this one is its
  § 9.5's consequence.
- **A 205 is not bodiless on the wire.** libfetch hands a 205's body to
  nobody (RFC 9110 § 15.3.6), but RFC 9112 § 6.3 frames it like any reply,
  so one whose head promised bytes is not kept; the bodiless three are a
  HEAD's answer, a 204 and a 304.
- **A kept connection is probed when it is taken**: a zero-wait read on a
  plain one, a zero-wait step on a TLS one. A server that closed its idle
  side, or one that said anything nobody asked for, is closed then and
  another taken or dialled. The retry rule is for the close that lands
  between the probe and the request.
- **Nothing past the message.** Bytes already read past a reply's end, a
  plain lookahead byte, or TLS plaintext waiting are a reply nobody asked
  for, and the connection goes (`leftover`); one that arrives later is the
  probe's to find. Either way only a later request on the same key could
  be confused by it. Direct, that is the same server, which answers its own
  requests anyway; through a proxy the key is the proxy's, and a request to
  ANOTHER origin may follow on the connection, so bytes past a reply there
  would be the proxy's error (it frames every reply it relays), which the
  leftover check and the probe exist to catch, not a trust any origin is
  given.
- **One trust store per connection, and per process.** A connection is
  keyed by the store it was verified under, by pointer, which a kept TLS
  client's own reference makes an identity. A fetch whose caller gives no
  store used to read the system's for itself, every fetch; it now shares
  the one libfetch reads on the first https hop and keeps for the process
  (`fetch_pool_trust`, the cold read's tenth finding), so every such
  connection is verified under the same store and keyed by it, and the
  roots are parsed once a program rather than once a request. An edit to
  the store reaches the programs started after it (DEBTS § libfetch).
- **The retry rule is per request**: each request a fetch makes (a
  redirect's next hop is another) goes at most twice, and the reason
  string counts them: "(retried once on a fresh connection)", or "(2
  requests retried, each once on a fresh connection)". A silence is not a
  closed connection, and a reply that began is not retried — except a 408
  on a kept connection, which is the server's idle timeout said in words:
  an idempotent request goes again on a fresh connection as Chrome sends
  it, and nothing of the 408's head reaches an observer (its fields, its
  cookies), since the answer the caller gets is the retry's.
- **Through a proxy**: libfetch sends https through a plain proxy in
  absolute form, not by `CONNECT`, so every proxied connection is plain to
  the proxy and keyed by it alone; the brief's `CONNECT` clause has nothing
  to apply to.
- **Concurrency is not capped.** The pool keeps at most six IDLE
  connections to a key; it does not make a seventh fetch to a busy host
  wait for one, as Chrome's six-per-host limit does. A page that asks for
  a dozen sheets at once opens a dozen connections, which then carry the
  rest of the page between them (DEBTS § libfetch).

**The `Connection` line (Chris and Opus, after the P5 capture).** The
brief said to send nothing for `Connection` on HTTP/1.1. Chrome 156 and
Firefox 158 both send `Connection: keep-alive` (capture-01..08, 25..27;
run1-overwritten/capture-01..09 and 17), so each browser preset says it,
where its browser does. yonder's own says it too: it keeps every
connection it can now, and the line says what it does. Netscape 4 and
Lynx, the costumes, and an agent typed claim nothing beyond their
strings. `Connection: close` is never sent: libfetch's option is
`keep_alive`, and a caller that wanted one-shot connections would ask
through the options, not the line.

**`Accept` per destination (the same ruling).** Each browser preset sends
its browser's captured `Accept` for a navigation, a sheet, a script, a
picture and a fetch, LESS the types yonder cannot decode. The minus is
JPEG XL, AVIF, SVG as a picture and signed exchanges, and also **WebP and
APNG**: the ruling named Chrome's navigation list with `image/webp` kept
and asked that WebP be verified first. It was: libimage decodes WebP and
APNG STILLS, by signature, and neither ANIMATION (an animated WebP is
refused as unsupported; an APNG draws its default image, still), and WEBP.md
§ Accept keeps `image/webp` out of every Accept until animation ships,
because naming it makes a server that would send a working GIF send an
animated WebP instead. So Chrome's navigation asks
`text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8`, its
pictures `image/*,*/*;q=0.8`; Firefox's navigation is as captured and its
pictures `image/png,image/*;q=0.8,*/*;q=0.5`. Wildcards stay: they are
the browser's "anything else", not a type. No font was captured, so a
font keeps yonder's list. Safari's navigation list is MDN's for it.
yonder's own asks `text/html,application/xhtml+xml,text/*;q=0.9,*/*;q=0.8`
for a page (its other destinations' lists already named only what it
decodes); a costume keeps the session's list and its pictures. The one
table is `agent.c`, beside the presets.

**Header ORDER per preset** (booked by W1 in BROWSER_DEBTS, paid here).
libfetch writes Host first, then the lines the caller's order names
(`os64_fetch_options_t.order`, comma-separated names), each where the
order puts it, then every line it does not name in libfetch's own order.
`way_order_of` says which kind a request is (a navigation, a no-cors
request, a cors one, a preflight) and the preset carries an order for
each: Chrome's navigation, its every other request (no-cors and cors
alike, capture-02..06 and -08) and its preflight; Firefox's navigation,
no-cors, cors and preflight, each its own. Captured requests match byte
for byte but for the two values yonder says differently
(`Accept-Encoding`, and `Accept` as above); a page's own line (the
captures' `X-W1`) goes after the named lines, where both browsers put
it among them. A Cookie, which no capture carried, goes there too.

**On the wire (QEMU, `make run-net`'s virtio NIC and dump, 2026-10-24).**
A local page of twelve sheets, twelve scripts and twelve pictures from
one host, served over plain HTTP by a listener that logs each request's
connection: under yonder's own preset with scripts off, ONE connection
carried all 25 requests, and the slirp side's dump shows one SYN to it.
Under the Chrome preset, scripts on, one connection carried 30 of the
page's 37 requests and five more the parallel rest; the logged heads are
Chrome's order (`Host`, `Connection: keep-alive`,
`Upgrade-Insecure-Requests`, `User-Agent`, `Accept`, `Referer`,
`Accept-Encoding`, `Accept-Language`; plain http to a private address
carries no Fetch Metadata or hints, so those are absent, not misplaced).

**The measurement.** The P5 is where a page waits on its network; QEMU
under TCG (no KVM here) mostly waits on its own CPU, so its clocks say
less than its connection counts do. Each figure is a fresh VM, the
Chrome preset, scripts on, the kept record's `arrived after`, and the
SYNs the dump shows to the page's host. Base is this PR's parent:

| Page | Base | Keep-alive |
|---|---|---|
| the local page above | 1311, 1350 ms; 37 connections | 867, 947 ms; 5 and 6 |
| a blog page from the corpus, 13 sheets and 13 pictures | 2814, 2781, 2784, 2765 ms; 79-80 connections | 3646, 2838, 2854, 3036 ms; 21-22 |
| a course page from the corpus, 38 same-host scripts | 23637, 20866 ms; 64-65 connections | 23834, 23115 ms; 12-13 |

Connections, and with them TLS handshakes, fall to a fifth or a quarter
of what they were on the corpus pages and a sixth on the local one. Only
the local page's arrival follows them down. The blog page's
arrival waits on its sheets, which it asks for at once, so each one
dials either way (the uncapped concurrency above); the course page's is
QuickJS under TCG. The storefront's sixty seconds on the P5 is the
measurement this table cannot stand in for.

## 3. Order and ownership

PR 1 (identity) then PR 2 (keep-alive), before the next Modern Web slice,
because Google is where a search starts and the second is what every big
site waits on. Each is a builder's with the three passes; Fable reads the
pool's locking and the retry rule in PR 2 before Codex (concurrency and
a rule that re-sends a request are the two places a wrong answer hides).
Rows that result (brotli, HTTP/2, the fingerprint, session resumption)
land in DEBTS.md and DIVERGENCES.md with their PR.
