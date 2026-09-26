# 05 — libfetch: a request body, and the cookie and Referer seam

*Two rows from LIBPAGE.md's booked table, both with the trigger "the first
login worth doing". A daily-driver browser logs in. This is fetch
machinery — streams, heads, bodies, buffer ownership across a read
boundary — so it is Fable-tier by BROWSER.md's own split, and Codex rounds
are Chris's call. Written as a packet so the board shows it.*

## Delivery

Implemented in two dependent slices: `codex/fetch-post`, then
`codex/fetch-cookies`. The public API is in `fetch.h` and `http.h`;
LIBFETCH.md records the ownership and callback contracts. Evidence is in
`docs/fetch-post-evidence.md` and `docs/fetch-cookies-evidence.md`.
The cookie jar and connection reuse remain outside this packet.

## Starting point for this packet

- `os64_fetch_open(url, opt)` sends GET only. `http_request` (`http.c`)
  renders the request line, `Host`, the caller's `User-Agent`/`Accept`,
  `Accept-Encoding: gzip, identity`, `Connection: close`, then the
  caller's `extra_headers` block verbatim (every byte judged by the
  field-byte rule so a header composed from page content cannot split a
  request).
- Redirects: `ask()` loops per hop; `extras_for_hop` re-derives the
  caller's headers per hop with the CREDENTIAL RULE — `Cookie` and
  `Authorization` go to the typed origin and to hops that stay on it,
  never to a hop that leaves; `Proxy-Authorization` to the proxy only.
  Right for a header the CALLER composed once; wrong for a jar, whose
  cookies are chosen per URL by domain and path and are different on
  every hop.
- The head parser (`http_response_t`) keeps the fields the FRAMING and
  the CONSUMER need and drops the rest, so `Set-Cookie` never reaches the
  caller. `Set-Cookie` is also the one header RFC 6265 forbids folding
  into a comma list, so "keep all headers as a map" is the wrong shape:
  they must be handed out one line at a time.
- libpage builds the POST body and its content type today and checks the
  bytes in its corpus; `wend` refuses a POST BY NAME at `perform` because
  nothing can carry it.

## Scope and ownership

Own `fetch.h`/`fetch.c`/`http.h`/`http.c`, `tools/test_http_host.sh` and
`tools/httptestd.py`, LIBFETCH.md. The JAR is NOT here: it is the
navigator's (packet 06, and LIBPAGE.md said so before either packet
existed). This packet builds the two doors the jar needs and a body the
form needs. os64get gains nothing and changes nothing.

## Deliverable

**A body.** `os64_fetch_options_t` grows `method` (GET or POST, an enum
and never a string), `body`/`body_len`, and `content_type`. The body is
CALLER-OWNED and must outlive `open`, because a 307 or 308 resends it:
copying it would double the memory of an upload and the caller already
holds it. `http_request` grows the method, `Content-Type` and
`Content-Length` lines and the body is written after the blank line by
the same writer that writes the head, in one connection. **The redirect
table for a POST is the browsers' and not RFC 7231's "MAY":** 303 becomes
a GET with no body (that is what 303 is FOR); 301 and 302 become a GET
with no body, which every browser has done since Netscape and every
server relies on; 307 and 308 resend the SAME method and body. `on_hop`
sees which. No `Expect: 100-continue`; no chunked request body (the
length is known); no `multipart` knowledge here — the enctype is a
content-type string and the body is bytes, exactly as libpage produced
them.

**The two cookie doors, and Referer beside them.** Two callbacks in the
options, each asked PER HOP with the hop's own URL, because that is the
address the jar's domain and path rules are about:

```c
// Asked before each request is rendered. The callback writes the request
// headers that depend on WHERE this hop goes — Cookie, Referer — into
// `out` as "Name: value\r\n" lines (field-byte rule applied on return,
// a bad byte refuses the hop). NULL = none. `extra_headers` stays for
// headers that do not depend on the hop.
bool (*headers_for)(void *ctx, const os64_url_t *hop_url, bool encrypted,
                    char *out, size_t cap);

// Called once per Set-Cookie line of each hop's head, as the head is
// parsed, with the URL that answered — before the redirect is followed,
// so a login's 302 that sets the session and sends you on is heard.
void (*on_set_cookie)(void *ctx, const os64_url_t *from_url,
                      const char *line, size_t len);
```

`headers_for` REPLACES the credential rule for `Cookie` (the jar decides
per URL; libfetch no longer second-guesses a header it did not compose)
and leaves it in place for `Authorization` and `Proxy-Authorization` in
`extra_headers`, which are still the caller's one-shot credentials. A
`Cookie` line that arrives in `extra_headers` keeps today's rule
unchanged, so os64get's behaviour does not move. `Referer` is the
navigator's to compose through the same door, and the one rule libfetch
enforces about it is the standard's downgrade rule: a `Referer` naming an
https page is STRIPPED from a request going out over plain http, whoever
composed it — that is a fact about the wire the library can see and the
caller cannot be trusted to.

**The head parser** grows a streaming hook: each header line is offered
to a per-fetch callback as it is parsed, before the framing decisions,
bounded by the head's existing size cap. `Set-Cookie` is the only
consumer today; the hook is general because the next header somebody
needs (`Content-Disposition`, for a download's name) is the same shape.

## Required evidence

- `tools/test_http_host.sh` against `http.client`: POST bodies at every
  chunk size the harness already sweeps, `Content-Length` exact, the
  redirect-method table (301/302/303 → GET without body, 307/308 →
  POST with the same body), a body larger than one write, a zero-length
  body, a `Content-Type` with a multipart boundary passed through
  verbatim. `Set-Cookie` lines delivered one per line, folded NEVER, in
  order, with the URL of the hop that set them; a `Set-Cookie` on a 302
  delivered before the hop is followed.
- `tools/httptestd.py` grows `/post-echo` (echoes the body and its
  content type), `/303-after-post`, `/307-after-post`, `/set-cookie`,
  `/needs-cookie` (200 with the cookie, 403 without), a downgrade hop to
  check the Referer strip.
- Guest, over slirp against httptestd: a form on a served page posted by
  `wend` (which drops its refusal by name the day this lands) and the
  server's access log as the last word — the LIBPAGE.md rule that runtime
  claims want a guest probe.
- ASan on every host run, as the suite does.
- `/sys/net/tcp` read after a POST: one connection, closed, no
  retransmits on the LAN — the counters are the instrument.

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| The jar itself (RFC 6265 storage, domain/path matching, expiry, `Secure`, `HttpOnly`, the public-suffix question) | the navigator's, LIBPAGE.md's ruling | packet 06's second slice |
| Keep-alive / connection reuse | README: measured first | `/sys/net/tcp` says forty dials hurt |
| Chunked request bodies, `Expect: 100-continue` | the length is always known for a form | a streaming upload |
| `Range:` / resume | still wants a consumer | a download manager |
| `Content-Disposition` for a download's name | the header hook carries it; naming a file from a header is a policy os64get already refuses (a redirect never names the file) | a save-as dialog in yonder |

## Implementation contracts

Two commits/slices: POST plus wend, then the cookie/Referer callbacks.
The public header and LIBFETCH.md are the detailed API contract.

- GET remains the zero-initialized default. Only POST carries a body; it
  borrows immutable bytes through open and never retries a failed send.
- Reserved request fields cannot enter through extra_headers. Framing,
  routing, coding negotiation and body metadata have one owner.
- POST -> GET at the same URL is allowed; SELF compares method too.
- Callback header output must terminate within its supplied capacity;
  failure refuses the request. The per-hop seam accepts Cookie and Referer
  only; static Authorization and Proxy-Authorization retain origin/proxy
  filtering. Mixing static and callback copies of a field is rejected.
- Response callbacks borrow a complete header value for the duration of
  the call. No comma folding, no retention of pointers, no 1xx or trailer
  cookies. Delivery precedes redirects; a later malformed header does not
  roll back callbacks already made. Oversized cookie values are ignored
  whole, never truncated into a different cookie.
- Callbacks run synchronously on the fetch thread, with the same ctx as
  cancellation and on_hop. They must not re-enter the same fetch object.
- Referer from HTTPS is removed whenever the outgoing connection is
  unencrypted, including the existing plain proxy transport of an HTTPS URL.
