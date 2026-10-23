# Origins and CORS (Slice 2)

How yonder judges a script's request by the Fetch standard: what an
origin is, when a request is preflighted, what goes with it, what comes
back to the page. Written by the Slice 2 builder before any code, read and
approved by Fable with eight rulings and six requirements, and kept as
built: every **RULED:** below is a ruling folded in where its question was,
and MODERN_WEB.md's `### Slice 2, as built` is the record of what the slice
built around it. The 151 host cases at the end are the contract the code
is held to (`tools/test_way_origin_host.c`, one case each).

**The standard read.** The Fetch Living Standard as published at
fetch.spec.whatwg.org, "Last Updated 19 October 2026", and the XMLHttpRequest
Living Standard of the same date, read whole in the sections cited. **Section
numbers moved:** the Slice 2 brief's "§ 3.2 (CORS protocol)" is § 3.3 today;
§ 3.2 is now the `Origin` header. Every citation below is to that day's
numbering. Where this design departs from the standard on purpose it says so
with **DIVERGES:** and why; each such departure is a DIVERGENCES.md row.

**The one-sentence shape.** The page's request is judged HOP BY HOP by pure
functions in `libway/origin.c`, called by the fetch job, which drives the
redirect loop itself (libfetch is told to stop at every redirect), so that
everything the standard decides per hop (tainting, the preflight, the
`Origin` header, whether cookies go, whether `Authorization` survives,
whether `Set-Cookie` is heard, the CORS check) is decided once per hop, in
one place, for `fetch`, asynchronous and synchronous XHR alike. libdom never
sees a response it may not expose: the job hands up a FILTERED response,
and a network error carries nothing at all.

---

## The rulings (Fable, 2026-10-22)

1. **Same site = schemefully same host**, until the public-suffix list
   lands. The stand-in is finer than the real partition, so every answer
   it changes is the restrictive one. DEBTS.md row, trigger: a redirect
   chain between sibling subdomains of one acceptance site answers `null`
   where Chrome answers the origin.
2. **The job drives the redirect loop, and the standard's credential and
   `Authorization` rule replaces libfetch's inside it.** libfetch's
   typed-origin rule never fires inside a fetch job, because each hop is
   typed afresh (a method by name stops libfetch at every redirect);
   libfetch keeps its own rule for its other callers.
3. **Every `https` to `http` redirect of a script request is a network
   error.** DIVERGENCES.md row.
4. **SameSite: the jar's debt is inherited**, not filtered here: Lax and
   Strict cookies ride a cross-site script request under `include`, no more
   than a scripted `form.submit()` sends today, and the response stays
   unreadable without the server's `Allow-Credentials`. DEBTS.md row, paid
   with the public-suffix list. The cookie choice is one function
   (`fetch_job.c`'s `hop_cookies`).
5. **fetch and XHR bypass the HTTP cache.** The cache keeps no response
   headers and no credentials partition, so it cannot answer a CORS
   question. DEBTS.md row, trigger: the P5 shows a page re-fetching the
   same API answer per navigation.
6. **An unreadable CORS header line is judged the restrictive way.**
7. **A malformed `Access-Control-Expose-Headers` exposes nothing extra.**
8. **One copy of the lists: libdom links libway**, and Slice 1a's
   `d_origin` is `way_origin_serialize` (Slice 2 landed second), so
   `location.origin`, the storage provider's origin and the job's are one
   string; a DOM host case holds them to it.

The stated divergences, accepted, each a DIVERGENCES.md row: `User-Agent`
forbidden, `*` preflight-cache entries never credentialed, `max-age` capped
at 7200 s, `localhost` not potentially trustworthy, the `https` to `http`
refusal. (No `Origin` on a `HEAD` that is not cors-tainted is the
standard's own rule, § 11, not a divergence.)

**The six requirements**, and where each stands (MODERN_WEB.md's as-built
says more): A, a `data:` body is capped by the same `max_body` as a network
one; B, a body crossing into the engine counts against the runtime's memory
limit (the sticky LIMIT, with a case); C, "hear `Set-Cookie`, then
CORS-check" pinned by a case; D, libfetch's growth has its own host cases
and a QEMU `make run-net` fetch of a real cross-origin resource; E, this
file; F, every row named above.

**Changed while building**, against the design as read:
- § 1: `way_origin_of` gives a tuple for every special scheme but
  file (ftp, http, https, ws, wss), as the URL Standard does and as 1a's
  `d_origin` did, so the one serializer serves `location.origin` and
  storage too; fetch still refuses every scheme but http, https and data.
- § 6: a new refusal, WAY_REFUSE_URL, for an http(s) URL os64's URL
  grammar refuses (userinfo, IPv6 literals, port 0): checked first, and
  `way_port_bad` answers "bad" for a port it cannot read.
- § 1: out of memory computing an origin reads as opaque.
- § 4, § 10: a server's Access-Control-Allow-Methods, -Allow-Headers
  or -Expose-Headers past WAY_CORS_LIST_MAX (1024) elements is judged as a
  list that does not parse (no grant, nothing exposed), so every walk over
  it is bounded; lists are read once, sorted, searched (n log n). No script
  budget interrupts C, so the bound is the code's.
- § 5: grants of one cache key share a number found once per call.
- libfetch: `method_name` (any token; the library then follows no
  redirect, REDIRECT_STOPPED), `body_headers`, `identity`, `on_header`;
  HEAD/205 bodiless; OS64_FETCH_EXTRA_MAX 8192 with the per-hop buffers
  moved into the fetch object.

---

## 1. What an origin is here, and when it is opaque

(HTML § origin; URL § origin; Fetch § 2.1, § 3.2)

- An origin is either a TUPLE (scheme, host, port) or OPAQUE.
- `way_origin_of(url)` gives a TUPLE for `http` and `https` URLs that
  `os64_url_parse` accepts: the scheme and host lowercased (the parser already
  folds both), the port the URL named or the scheme's default when it named
  none (80, 443). Comparison uses the EFFECTIVE port, so `http://a` and
  `http://a:80` are one origin.
- `blob:` URLs: the inner URL's origin when that is `http`/`https`, else opaque
  (URL § origin). There is no Blob URL store in yonder, so a `blob:` URL is
  never FETCHED (§ 13); the rule matters only for the origin computation.
- **Opaque** for everything else: `file:`, `about:`, `data:`, `javascript:`,
  an unparseable URL, and any scheme we do not fetch (`ftp:`, `ws:` — no
  WebSocket in this slice). An opaque origin is same-origin ONLY with itself
  (HTML § same origin: "A and B are both opaque origins and A is B"): each
  opaque origin carries a 64-bit identity drawn at creation, and two opaque
  origins compare equal only when the identities match.
- **The page's origin is computed ONCE**, from the document's final URL (the
  address the body came from after redirects, `way_head_t.url`, which is what
  `yonder_scripts_options_t.url` already carries), when the script host is
  made, and stored. It is NEVER recomputed from `location` or from anything a
  script can write. (`history.pushState` in slice 1b changes the URL, and only
  within the origin; computing the origin once means a bug there can never
  move a page's origin.) `document.domain` is not supported, so nothing
  changes it.
- **Serialization** (HTML § serialization of an origin; Fetch § 3.2 ABNF):
  opaque → `null`; tuple → `scheme "://" host [":" port]`, the port written
  only when it is not the scheme's default; no trailing slash; lowercase.
  IPv6 literals and userinfo never reach here (the URL parser refuses them).
  Non-ASCII hosts are refused by the parser too (no IDNA), so a serialized
  origin is always ASCII.
- **A page loaded from `file:`** therefore has an opaque origin: every fetch it
  makes to `http(s)` is cross-origin, goes out in `cors` mode with
  `Origin: null`, and is readable only when the server answers
  `Access-Control-Allow-Origin: *` or `null` (Chrome's behaviour too).
- **"Same site"** (HTML § same site) needs the public-suffix list, which os64
  does not have (MODERN_WEB.md Tier 1 books it). Where the standard asks
  "same site" (the redirect-taint, § 7; the preflight cache's partition key,
  § 5) this design answers **schemefully same HOST**: same scheme and same
  host, ports ignored. That partition is FINER than the real one (two
  subdomains of one site count as two sites), so every answer it changes is
  the restrictive one: more `Origin: null`, fewer cache hits. Never the other
  way: the naive "last two labels" rule would call `evil.co.uk` and
  `bank.co.uk` one site, and is rejected.
  **RULED (1):** an acceptable stand-in until the PSL lands; it costs
  compatibility only in redirect chains that hop between sibling subdomains.

## 2. Same origin

(HTML § same origin)

`way_origin_same(a, b)`: both opaque with the same identity, or both tuples
with identical scheme, identical host and identical effective port. Byte
comparison: the parser has already lowercased scheme and host. Used for:
the main fetch's tainting choice (§ 6), `same-origin` mode, credentials mode
`same-origin` (§ 8), the `Authorization` strip on redirect (§ 8), the CORS
check's comparison is NOT this (it compares serializations, § 9).

## 3. The safelisted methods and request headers

All in `origin.c`, as pure predicates over (name, value) bytes; libdom's
`Headers` guards and the job both call them (§ 14: one copy).

- **CORS-safelisted method** (§ 2.2.1): exactly `GET`, `HEAD`, `POST`,
  compared CASE-SENSITIVELY after normalization (`normalize a method`
  upper-cases only `DELETE GET HEAD OPTIONS POST PUT`, so `patch` stays
  `patch` and is not safelisted; `post` normalizes to `POST`).
- **Forbidden method** (§ 2.2.1): `CONNECT`, `TRACE`, `TRACK`, case-
  insensitively. `Request`/`fetch`/`XHR.open` throw `TypeError`/
  `SecurityError` (XHR § open step) before any job exists; the job refuses one
  again (defence: the job is the transport and trusts no caller).
- **CORS-unsafe request-header byte** (§ 2.2.2): a byte < 0x20 other than
  HT, or one of `" ( ) : < > ? @ [ \ ] { }` or 0x7F.
- **CORS-safelisted request-header** (§ 2.2.2), judged per header ENTRY, not
  per combined value:
  - the value is at most **128 bytes**, else not safelisted;
  - `accept`: no CORS-unsafe byte;
  - `accept-language`, `content-language`: only `0-9 A-Z a-z`, SP, `* , - . ; =`;
  - `content-type`: no CORS-unsafe byte, AND the value PARSES as a MIME type
    (MIME Sniffing § parse a MIME type — the strict parser, NOT "extract a MIME
    type", as the standard's own JSON-smuggling example explains) whose essence
    is exactly one of `application/x-www-form-urlencoded`,
    `multipart/form-data`, `text/plain`;
  - `range`: parses as a SINGLE range (`bytes=N-` or `bytes=N-M`, § 2.2.2
    "parse a single range header value", allowWhitespace false) whose start
    is present (`bytes=-500` is not safelisted);
  - every other name: not safelisted.
- **CORS-unsafe request-header names** (§ 2.2.2): every entry that is not
  safelisted, PLUS — when the safelisted entries' values total more than
  **1024 bytes** — every safelisted name too; the result byte-lowercased,
  de-duplicated and sorted bytewise.
- **CORS non-wildcard request-header name** (§ 2.2.2): `Authorization`.
- **No-CORS-safelisted request-header** (§ 2.2.2): name is `Accept`,
  `Accept-Language`, `Content-Language` or `Content-Type` AND the header is
  CORS-safelisted. **Privileged no-CORS request-header name**: `Range`.
- **Forbidden request-header** (§ 2.2.2), silently dropped wherever a page
  sets one (`Headers` with guard `request`/`request-no-cors`, XHR's
  `setRequestHeader`) and dropped again by the job: `Accept-Charset`,
  `Accept-Encoding`, `Access-Control-Request-Headers`,
  `Access-Control-Request-Method`, `Connection`, `Content-Length`, `Cookie`,
  `Cookie2`, `Date`, `DNT`, `Expect`, `Host`, `Keep-Alive`, `Origin`,
  `Referer`, `Set-Cookie`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`,
  `Via`; any name starting `proxy-` or `sec-`; and `X-HTTP-Method`,
  `X-HTTP-Method-Override`, `X-Method-Override` when any of their
  comma-split values (§ 2.2.2 "get, decode, and split") is a forbidden method.
  **DIVERGES:** `User-Agent` is treated as forbidden too (dropped silently).
  The standard removed it from the list; Chrome still drops it, and libfetch
  sends the browser's own. A page's `User-Agent` reaching the wire would also
  be a CORS-unsafe header and force a preflight nobody expected.
- **Forbidden response-header name** (§ 2.2.2): `Set-Cookie`, `Set-Cookie2`.
- **CORS-safelisted response-header name** given a list L (§ 2.2.2):
  `Cache-Control`, `Content-Language`, `Content-Length`, `Content-Type`,
  `Expires`, `Last-Modified`, `Pragma`, and any item of L that is not a
  forbidden response-header name.

## 4. When a preflight is needed, its shape, and its judgement

**When** (§ 4.1 main fetch, the "use-CORS-preflight flag / unsafe-request
flag" branch; § 4.4 HTTP fetch step "makeCORSPreflight"). Every script request
here has the unsafe-request flag set (`Request` constructor; XHR § send). Two
questions, in the standard's order:
1. Is this a PREFLIGHT-CAPABLE hop (§ 4.1)? Its tainting becomes `cors` (§ 6,
   so never a same-origin basic hop, never `no-cors`) AND either the request's
   use-CORS-preflight flag is set (XHR with an upload listener registered at
   `send()`, XHR § send), or its method is not CORS-safelisted, or its
   CORS-unsafe request-header names are non-empty.
2. If so, is a preflight actually SENT (§ 4.4)? Yes when either (a) there is
   no method cache entry match (§ 5: the method or `*`) AND (the method is not
   CORS-safelisted OR the use-CORS-preflight flag is set), or (b) at least one
   CORS-unsafe name has no header-name cache entry match. Otherwise the cache
   answers and the request goes straight out.
The decision is remade on EVERY hop
(§ 4.5's last step re-enters main fetch), with the hop's current method and
headers: a 303 that turned a `PUT` into a `GET` with no body headers may need
none; a same-origin request redirected cross-origin may need its first.

**Shape** (§ 4.8):
- method `OPTIONS`, to the hop's current URL;
- `Accept: */*`;
- `Access-Control-Request-Method: <the request's method>`;
- `Access-Control-Request-Headers: <unsafe names joined by "," with NO space>`
  only when there are any (§ 4.8 says so explicitly);
- `Origin: <serialized request origin>` (the preflight's tainting is `cors`,
  § 3.2's first branch), which is `null` when the request is redirect-tainted
  (§ 7);
- `Referer` per the request's referrer and policy, `User-Agent` the browser's;
- **NO credentials**: the preflight's credentials mode is the default
  `same-origin` with tainting `cors`, so includeCredentials is false (§ 4.6):
  no `Cookie`, and its `Set-Cookie` is NOT heard; no `Authorization` (the
  author headers are not copied to it); no body;
- it is NOT followed if it redirects: a preflight goes through
  HTTP-network-or-cache fetch, not HTTP fetch, so a 3xx is simply a non-ok
  status and fails the judgement.

**Judgement** (§ 4.8, in order; any failure is a network error for the whole
fetch, and clears the cache entries for (partition, origin, URL), § 4.1):
1. A CORS check (§ 9) with the ACTUAL request's credentials mode (so under
   `include` the preflight answer needs `Access-Control-Allow-Credentials:
   true` too) passes, AND the status is ok (200–299).
2. `methods` = extract header list values of `Access-Control-Allow-Methods`
   (`#method`), `headerNames` = of `Access-Control-Allow-Headers`
   (`#field-name`); either FAILURE (an element that is not a token) → fail.
3. `methods` null and use-CORS-preflight flag set → `methods` = « method ».
4. The method fails when it is not in `methods` (byte-exact: methods are
   case-sensitive), is not CORS-safelisted, and (credentials is `include` OR
   `methods` lacks `*`).
5. A CORS non-wildcard name (`Authorization`) present in the request's headers
   and not named (case-insensitively) in `headerNames` → fail, whatever `*`
   says.
6. Each CORS-unsafe name not named in `headerNames` fails when credentials is
   `include` OR `headerNames` lacks `*`.
7. `max-age` = extract `Access-Control-Max-Age` (`delta-seconds`, a single
   header: two lines are failure); failure or null → **5**; capped (§ 5).
8. Cache the methods and header names (§ 5), then send the actual request.

## 5. The preflight cache

(§ 4.9, and § 4.8's last steps)

- **Where:** libway, beside the jar: a `way_preflight_t` object the browser
  owns (one per yonder process, made at start like `g.way.jar`), carried to the
  job in its hooks like the jar is, with its own lock because jobs run on pool
  workers. Its logic is pure (`origin.c`, the clock injected), so the host
  tests drive expiry without waiting.
- **Key** (§ 4.9 cache entry): network partition key, byte-serialized request
  origin (which is `null` for a redirect-tainted request), the hop's URL
  (byte-exact, fragment removed), a `credentials` boolean (the request was
  `include`), and the method or header name.
  The partition key is the page's top-level site; yonder has no frames, so it
  is the page's own origin's schemeful host (§ 1's stand-in: finer, so it only
  costs hits).
- **Match** (§ 4.9): same partition, origin, URL, and (entry.credentials is
  true, or the request is not `include`); a method entry matches the method
  byte-exactly or is `*`; a header entry matches the name case-insensitively,
  or is `*` and the name is not `Authorization`.
  **DIVERGES (tightening):** a `*` method or header entry is stored with
  credentials FALSE whatever the preflight's mode, so it never answers for a
  credentialed request. The standard stores it with the preflight's
  credentials, and its match rule then lets a cached `*` from a credentialed
  preflight that ALSO named the method answer for a later credentialed request
  of a method the server never named, without a preflight — the very case § 4.8
  step 4 refuses when it is asked live. Costs one extra preflight at most.
- **Lifetime:** `max-age` seconds from storing, judged on the MONOTONIC clock
  (a wall-clock jump must not resurrect or kill entries). **Capped at 7200 s**
  (Chrome's two hours; the standard allows "an imposed limit"; Firefox uses a
  day — the shorter is the safer). `max-age: 0` stores nothing.
- **Size cap:** 256 entries (one entry = one (key, method-or-name)); past it
  the entry expiring soonest goes first (expired ones before all). The URL is
  stored at its own length, never in a fixed 8 KiB buffer.
- **Clearing:** a failed preflight, or a failed actual request that was
  preflighted (§ 4.1: "If corsWithPreflightResponse is a network error, then
  clear cache entries using request"), removes every entry for its (partition,
  origin, URL).

## 6. The hop: tainting, mode and the response type

The job runs § 4.1 main fetch on each hop, the "first matching statement"
read in order (current URL = this hop's URL; request origin = the page's,
§ 1; tainting starts `basic`):

1. **Blocked first** (§ 4.1 "should request be blocked…"): a bad port (§ 2.9's
   table, for `http`/`https` URLs: 0, 1, 7, 9, 11, 13, 15, 17, 19, 20, 21, 22,
   23, 25, 37, 42, 43, 53, 69, 77, 79, 87, 95, 101–104, 109, 110, 111, 113,
   115, 117, 119, 123, 135, 137, 139, 143, 161, 179, 389, 427, 465, 512–515,
   526, 530–532, 540, 548, 554, 556, 563, 587, 601, 636, 989, 990, 993, 995,
   1719, 1720, 1723, 2049, 3659, 4045, 4190, 5060, 5061, 6000, 6566,
   6665–6669, 6679, 6697, 10080) → network error; mixed content (§ 11) →
   network error.
2. **Same origin and still basic**: the hop's URL is same-origin with the
   request origin AND tainting is `basic` → tainting stays `basic`; no CORS,
   no preflight. (Once tainting has left `basic` it never returns: a chain
   A → B → A ends `cors`, § 4.1's condition.)
3. **`data:`** → `basic`, answered without the wire (§ 4.3 scheme fetch,
   § 6 data: URLs), status 200 `OK`, `Content-Type` from the URL.
4. (`navigate`/`websocket` modes do not exist for scripts; `Request` refuses
   `navigate`.)
5. **mode `same-origin`** → network error (before anything is sent).
6. **mode `no-cors`**: redirect mode not `follow` → network error; else
   tainting `opaque`, request sent with no CORS.
7. **Not `http`/`https`** → network error (`file:`, `about:`, `blob:` and the
   rest: § 13).
8. **mode `cors`**: tainting `cors`; preflight first if § 4 says so; then the
   request.

After the response head (§ 4.4 HTTP fetch, in order):
- tainting `cors` → the CORS check (§ 9) on THIS response, redirects
  included (a cross-origin 302 must carry `Access-Control-Allow-Origin` too);
  failure → network error, and nothing of the body is read.
- a redirect status (301, 302, 303, 307, 308) with a `Location`:
  redirect mode `error` → network error; `manual` → an `opaqueredirect`
  response; `follow` → § 8's redirect steps, then the next hop.
  A redirect status with NO `Location` is a final response like any other
  (§ 4.5 "If locationURL is null, then return response").

**The response types and what each exposes** (§ 2.2.6 filtered responses,
§ 4.1 main fetch's last steps). The JOB builds the filtered response; libdom
receives only it.

| type | when | status / statusText | headers | body | url / redirected |
|---|---|---|---|---|---|
| `basic` | tainting `basic` at the last hop | as sent | all but `Set-Cookie`, `Set-Cookie2` | as sent | last URL / URL list > 1 |
| `cors` | tainting `cors` | as sent | only the CORS-safelisted response-header names given the exposed list (§ 10) | as sent | last URL / URL list > 1 |
| `opaque` | tainting `opaque` | 0 / "" | none | none (null) | "" / false (URL list « ») |
| `opaqueredirect` | redirect mode `manual` met a redirect | 0 / "" | none | none (null) | the URL that answered with the redirect / false |
| `error` | any network error | 0 / "" | none | none | "" / false |

- `fetch()` turns `error` into a `TypeError` rejection; `Response.error()`
  makes one directly. XHR turns it into its `error` event (status 0).
- Body null also for a null body status (101, 103, 204, 205, 304) and for a
  `HEAD` request (§ 4.1), whatever the server sent.
- **An `opaque` or `opaqueredirect` response's body is never READ by the job**:
  the head is read (the request has had its effect, and `Set-Cookie` is heard
  if credentials were included, § 8), then the fetch is closed. What was never
  read cannot leak through a bug further up.
- `response.url` is the last URL serialized without its fragment (§ 5.5 url
  getter). `opaqueredirect` keeps its URL list (§ 2.2.6: "Exposing the URL
  list for opaque-redirect filtered responses is harmless").

## 7. The redirect-taint and the serialized request origin

(§ 2.2.5 "compute the redirect-taint", "serializing a request origin")

The job keeps the URL list. Over it, with the request origin O:
`taint = same-origin`; for each consecutive pair (last, url): if url is not
same-SITE with last AND O is not same-site with last → `cross-site` (final);
else if url is not same-ORIGIN with last AND O is not same-origin with last →
`same-site`. Serializing the request origin gives `null` unless the taint is
`same-origin`. In words: once a request has been redirected FROM a URL that is
not the page's own origin to a URL of another origin, the page's origin is
written `null` from then on, in `Origin`, in the preflight, in the cache key,
and in the CORS check's comparison. ("Same site" here is § 1's stand-in.)

## 8. Credentials, across redirects

**What a credential is here.** os64 has cookies and nothing else that the
standard calls credentials: no HTTP authentication entries (no 401 prompt,
§ 13), no client certificates. So "include credentials" means exactly two
things for a hop: the jar's `Cookie` line is sent, and the hop's
`Set-Cookie` lines are heard. A page's own `Authorization` header is NOT a
credential in this sense (§ 4.6: it is sent whatever the credentials mode),
and is governed by the CORS rules (§ 3, § 4) and the redirect strip below.
`Proxy-Authorization` is libfetch's and the environment's, and is untouched.

**includeCredentials, per hop** (§ 4.6): true when the credentials mode is
`include`, or it is `same-origin` and the hop's tainting is `basic` (§ 6);
false otherwise, and always false for a preflight.

| mode | same-origin hop (tainting `basic`) | cross-origin hop (`cors`) | after any cross-origin hop (still `cors`) | `no-cors` cross-origin (`opaque`) |
|---|---|---|---|---|
| `omit` | no cookies sent, none heard | none | none | none |
| `same-origin` (fetch's default; XHR without `withCredentials`) | sent and heard | none | none (even back on the page's origin) | none |
| `include` (XHR `withCredentials`) | sent and heard | sent and heard; the response is READABLE only with `Access-Control-Allow-Credentials: true` and an exact `Access-Control-Allow-Origin` | sent and heard | sent and heard |

- `Set-Cookie` heard when includeCredentials is true is heard EVEN IF the CORS
  check then fails (§ 4.7 stores cookies before § 4.4 checks; § 3.3.6:
  "However, any Set-Cookie response headers will be respected"). It is never
  heard when includeCredentials is false (§ 3.3.6: "they are ignored
  otherwise"; § 4.7's condition).
- The cookies chosen for a hop are the jar's for THAT hop's URL, over that
  hop's actual transport (`encrypted` is libfetch's per-hop answer, so a
  Secure cookie never rides a plain proxy), with HttpOnly cookies included
  (§ 3.1.1: httpOnlyAllowed is true from fetch; the page can still never read
  them: `Cookie` is a forbidden request header and `Set-Cookie` a forbidden
  response header, so neither name ever reaches script).
- **The `Authorization` strip** (§ 4.5): when a redirect's target is not
  same-origin with the CURRENT URL, the page's `Authorization` header is
  deleted from the request, and it stays deleted for every later hop (even
  one that comes back). (libfetch's own rule re-adds it on a hop back to the
  typed origin; the standard's is stricter, and is the one used.)
- **Method and body across a redirect** (§ 4.5): 301/302 with `POST`, or 303
  with anything but `GET`/`HEAD` → method `GET`, body dropped, and the
  request-body headers (`Content-Encoding`, `Content-Language`,
  `Content-Location`, `Content-Type`) deleted; 307/308 keep method and body
  (the bytes are held by the job, so a replay always has them).
- **Redirect count** (§ 4.5): the 21st redirect is a network error (count 20),
  NOT libfetch's default of 5. A `Location` that does not parse is a network
  error; one that is not `http`/`https` is a network error; one with userinfo
  never parses (os64's URL grammar refuses userinfo).
- **What libfetch already does, and how this relates.** libfetch keeps
  `Cookie` and `Authorization` from `extra_headers` to the TYPED origin and
  strips them on a hop that leaves it (fetch.c `extras_for_hop`), and offers
  each hop's cookies through `headers_for`. Because the job stops libfetch at
  EVERY redirect (`on_hop` → STOP) and opens each hop itself, libfetch's typed
  origin is always the hop's own URL, so that rule never fires inside a fetch
  job: the job's strip (above) replaces it, and the job's `headers_for` sends
  `Cookie` only when the hop's includeCredentials is true and never sends
  anything else but `Referer`. The job never puts `Cookie` in
  `extra_headers` at all.
  **RULED (2):** the standard's rule replaces the library's, in the job.
  Driving the loop in the job is what lets the `Origin` header, the
  preflight and the tainting change per hop (none of which libfetch's
  per-hop hooks can express: `headers_for` may supply only `Cookie` and
  `Referer`). libfetch's typed-origin rule never fires inside a fetch job,
  so nothing here may rely on it.
- **Downgrade hops.** **DIVERGES:** a redirect from `https` to `http` is a
  network error for every script request, also on an `http` page where the
  standard would follow it. fetch.h states the house policy ("a person at a
  keyboard may choose what a script may not") and a 307 would otherwise
  replay a body in the clear to an address the page never named. A page can
  still fetch an `http` address it names itself (when § 11 allows).
  **RULED (3):** kept.
- **SameSite.** The jar RECORDS `SameSite` and does not enforce it (existing
  debt, YONDER.md; MODERN_WEB.md Tier 1). Under `include`, a cross-site fetch
  would therefore carry a site's Lax and Strict cookies, which Chrome (Lax by
  default for unset) would not send.
  **RULED (4): (b).** The two weighed:
  (a) build the filter now, for script requests only: a hop that is
  cross-site from the page (§ 1's stand-in: not the same scheme and host), or
  whose redirect-taint is `cross-site` (§ 3.1.3), carries only `SameSite=None`
  cookies, unset counting as Lax — a one-flag change to `way_jar_cookies`,
  but sibling-subdomain APIs with unset or Lax cookies (Chrome sends those)
  would lose their login until the PSL lands; or
  (b) inherit the debt: a no-cors `POST` with `include` is what a scripted
  `form.submit()` already does today with every cookie, so fetch adds no
  capability in kind, and the response is unreadable without the server's
  explicit `Access-Control-Allow-Credentials`. My lean is (a), because a
  fetch is invisible and repeatable where a form submission navigates away,
  but it costs compatibility and is the jar's file, not this slice's.
- **The HTTP cache.** **RULED (5):** fetch and XHR neither read nor write the
  browser's cache (`way_cache_t`) in this slice — every request goes to the
  network, as cache mode `no-store` would. The cache keeps status, type and
  final URL but no response headers (no `Access-Control-*`, no `Vary`), so a
  cached response could not be CORS-checked, and an entry stored from an
  uncredentialed fetch could be served to a credentialed one. The cost is
  bandwidth only. The cache learns headers, `Vary` and a credentials flag
  in a slice of its own (DEBTS.md).

## 9. The CORS check on a response

(§ 4.10; § 3.3.4; the table in § 3.3.5)

1. `origin` = GET `Access-Control-Allow-Origin` (all lines with that name,
   values joined by `, `, § 2.2.2 "get"). Null (absent) → FAILURE. ("Null is
   not `null`": a header whose value is the string `null` is a value.)
2. credentials mode not `include` AND `origin` is exactly `*` → SUCCESS.
3. `origin` is not byte-for-byte the serialized request origin (§ 1, § 7) →
   FAILURE. (So `https://rabbit.invalid/` with a slash fails, `HTTPS://…`
   fails, two lines fail because they join to `a, b`.)
4. credentials mode not `include` → SUCCESS.
5. GET `Access-Control-Allow-Credentials` is exactly `true` (case-sensitive;
   two lines join to `true, true` and fail) → SUCCESS; else FAILURE.
So under `include`, `*` NEVER passes, whatever `-Allow-Credentials` says.

**Lines libfetch could not read.** http.c omits a header line longer than
`HTTP_LINE_MAX` whole and reports its NAME with no value. For a CORS header
that is a value we do not know, and joining what we do know could make two
lines read as one good one. So **a CORS header with an omitted line is
UNREADABLE, and is judged the restrictive way**: `-Allow-Origin` or
`-Allow-Credentials` unreadable → the check fails; `-Allow-Methods` or
`-Allow-Headers` unreadable → the preflight fails; `-Max-Age` unreadable →
nothing is cached; `-Expose-Headers` unreadable → nothing beyond the
safelisted names is exposed. **RULED (6):** confirmed; the standard assumes
every byte is in hand, and where libfetch cannot promise that, "unknown"
reads as "no".

## 10. The exposed headers

(§ 4.1 main fetch; § 2.2.2 CORS-safelisted response-header name; § 2.2.6
CORS filtered response)

For a `cors` response: `names` = extract header list values of
`Access-Control-Expose-Headers` (`#field-name`).
- credentials mode not `include` and `names` contains `*` → the exposed list
  is every header name in the response;
- else `names` non-null → the exposed list is `names` (a literal `*` in it
  then matches only a header named `*`);
- null → empty.
- FAILURE: **RULED (7).** § 4.1 says "if
  headerNames is non-null or failure, then set … to headerNames", which cannot
  mean "set the list to failure". I read it as "non-null and not failure":
  a malformed `-Expose-Headers` exposes nothing beyond the safelisted names.
The response then keeps only headers whose name is a CORS-safelisted
response-header name given that list: the seven always-safe names plus the
listed names, MINUS `Set-Cookie`/`Set-Cookie2` even when listed (a listed
forbidden name is not safelisted, § 2.2.2). Comparison is case-insensitive.
For a `basic` response every header but `Set-Cookie`/`Set-Cookie2` is kept.
`Headers.getSetCookie()` on a network response is therefore always empty.
The filtered header block is what crosses from the job to the window; the
unfiltered one is freed in the job.

## 11. The `Origin` header

(§ 3.2 "append a request `Origin` header", appended in § 4.6 for every hop
that goes to the network, the preflight included)

- tainting `cors` → `Origin: <serialized request origin>` (`null` when
  redirect-tainted, § 7, or when the page's origin is opaque);
- else, method neither `GET` nor `HEAD` → the same value, except when the
  mode is not `cors`, switched on the request's referrer policy:
  `no-referrer` → `null`; `no-referrer-when-downgrade`, `strict-origin`,
  `strict-origin-when-cross-origin` → `null` when the origin is a tuple with
  scheme `https` and the hop's URL is not `https`; `same-origin` → `null` when
  the hop is not same-origin with the request origin; any other policy →
  unchanged;
- else (a `GET` or `HEAD` that is not `cors`-tainted) → no `Origin`.
So a same-origin `POST` from `fetch` carries `Origin` (mode `cors`, method
not GET/HEAD), a same-origin `GET` does not, and a cross-origin `GET` does.
**Note against the brief:** it says "every CORS request and every non-GET";
the standard's second branch names neither `GET` nor `HEAD`, so a `HEAD`
that is not cors-tainted sends none, while a cors-tainted `HEAD` carries
one by the first branch. That is the standard, not a divergence, and this
design follows it.
The page can never supply `Origin` itself (forbidden request header).

**`Referer`** goes with each hop by the request's referrer and referrer
policy; the default policy is `strict-origin-when-cross-origin`, which is
what `way_referrer` already computes; `Request.referrerPolicy` and
`referrer: ""` are honoured, `no-referrer` sending none.

## 12. Mixed content

(Mixed Content § should fetching request be blocked; Secure Contexts §
potentially trustworthy; Fetch § 4.1 calls it on every hop)

- The page PROHIBITS mixed content when its origin (§ 1) is a tuple with
  scheme `https`. (A page that arrived over a plain proxy still has an `https`
  origin and still prohibits; Secure cookies are a separate question, judged
  on the transport, § 8.)
- On such a page, a hop to an `http` URL is BLOCKED unless that URL is
  potentially trustworthy: a host that is an IPv4 literal in 127.0.0.0/8.
  **DIVERGES (permitted):** the name `localhost` is NOT trustworthy here: the
  standard trusts it only when the UA guarantees it resolves to loopback, and
  os64's resolver reads a user-editable hosts file. `data:` URLs are
  trustworthy and allowed.
- Blocked → network error → `TypeError` (or XHR `error`), checked BEFORE the
  request leaves, on the first hop and on every redirect hop, and the page's
  record gets one fact line (`mixed content: <url> from <page origin>`, the
  diag's shape). Nothing is asked of the person: the question bar is for
  navigations, not scripts.

## 13. What is deliberately NOT built, and why that is safe

- **Schemes**: only `http`, `https` and `data:` are fetched. `file:`,
  `about:`, `blob:` (no Blob URL store), `ftp:` → network error. Refusing is
  the safe direction: nothing is fetched.
- **HTTP authentication** (401 prompts, authentication entries, URL
  userinfo): none. XHR `open(…, user, password)` with a username or password
  makes the request URL carry credentials, which os64's URL grammar refuses,
  so `send()` ends in a network error. Nothing secret is ever composed.
- **The HTTP cache** for fetch and XHR (ruling 5): bypassed, so no cached
  response can skip a CORS check or cross a credentials boundary.
- **Service workers, COEP/`credentialless`, CORP, Timing-Allow-Origin,
  resource timing**: none. CORP guards an `opaque` response's bytes against
  side channels; here an opaque body is never read (§ 6), so there is nothing
  for it to guard.
- **Fetch metadata (`Sec-Fetch-*`)**: not sent. A server that relies on them
  must already accept requests without them (older browsers); not sending
  them never sends MORE than a browser would.
- **Private Network Access**: not in the Fetch standard; not built.
- **Third-party cookie blocking or partitioning**: none (the jar has none
  today); SameSite is ruling 4.
- **Subresource Integrity** (`integrity` init member): any non-empty
  `integrity` makes the fetch a network error, FAIL CLOSED, never ignored,
  because a page that asks for integrity is relying on it; libdom carries no
  digest (DEBTS.md).
- **`keepalive`**: the member is honoured as data (the 64 KiB rule throws as
  the standard says), but a keepalive fetch is cancelled with its page like
  any other (it does not outlive the page). Fewer requests, never more.
- **Streaming request bodies** (`ReadableStream` body, `duplex`): a
  `TypeError`, scoped out by the brief's body list.
- **Range with gzip**: libfetch always offers `gzip, identity`; the standard
  sends `Accept-Encoding: identity` when the request has `Range` (§ 4.6). The
  job asks libfetch for identity when the author headers carry `Range`.

## 14. Where the code lives, and one code path for every caller

- **`libway/origin.c` + `way/origin.h`**: pure (no I/O, no libfetch types —
  only `os64/url.h` and the string helpers), so `tools/test_way_origin_host.sh`
  drives it alone under ASan/UBSan: origins and their serialization,
  same-origin, the stand-in for same-site, the method and header predicates
  (§ 3), the preflight decision, request shape and judgement (§ 4), the
  preflight cache with an injected clock (§ 5), the per-hop main-fetch
  decision and response type (§ 6), redirect-taint (§ 7), includeCredentials
  and the redirect rewrite and strip (§ 8), the CORS check (§ 9), the exposed
  list and the filter (§ 10), the `Origin` value (§ 11), mixed content and the
  bad ports (§ 6, § 12).
  **RULED (8):** one copy of every list: libdom links libway and includes
  `way/origin.h`, so the binding's `Headers` guards ask the same predicates
  the job does, and Slice 1a's `d_origin` is `way_origin_serialize`.
- **`yonder/fetch_job.c`**: the job. Its run function `yonder_fetch_run`
  takes the request (URL, method, author headers as one block, body bytes and
  type, mode, credentials, redirect mode, referrer and policy, the page's
  origin and whether it prohibits mixed content, the use-CORS-preflight flag)
  and the page's hooks (the jar, the preflight cache, cancellation), loops
  over hops calling `origin.c`, opens each hop with libfetch (one trust store
  loaded per job, not per hop), and returns the FILTERED product (§ 6).
- **One path for every caller**: async `fetch` and async XHR submit the job
  to the pool; **sync XHR calls `yonder_fetch_run` inline on the window
  thread** with the same options and the same hooks, its cancellation
  predicate the script task's own budget (the binding's `stop`, libjs's
  `os64_js_check_budget`): a sync request ends when its task would.
  `way_fetch_whole` is never used for any of them: it replaces the caller's hooks with ordinary cookie
  handling, follows redirects by its own rule and keeps no response headers
  (libway/load.c), so a request through it would skip every rule above.
  Classic `<script src>` keeps its own job (`no-cors`, `include`, as HTML
  says), unchanged by this slice.

## 15. Two binding rules carried from Slice 1a's cold read

Not origin rules, but they decide whether a page can reach state the rules
above depend on:
- **Private state never reaches script.** A `Headers` list, a `FormData`'s
  entries, a `Request`/`Response` body and its type, an `AbortSignal`'s
  algorithms and reason, the XHR's state and its request: C structures (or JS
  objects never handed out), never a JS Array filled by `JS_SetProperty*`
  (which runs a page's `Array.prototype` setter with the private object as
  `this`). Every C walk is bounded by a count C owns. Every array returned to
  a page is built by DEFINING its elements (`JS_DefinePropertyValueUint32`,
  WebIDL's create-an-array-from-a-list). In particular the request the job
  receives is assembled from C state alone, so no page getter can run between
  the `Headers` guard's verdict and the bytes the job sends.
- **No index held across page code.** After anything that can run page code
  (a getter, `valueOf`/`toString` on an argument, a Proxy trap, an event
  listener, a `forEach` callback), every position is looked up again:
  `Headers` iteration and `forEach` over the sort-and-combined list while a
  callback mutates it (the standard's iterator walks a fresh sort-and-combine
  each step, by index, and stops when the index passes the current length),
  `FormData` entries the same way, XHR state re-checked after every event it
  fires (a `readystatechange` handler may call `abort()` or `open()`), and an
  `AbortSignal`'s algorithm list snapshotted in C before its `abort` event
  runs listeners. A third rule joined them while building: no C work a page
  or a server can size is worse than n log n, and each such walk is held to
  its bound by a count of its steps in a host case (`WAY_ORIGIN_STEPS`,
  libdom's `d_request_steps` and `d_signal_steps`).

## Host cases for `origin.c` (one line each)

Grouped by the section of the Fetch standard each sentence comes from. Each
line is one case in `tools/test_way_origin_host.c`. "→" is the expected
answer.

### Origins (HTML § origin, Fetch § 3.2 serialization)
1. `http://Example.COM/x` → tuple (http, example.com, 80), serializes `http://example.com`.
2. `https://a.test:443/` → serializes `https://a.test` (default port dropped).
3. `https://a.test:8443/` → serializes `https://a.test:8443`.
4. `http://a.test:80` and `http://a.test` → same origin.
5. `http://a.test` and `https://a.test` → not same origin (scheme).
6. `http://a.test` and `http://a.test:8080` → not same origin (port).
7. `http://a.test` and `http://b.a.test` → not same origin (host).
8. `file:///etc/x` → opaque, serializes `null`.
9. `about:blank` → opaque; `data:,x` → opaque; `javascript:1` → opaque.
10. an unparseable URL → opaque.
11. two opaque origins from two calls → not same origin; one opaque origin with itself → same origin.
12. `blob:http://a.test/uuid` → origin `http://a.test`; `blob:file:///x` → opaque.
13. the serialization never has a trailing slash and is lowercase.
14. same-site stand-in: `http://a.test` vs `http://a.test:81` → same site; vs `https://a.test` → not; vs `http://b.a.test` → not.

### Methods and request headers (§ 2.2.1, § 2.2.2)
15. `GET`, `HEAD`, `POST` → safelisted; `get` before normalization → normalizes to `GET`, safelisted after.
16. `PUT`, `DELETE`, `PATCH`, `patch`, `OPTIONS`, `CHICKEN` → not safelisted.
17. normalize: `delete get head options post put` upper-cased; `patch`, `Egg` unchanged.
18. `CONNECT`, `trace`, `Track` → forbidden methods; `PATCH` → not.
19. each CORS-unsafe byte (`" ( ) : < > ? @ [ \ ] { }`, 0x7F, 0x01) in an `Accept` value → not safelisted; HT → allowed.
20. `Accept: application/json` → safelisted; 129-byte `Accept` → not; 128-byte → safelisted.
21. `Accept-Language: en-US,en;q=0.9` → safelisted; `Accept-Language: en_US` → not (`_`).
22. `Content-Language: de-DE` → safelisted; with `/` → not.
23. `Content-Type: text/plain` / `application/x-www-form-urlencoded` / `multipart/form-data; boundary=x` → safelisted.
24. `Content-Type: TEXT/PLAIN; charset=UTF-8` → safelisted (essence is case-folded by the MIME parser).
25. `Content-Type: application/json` → not; `text/plain;` with a `"` parameter value containing an unsafe byte → not.
26. `Content-Type: text/plain, application/json` (one value) → not (strict parse fails or essence wrong).
27. `Content-Type: text` (no subtype) → not (parse failure).
28. `Range: bytes=0-` and `bytes=5-10` → safelisted; `bytes=-500`, `bytes=0-1,5-6`, `bytes = 0-` → not.
29. any other name (`X-Requested-With`, `Authorization`, `Cache-Control`) → not safelisted.
30. unsafe names over a list with `X-B`, `x-a`, `X-A` → `x-a,x-b` (lowercased, deduplicated, sorted).
31. two `Content-Type` entries, `application/json` then `text/plain` → `content-type` unsafe (judged per entry).
32. safelisted entries totalling 1024 bytes → no unsafe names; 1025 bytes → every one of their names unsafe.
33. `Authorization` → non-wildcard name; `authorization` too; `X-Auth` → not.
34. no-CORS-safelisted: `Accept: */*` yes; `Content-Type: application/json` no; `X-Foo` no; `Range: bytes=0-` no (not in the name list); `Range` → privileged.
35. forbidden request headers: each of the 21 names, case-insensitively → forbidden; `Proxy-Foo`, `Sec-Foo` → forbidden; `Proxy` alone and `Sector` → not.
36. `X-HTTP-Method-Override: GET` → not forbidden; `: GET, TRACE` → forbidden; `X-Method-Override: "CONNECT"` (quoted) → judged on the split value.
37. `User-Agent` → treated as forbidden (stated divergence).
38. forbidden response-header names: `Set-Cookie`, `set-cookie2` → yes; `Set-Cookie3` → no.
39. CORS-safelisted response-header names: the seven → yes, with an empty list; `X-Foo` → no; `X-Foo` with list « x-foo » → yes; `Set-Cookie` with list « set-cookie » → no.

### Main fetch, the hop's decision (§ 4.1, § 2.9)
40. same-origin URL, tainting basic, mode cors → BASIC, no preflight even for `PUT` with `X-Foo`.
41. same-origin URL, tainting already cors (after a cross-origin hop) → CORS (never returns to basic).
42. `data:` URL from any page and mode → BASIC, no wire.
43. mode same-origin, cross-origin URL → NETWORK ERROR, nothing sent.
44. mode no-cors, cross-origin, redirect follow → OPAQUE.
45. mode no-cors, cross-origin, redirect manual or error → NETWORK ERROR.
46. mode cors, cross-origin, `file:`/`about:`/`blob:`/`ftp:` URL → NETWORK ERROR.
47. mode cors, cross-origin `GET` with no author headers → CORS, no preflight.
48. mode cors, cross-origin `PUT` → CORS with PREFLIGHT.
49. mode cors, cross-origin `GET` with `X-Foo` → PREFLIGHT; with `Accept: text/html` → none.
50. mode cors, cross-origin `POST` with `Content-Type: application/json` → PREFLIGHT; with `text/plain` → none.
51. use-CORS-preflight flag, cross-origin safelisted `GET` → PREFLIGHT; same-origin → none.
52. each bad port (0, 22, 25, 6000, 10080…) on `http`/`https` → NETWORK ERROR; 8080, 443, 80 → allowed.
53. a hop decision is made per hop: same-origin `PUT` (basic, no preflight) redirected 307 cross-origin → PREFLIGHT on hop 2.
54. a cross-origin `PUT` redirected 303 → hop 2 is `GET` with no body headers → no preflight.

### The preflight (§ 4.8)
55. shape: method `OPTIONS`, URL the hop's, `Accept: */*`, `Access-Control-Request-Method: PUT`.
56. `Access-Control-Request-Headers` lists the unsafe names lowercased, sorted, joined by `,` without a space.
57. no unsafe names → no `Access-Control-Request-Headers` line.
58. the preflight carries `Origin` (the serialized request origin; `null` when redirect-tainted) and no `Cookie`, no `Authorization`, no body, whatever the request's credentials mode.
59. judgement: 200 with `ACAO: <origin>` and `ACAM: PUT` → pass; 204 → pass; 299 → pass.
60. 300, 301, 404, 500 with perfect headers → fail (not ok; a redirect is not followed).
61. missing `ACAO` → fail; `ACAO: *` with credentials omit/same-origin → pass; with include → fail.
62. include: `ACAO: <origin>` without `ACAC: true` → fail; with `ACAC: True` → fail; with `ACAC: true` → pass.
63. `ACAM` absent and method `PUT` → fail; absent and method `POST` (preflighted for a header) → method passes.
64. `ACAM: put` and method `PUT` → fail (methods are case-sensitive).
65. `ACAM: *` and method `DELETE`, credentials same-origin → pass; credentials include → fail.
66. `ACAM: GET, PUT` (with OWS and an empty element `GET,,PUT`) → `PUT` passes.
67. `ACAM: PU T` (not a token) → FAILURE → fail.
68. use-CORS-preflight flag, `GET`, `ACAM` absent → `methods` becomes « GET » and is cached.
69. `ACAH: x-foo` with request header `X-Foo` → pass; `ACAH` absent → fail.
70. `ACAH: *` with `X-Foo`, credentials same-origin → pass; include → fail.
71. `ACAH: *` with `Authorization` in the request → fail (non-wildcard); `ACAH: *, authorization` → pass; `ACAH: AUTHORIZATION` → pass (case-insensitive).
72. `ACAH: x-foo, x-bar` with `X-Foo`, `X-Baz` → fail on `x-baz`.
73. `ACAH: x foo` (not a field-name) → FAILURE → fail.
74. a failed preflight clears every cache entry for its (partition, origin, URL).
75. an unreadable (omitted) `ACAO`, `ACAM` or `ACAH` line → fail.

### The preflight cache (§ 4.9)
76. `Max-Age` absent → entry lives 5 s: hit at 4.9 s, miss at 5 s.
77. `Max-Age: 600` → hit at 599 s, miss at 600 s.
78. `Max-Age: 100000` → capped to 7200 s.
79. `Max-Age: 0` → nothing stored.
80. `Max-Age: -1`, `Max-Age: 1.5`, `Max-Age: abc`, two `Max-Age` lines → failure → 5 s.
81. an entry keyed on origin A does not answer origin B; on URL `/x` does not answer `/y`; on `/x?a` does not answer `/x?b`.
82. a non-credentialed entry does not answer an `include` request; a credentialed entry answers both.
83. a `null`-origin (redirect-tainted) entry does not answer the untainted origin, and the reverse.
84. method `*` entry answers any method for a non-`include` request, and never for an `include` one (stated tightening).
85. header `*` entry answers `X-Foo` but not `Authorization`.
86. a method entry answers the method byte-exactly (`PUT` entry does not answer `put`).
87. a cached method and every cached unsafe name → no preflight; one unsafe name uncached → preflight.
88. a second preflight updates an existing entry's lifetime rather than duplicating it.
89. the 257th entry evicts the expired first, then the one expiring soonest.
90. different partitions (two pages of different top-level hosts) do not share entries.
91. the clock is the injected monotonic one: moving the wall clock back changes nothing.

### Redirect-taint and the serialized request origin (§ 2.2.5)
92. URL list « A » with origin A → same-origin; serialized `http://a.test`.
93. « B » with origin A (a plain cross-origin request) → same-origin taint; serialized origin is A's.
94. « A, B » with origin A → same-origin taint (redirect FROM the page's own origin).
95. « B, C » with origin A (all different hosts) → cross-site; serialized `null`.
96. « B, A » with origin A → cross-site; serialized `null` (back to the page through another site).
97. « B, B' » with origin A, B' same host as B on another port → same-site taint; serialized `null`.
98. once cross-site, a later hop never lowers it.

### includeCredentials, Set-Cookie and the redirect rewrite (§ 4.5, § 4.6, § 4.7)
99. include, any tainting → credentials included.
100. same-origin, tainting basic → included; tainting cors → not; opaque → not.
101. omit → never included.
102. a preflight → never included.
103. Set-Cookie is heard exactly when the hop's credentials are included, including on a redirect hop, and including when the CORS check then fails.
104. 301 or 302 with `POST` → `GET`, body dropped, the four request-body headers deleted; other headers kept.
105. 301 or 302 with `PUT` → method and body kept.
106. 303 with `POST`, `PUT`, `DELETE` → `GET`, body dropped; 303 with `HEAD` → stays `HEAD`; with `GET` → stays `GET`.
107. 307 and 308 with `POST` → method, body and body headers kept.
108. a redirect whose target is not same-origin with the CURRENT URL deletes `Authorization`; a same-origin one keeps it.
109. A → B (strip) → A: `Authorization` stays deleted.
110. redirect count: the 20th redirect is followed, the 21st is a network error.
111. `Location` that does not parse → network error; `mailto:x` → network error; absent → the 3xx is the final response.
112. any `https` → `http` redirect → network error (stated tightening), also on an `http` page.

### The response's CORS check (§ 4.10, § 3.3.5's table)
113. `ACAO` absent → fail.
114. `ACAO: null` (the string) with a request origin that serializes `null` → pass (omit/same-origin); with a tuple origin → fail.
115. omit, `ACAO: *` → pass; omit, `ACAO: *`, `ACAC: true` → pass (ACAC ignored).
116. omit, `ACAO: https://rabbit.invalid/` → fail (slash); `https://rabbit.invalid` → pass.
117. include, `ACAO: *`, `ACAC: true` → fail.
118. include, `ACAO: https://rabbit.invalid`, `ACAC: true` → pass; `ACAC: True` → fail; `ACAC` absent → fail.
119. `ACAO: HTTPS://rabbit.invalid` → fail (byte compare).
120. two `ACAO` lines each equal to the origin → joined, fail.
121. two `ACAC: true` lines → joined `true, true`, fail.
122. a cross-origin redirect response in cors tainting without `ACAO` → network error (the check runs on redirects too).
123. an unreadable `ACAO` or `ACAC` line → fail, whatever the other lines say.

### Response types and exposed headers (§ 2.2.6, § 4.1)
124. basic: every header but `Set-Cookie`, `Set-Cookie2` kept, any case.
125. cors with no `Access-Control-Expose-Headers` → only the seven safelisted names kept.
126. cors with `ACEH: X-Foo, x-bar` → `X-Foo` and `X-Bar` kept too.
127. cors with `ACEH: *`, credentials not include → every header but `Set-Cookie`(2) kept.
128. cors with `ACEH: *`, include → only a header literally named `*` would be exposed beyond the seven.
129. cors with `ACEH: Set-Cookie` → `Set-Cookie` still hidden.
130. cors with a malformed `ACEH` (`X Foo`) or an unreadable one → only the seven.
131. opaque: status 0, status text "", no headers, no body, URL list empty (url "", redirected false).
132. opaqueredirect: status 0, "", no headers, no body, URL kept, redirected false.
133. error: status 0, "", no headers, no body.
134. redirect manual + a redirect: same-origin → opaqueredirect; cross-origin cors with good `ACAO` → opaqueredirect; with no `ACAO` → error.
135. redirect error + a redirect → error.
136. `HEAD`, and status 101, 103, 204, 205, 304 → body null, headers kept per type.
137. redirected is true exactly when the URL list has more than one URL; url is the last, without its fragment.

### The `Origin` header (§ 3.2)
138. cors tainting, `GET` → `Origin: <origin>`; redirect-tainted → `Origin: null`; opaque page origin → `Origin: null`.
139. basic tainting, `GET` and `HEAD` → no `Origin`.
140. basic tainting, `POST`, `PUT`, `DELETE` → `Origin: <origin>`.
141. no-cors `POST` from an `https` page to an `https` URL, policy `strict-origin-when-cross-origin` → origin; to an `http` loopback URL (the only `http` mixed content lets through), each of `no-referrer-when-downgrade`, `strict-origin`, `strict-origin-when-cross-origin` → `null`.
142. no-cors `POST`, policy `no-referrer` → `Origin: null`.
143. no-cors `POST`, policy `same-origin`, cross-origin URL → `null`; same-origin URL → origin.
144. no-cors `POST`, policy `unsafe-url` or `origin` → origin.
145. cors-mode `POST` same-origin with policy `no-referrer` → origin (the policy switch applies only when mode is not cors).
146. the preflight carries the same `Origin` value as its request would.

### Mixed content (Mixed Content § 4, Secure Contexts § 3)
147. https page → `http://a.test/` → blocked; → `https://a.test/` → allowed.
148. https page → `http://127.0.0.1/`, `http://127.9.9.9:8080/` → allowed; → `http://localhost/` → blocked (stated choice).
149. https page → `data:,x` → allowed.
150. http page → `http://a.test/` → allowed; file page (opaque) → `http://a.test/` → allowed.
151. https page, `https://a.test` redirecting to `http://b.test` → blocked at the hop (and refused anyway as a downgrade).

---
