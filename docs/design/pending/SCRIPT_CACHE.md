# SCRIPT_CACHE.md — a script's requests through the HTTP cache, and modules from their bytecode

*Opus, 2026-10-30. A design, not code: Chris and Fable ruled option B for
7b (BYTECODE_CACHE.md § 9), with four conditions of Fable's, and asked for
the design before the first line. This is ORIGIN.md ruling 5's booked
slice — "the cache learns headers, `Vary` and a credentials flag" — and
then 7b on top of it.*

## 1. Why, measured

Ruling 5 sent every script request — `fetch()`, XMLHttpRequest, and with
them every module — to the network, because the cache kept no response
headers and so could not answer a CORS question. Measured on the P5 with
PR #423's instrument (three runs, `yonder --exit-when-loaded` over ssh):

| Page | Module fetches | Bytes | Fetching, summed over 12 workers | Compiling, on the window thread |
|---|---|---|---|---|
| a pull request, run 1 | 207 | 8.8 MB | 29.9 s | 3.17 s |
| the same, run 2 | 217 | 9.0 MB | 32.0 s | 2.96 s |
| a repository's home | 117 | 3.0 MB | 17.5 s | 0.44 s |

A median fetch is about 110 ms. Fetching costs ten to forty times what
compiling does when summed; over twelve workers and a graph found in waves,
its wall-clock cost on the big page is about the compile's, and on the
smaller one well above it. These are content-hashed assets, and the one
checked answers `Cache-Control: public, max-age=31536000, immutable`,
`Access-Control-Allow-Origin: *` and `Vary: Accept-Encoding` alone: a
year's freshness, no credentials, nothing varied on but the encoding. So
the cache comes first (it
takes away the fetches, the larger and the parallel half) and 7b second (it
takes away the compiles, the serial half on the window's thread).

## 2. The four conditions, and how each is met

1. **The credentials are part of the key, not a flag beside it** (Fable).
   An entry's key holds whether the request that stored it sent credentials
   (`way_credentials_included`: cookies went, or would have). A lookup
   builds the same key from its own request, so an anonymous fetch's entry
   is never even FOUND by a credentialed request, nor the reverse: there is
   no comparison a later path could forget, because a different key is a
   different file.
2. **The CORS check on a stored response is the live one** (Fable).
   `way_cors_check(r, &resp)` takes the request and a `way_response_t` —
   status, header block, and the names of header lines too long to read.
   The entry keeps exactly those three, as received; a hit builds the
   `way_response_t` from them and calls the same function with the
   current request. There is no second checker.
3. **Measure first.** § 1.
4. **The cache is partitioned by the page's top-level site** (Fable's
   question, decided here; § 3).

## 3. The partition — the WHOLE cache, decided up front

**Every entry's key begins with the page's site**: the partition key the
preflight cache already uses (ORIGIN.md § 5), the page's own origin's
schemeful host, since yonder has no frames. A picture, a sheet, a classic
script and a module fetched for one site's page are kept apart from the
same addresses fetched for another's.

**The whole cache, not only the new entries.** The leak Fable names — one
site learning what another cached by timing a fetch — is the same leak for a
picture as for a module, and one rule is a rule nobody has to remember the
exception to. The cost is what every browser has paid since 2020: a shared
asset (a font from a CDN, a common library) is fetched once per site rather
than once. It is decided now because it changes every entry's key, which is
cheap while the cache is young and a migration later.

`way_hooks_t` gains the partition (`hooks_ask` already learns the page's
address); `way_fetch_whole` keys by it; the entry format becomes version 2,
and a version 1 entry reads as a miss and is swept as it ages. 7a's
companions follow their entries' keys unchanged — a companion is named by
its entry.

## 4. What an entry keeps

Today: the address asked for and the one it came from, status, type,
charset, the keep fields, the body. Version 2 adds:

- **the key's parts**, checked on read as the address is today (the file
  name is a hash): the partition, the credentials bit, the address;
- **the response's header block as received** (the fetch job's, which
  already strips `Set-Cookie`: a hit never replays a cookie), and its
  **omitted** names — what `way_cors_check` reads;
- **the request header values the response `Vary`s on**, name and value, so
  a lookup whose own values differ is a miss. `Vary: *` is never kept.
  `Vary: Origin`, which CDNs serve to CORS requests, works: the origin is
  the request's, and a different origin on the same site is a miss.

CACHE.md rule 1's "no `Vary` but `Accept-Encoding`" relaxes to "`Vary`
on headers the entry recorded". Entries from `way_fetch_whole` record what
its requests send.

## 5. Which requests use it

A script's request goes through the cache when it is a GET with no body,
fetched by the fetch job, under its cache mode, which libdom already parses
(`kCaches`) and the host request must now carry:

| Cache mode | Read | Write |
|---|---|---|
| `default` | fresh: served; stale: revalidated | yes |
| `no-store` | no | no |
| `reload` | no | yes |
| `no-cache` | always revalidated | yes |
| `force-cache` | served whatever its age | yes |
| `only-if-cached` | served whatever its age; a miss is a network error | no |

Modules are fetched in mode `default` (HTML's "fetch a single module
script"), so the measured pages take the first row.

Not cached, booked: a response that came through a redirect (CACHE.md's
permanent-redirect rule waits for a page that needs it), a range request,
and anything but GET.

## 6. A hit, a stale entry, and the CORS path

- **Fresh hit**: the stored response, put through the live CORS check with
  this request. **A stored response that fails it is a MISS, not a network
  error**, and the request goes to the network as if nothing were kept.
  DIVERGES: the standard answers the stored response, failing; failing over
  to the network can only turn a failure into a success the server itself
  would give, never the reverse, and it covers the server that answers
  per-origin without saying `Vary: Origin`.
- **Stale, with a validator**: the conditional lines (`If-None-Match`,
  `If-Modified-Since`) are added BELOW the CORS layer — after the
  preflight decision, as Fetch adds them in "HTTP-network-or-cache fetch" —
  so they never make a request preflight-worthy. A 304 lays its headers over
  the stored ones (CACHE.md rule 4), and the merged response goes through the
  live check.
- **The response's type** (`basic`, `cors`, `opaque`) is the request's to
  decide on a hit as on the network: it is not stored.

## 7. Lifecycle

| Event | Entry | Hit |
|---|---|---|
| script GET, cacheable, credentials as the key says | stored with its headers, omitted names and Vary'd values | — |
| same request, fresh, Vary'd values equal | — | served; live CORS check; failed check = miss |
| a credentialed request for an anonymous entry (or the reverse) | not found: another key | — |
| another site's page asks for the same address | not found: another partition | — |
| stale, validator | revalidated below CORS; 304 merged; checked live | served after 304 |
| a new body | replaces the entry; its companion stays until its key misses (7a) | — |
| `no-store` answer, or `Vary: *` | not kept; the entry it would replace dropped, companion with it | — |
| cache mode `no-store` / `reload` | not read (`reload` writes) | — |
| version 1 entry | a miss; swept by age | — |
| cache off, cleared, swept, two windows | as CACHE.md, the partition changing nothing | — |

## 8. Then 7b

With modules' bodies in the cache, 7b is 7a again: a companion beside the
module's entry, tagged by its body's digest, read on the fetch's worker,
and run through a module verb in libjs (`os64_js_module_compile_bytecode`,
its resolution left to the loader as a compile leaves it). The name is not
renamed at the read: a module's name is its identity for resolution, so
the blob carries it and one made under another name is a miss.

## 9. Proof

- libway: the key's parts each a different file (partition, credentials,
  address); `Vary` recorded and compared; `Vary: *` refused; version 1 a
  miss; the header block and omitted names round-tripped.
- The fetch job's host test, against the fake libfetch: a cached CORS
  response served and checked live (a stored `Access-Control-Allow-Origin`
  for another origin a miss, the network asked); an anonymous entry never
  answering a credentialed request and the reverse; the conditional lines
  not making a request preflight; a 304 merged and checked; each cache mode
  of § 5; `Set-Cookie` never replayed.
- WPT: the existing directories unchanged. `fetch/http-cache/` cannot be
  run here: its pages are driven by a wptserve handler (`http-cache.py`),
  and the runner answers a `.py` handler with a network error.
- 7b: libjs's module cases (a graph read from its blobs in another
  runtime, every refusal, each allocation of a read and of a keep's write
  failed in turn); the fetch job's (a module's blob read only for a body
  the entry holds, under the module's tag, the credentials in its key);
  yonder's end to end (a module kept, its blob written, the next visit's
  module from the cache and read from it, the record saying so).
- The P5: the pull-request page loaded twice with `--exit-when-loaded`; the
  second record's request lines say how many came from the cache (a new
  `kept` on the line) and the module fetching's sum falls.

## 10. Ruled (Chris, 2026-10-30)

1. **The whole cache partitioned** (§ 3), not only the new entries: yes.
2. **A stored response that fails the live CORS check is a miss**, not a
   network error (§ 6): yes.
3. **The cache-mode table** of § 5 as the slice's scope: yes.

## 11. As built

- **What the job can say a request sent** (`request_value`, for `Vary`):
  its Accept, and the lines the page and the hop added, Origin among them.
  The lines libfetch and the agent's preset add (User-Agent,
  Accept-Language, Cookie, Referer, `Sec-*`) it cannot, so a reply varying
  on one is not kept (DEBTS.md).
- **A hit reaches the page by the network's own path**: the final
  response's filtering is one function (`finish`), the network's body or
  the kept one handed to it.
- **The cookies never reach the disk**: a kept head is
  `way_headers_storable`'s, Set-Cookie and Set-Cookie2 struck; a 304's are
  struck again as it is laid over (`way_headers_update`, which also leaves
  the kept body's framing lines alone).
- **The answer's source is on the page record's request line**: `, kept`
  or `, confirmed` (YONDER_DIAGNOSTICS.md). The cache's own counts, which
  Settings shows, are still `way_fetch_whole`'s alone (DEBTS.md).
- **`way_served_t` moved to `way/cache.h`**, since the fetch job, which
  does not include all of `way/way.h`, says it too.
- **The two paths share one key space, and each takes only what it can
  answer** (Codex #425 rd1). An entry `way_fetch_whole` kept (a
  picture's, a sheet's, a classic script's) has no head, so a script's
  request reads it as a miss and the reply it keeps replaces it; an entry
  a script's request kept with recorded `Vary` values is a miss for
  `way_fetch_whole`, whose requests do not say what they sent.
- **The cache answers a request's first hop alone**: a later hop is
  another address, which a kept reply's validator must not reach.
- **A request's own lines make its mode** (`cache_mode_of`, once, as the
  fetch starts): a default-mode request carrying a precondition of the
  page's own (`If-Match`, `If-None-Match`, `If-Modified-Since`,
  `If-Unmodified-Since`, `If-Range`), as Fetch's HTTP-network-or-cache
  fetch says, or asking `Cache-Control: no-store`, is no-store; one asking
  `no-cache` or `max-age=0`, or `Pragma: no-cache` with no Cache-Control
  (RFC 9111 § 5.2.1), is no-cache, asked about every time. **`only-if-cached`
  with no cache to ask** is a network error, nothing sent.
- **An answer the cache may not keep drops the key**, whether the request
  read it or not (a reload does not read); **a POST, PUT, PATCH or DELETE
  that worked drops its address's entries**, under either credentials
  (RFC 9111 § 4.4).
- **Repeated request lines a reply varies on are one list**, compared
  whole, in order (Codex #425 rd2), **and a line not sent is not a line
  sent empty** (rd3): a recorded value is `=value` for one sent, `-` for
  none (`way_said_t`). An entry recorded before that read as a miss.
- **A 304 lays its omitted names over the kept ones by name**, as its
  header lines are laid (rd3): a line it sends readable is no longer
  omitted.
- **A 304 confirms a varied entry by the varied rules**
  (`way_keep_confirm_varied`, rd4): one repeating `Vary: Origin` renews
  the entry and keeps its compiled form, where the plain rules would have
  let both go.
- **7b's blob is read by the fetch job** (`module_bytecode`) for a module's
  request whose answer the entry holds — served kept, confirmed by a 304,
  or just stored — and rides to the module map with the answer
  (`yonder_modules_answered`); a compile is kept by 7a's write job, whose
  key now carries the credentials (a cross-origin module's entry is
  anonymous). The bytecode record and its tags moved to
  `yonder/bytecode.c`, so the fetch job's host test links them.
- **The round-trip oracle stays classic.** Reading a module enters it in
  the context's modules under its stored name, and nothing undoes that, so
  a module cannot be written and read back in the runtime that compiled
  it. The libjs and yonder cases above are its proof instead.
- **Engine patch 0016**: a module read that fails part way is unlisted and
  freed (libjs CONTRACT.md). Upstream's failure path left a half-read
  module listed under its name, which the GC walked at teardown — a
  crash on a runtime that ran out of memory reading one.
