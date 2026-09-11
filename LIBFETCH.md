# LIBFETCH.md — a URL in, the page's bytes out

*The design record for `libfetch`, the fetch machinery sawn out of os64get
into a shared library. Written 2026-09-11, the day the browser arc opened;
BROWSER.md fenced this extraction as Fable-tier because it is where the
lifetime bugs live — streams, heads, bodies, buffer ownership across a read
boundary. BROWSER.md is the constitution; this file owns the library.*

## The ruling

**libfetch answers one question: "give me the bytes at this address", and
it answers it the same way for every program that asks.** A URL goes in; a
head (status, media type, length, where the page finally came from) and a
pull-stream of DECODED body bytes come out — chunking off, gzip off,
redirects followed, TLS verified, deadlines kept. What a program then DOES
with the bytes is the program's: os64get stages them into a file, the
line-mode browser hands them to libhtml, the graphical browser hands image
bytes to libimage. None of that is in here.

**Why it is a library and not a bigger os64get.** The browser is a fetch
loop with a face; a browser that copied os64get's http.c would be a
browser with two copies of every bug, and the second copy is the one that
does not get the fix. BROWSER.md's rung 3 built the HTTP machinery "in
cleanly separable functions — the seam a libfetch would eventually be sawn
along" and ruled the sawing a later slice with a consumer. The consumer is
here. This is FreeBSD's libfetch shape (1998, Dag-Erling Smørgrav: one
library under `fetch(1)`, `pkg`, and everything else on the system that
downloads), which is the right shape for the same reason it was there —
a system should have one opinion about what an HTTP reply means.

**What moves, what stays, and the rule that decides.** The rule: *policy
about the WIRE is the library's; policy about THIS MACHINE is the
caller's.* So the library owns the request line, the head parser, framing
and codings, the redirect ARITHMETIC (where does that Location point) and
the default redirect VERDICTS (which of those are safe to follow), the
proxy rules (`$http_proxy`, `$https_proxy`, `$no_proxy` — the same
environment for every program, so one reader), the idle deadline, the TLS
transport and its trust. os64get keeps everything that is about a file on
this disk: destination naming, the `.part`-then-rename discipline, its exit
codes, its prose, its progress meter, and the valet's own dialect
(`GET <name>\n` to 6464), which is not HTTP and never was.

## Where it sits

`userland/libfetch/` builds `/lib/libfetch.so`, prelinked like every
library. Its dependencies are the three it inherits from os64get's fetch
path and no others: `libtls.so` (the encrypted transport), `libgzip.so`
(content coding), `libos64.so` (heap, strings, sockets, the URL grammar).
The arrows never point back: libtls and libgzip learn nothing about HTTP.

```text
os64get / line-mode browser / graphical browser / gopher (its `h` links)
                              |
                         libfetch.so
                        /     |      \
               libtls.so  libgzip.so  libos64.so (url, dial, read_for)
```

Files: `fetch.c` (the driver — dial, ask, head, hops, body), `http.c`
(the parser, moved whole from os64get with its host harness),
`transport.c` (url_io.c renamed: the plain-or-TLS byte source with the
idle deadline), `proxy.c` (the environment rules, moved). Public headers
under `include/fetch/`. Names carry `os64_fetch_`; the HTTP parser keeps
its `http_` names because its harness and its comments are written in
them and nothing outside the library calls it directly.

**One hoist rides along, into libos64, not libfetch:** `http_url_absolute`
— RFC 3986 §5.2 reference resolution — becomes `os64_url_absolute` in
`url.c`. It was kept beside HTTP because "a Location may be relative" was
its only customer. The browser resolves every `href` on every page against
the page's base with exactly this arithmetic, and a navigator that linked
libfetch to resolve a link would be linking a network library to do string
work. The grammar already lives in libos64; its resolution joins it.

## The contract

```c
typedef struct os64_fetch os64_fetch_t;              // opaque; one fetch, one object, no globals

os64_fetch_t *os64_fetch_open(const char *url, const os64_fetch_options_t *opt);
os64_fetch_status_t os64_fetch_status(const os64_fetch_t *f);   // why open or read stopped
const os64_fetch_head_t *os64_fetch_head(const os64_fetch_t *f);// valid once open returns with a head
int64_t       os64_fetch_read(os64_fetch_t *f, void *buf, size_t cap);
const os64_fetch_progress_t *os64_fetch_progress(const os64_fetch_t *f);
const char   *os64_fetch_reason(const os64_fetch_t *f);          // one sentence, the library's words
void          os64_fetch_close(os64_fetch_t *f);
```

- **`open` dials, follows redirects, and reads the head.** It returns NULL
  only for no memory; every other outcome is an object whose `status`
  says what happened, so the caller has the detail (which hop, which TLS
  reason, which dial code) to explain it. It blocks, on purpose: a fetch
  is a thread's work, and the browser that wants twenty images at once
  runs twenty fetches on twenty threads — which is why the library has NO
  STATIC STATE. os64get's transfer buffers were `static` ("transfer state,
  not call state"); here every buffer belongs to the fetch object, and two
  fetches on two threads never touch one byte in common.
- **A non-200 answer is still an answer.** os64get calls a 404 a refusal;
  a browser shows the 404 page, and it is the server's page to show. So
  `open` succeeds for ANY final status and the head carries it; the body
  is readable whatever the number. A redirect the verdict declined to
  follow is a final answer too, head and `Location` intact, and the
  caller decides what to say. The caller, not the library, knows whether
  a 404 is a failure.
- **`read` hands back body bytes with chunking and gzip removed.** > 0 got
  some; 0 the body is over — and `status` says whether it ENDED
  (`OS64_FETCH_OK`: every promised byte arrived) or was merely stopped
  (`OS64_FETCH_CUT`: the peer closed early on a length- or chunk-framed
  body); < 0 it cannot go on, and `status` says why. Close-framed bodies
  cannot tell the two apart and never claim to; that is HTTP/1.0's
  original sin, not the library's. A caller that sees 0 with OK has a
  whole file, which is the property os64get's publish step trusts.
- **The head** carries what a consumer switches on: `status` and the
  server's reason; `content_type` (the media type, lowercased) and
  `charset` (the parameter, lowercased — libhtml's `charset` option is fed
  from this field, and it is the reason `Content-Type` is parsed at all,
  which os64get never needed); `has_length`/`length`; `encoding` (what
  the wire said, so a consumer knows gzip was undone); `url` — the FINAL
  address after redirects, parsed and as text, which is the browser's
  base for every relative link on the page; `hops`; `encrypted` (https
  and not through a proxy) and `via_proxy`, the facts behind os64get's
  "this is not end-to-end" warning, which stays os64get's sentence.
- **`progress`** is counters a caller reads between reads: wire bytes
  received, decoded bytes produced, the promised length. os64get's meter
  and the browser's status line both draw from it. No callback: a
  library that calls back into a UI is a library that holds a lock it
  does not know about.
- **`reason`** is one sentence in the library's voice, for the caller
  that has nothing better to say — `os64_dial_reason`'s pattern, and for
  its reason. os64get keeps its own sentences where they are more
  specific; the browser starts with these.

### Options, and the two callbacks

```c
typedef struct {
    const char *user_agent;        // NULL = none sent
    const char *accept;            // NULL = none sent
    const char *extra_headers;     // "Name: value\r\n"..., refused by name if a byte is not a field byte
    os64_tls_trust *trust;         // caller-owned; NULL = the library loads the store for this fetch
    uint64_t max_body;             // decoded bytes; 0 = no cap
    uint32_t idle_ms;              // 0 = the default (30 s, os64get's URL_IDLE_MS)
    uint32_t max_hops;             // 0 = the default (5)
    bool     no_proxy;             // ignore the environment's proxy settings
    os64_fetch_verdict_t (*on_hop)(void *ctx, const os64_fetch_hop_t *hop);  // NULL = the default verdicts
    bool (*cancelled)(void *ctx);  // NULL = never
    void *ctx;
} os64_fetch_options_t;
```

- **Request headers exist because the browser is the customer being
  designed.** `User-Agent` because a meaningful fraction of the web
  refuses a request without one; `Accept` because content negotiation is
  how a server chooses HTML over JSON; `extra_headers` for `Referer`, and
  later `Range` and `Cookie`, which are headers and not new machinery.
  The field-byte rule (no CR, LF, or control byte in a name or value —
  the request-splitting shape, `is_field_byte` in http.c) is applied to
  all three and refuses by name, because a header the caller composes
  from page content is a header an attacker composes. **And a credential
  is for the origin it was given for:** `Cookie`, `Authorization` and
  `Proxy-Authorization` go to the typed origin and to hops that stay on
  it (same scheme, host and port); a hop that leaves it gets the block
  without them. curl's rule, and the Fetch standard's for Authorization
  — without it, a redirect off the origin would hand the caller's session
  to whoever the redirect names.
- **`on_hop` is the caller's say over redirects, and it fires on EVERY
  hop**, followed or not. The library does the arithmetic first — resolves
  the `Location` against the current address, parses it, and fills the
  hop with the facts: the whole target, its parse result, whether it is a
  DOWNGRADE (https → http), a SELF (the address that just answered), an
  UNFETCHABLE scheme (`mailto:`, `ftp://`), which proxy would carry it,
  the hop count. The default verdict, applied when `on_hop` is NULL or
  returns `OS64_FETCH_HOP_DEFAULT`, is os64get's policy today: follow
  301/302/303/307/308 to a whole, fetchable, different, non-downgraded
  address within the hop cap; stop on anything else. os64get's callback
  narrates each followed hop (its `-> address` line, silenced by `-q`)
  and otherwise returns DEFAULT. The browser's callback can allow a
  downgrade after warning, because a person at a keyboard may choose
  what a script may not. A verdict of STOP ends the fetch with the
  redirect as its final head. 300, 304 and 305 are never followed and
  never offered: a list for a person, an answer to a question nobody
  asked, and a stranger choosing this machine's route.
- **`cancelled` is a predicate, not a flag on the object**, because the
  thing that cancels is a signal handler or a UI thread, and both already
  have a flag of their own (os64get's `install_cancelled`). The library
  asks it before every wait and after every interrupted one, and answers
  `OS64_FETCH_INTERRUPTED` when it says yes. A wait that a signal
  interrupts is otherwise RETRIED under the same idle deadline: a caught
  SIGWINCH must not abort a page load, and os64's signals are numbers a
  handler sees, not a reason a library may act on. So the predicate is
  the only way to say "stop", which is exactly why it exists, and a
  caller that wants Ctrl+C to end a fetch installs the handler that sets
  the flag the predicate reads — as os64get does today.
- **Trust has one ownership rule: the library frees only what it
  loaded.** A caller that loads the store once (the browser, per process)
  passes it and keeps it. A caller that passes NULL (os64get, per
  invocation) gets the store loaded on the first https hop and freed at
  close, with the store's report kept in the status detail when loading
  failed — the "trust once per invocation" rule of #89, now the library's.

### The status vocabulary, and os64get's exit codes

One enum, and every value names a cause a caller can act on. os64get maps
them onto the exit codes it has always had, so OS64GET.md's exit table and
every script written against it stay true:

| `OS64_FETCH_*` | means | os64get exit |
|---|---|---|
| `OK` | head read; or body ended whole | 0 |
| `BAD_URL` (+ `os64_url_result_t`) | the address does not parse | 13 |
| `UNSUPPORTED_SCHEME` | parses, but not http/https | 13 |
| `DIAL_FAILED` (+ the dial code) | no road to the peer | 3, or 15 past the first hop |
| `PROXY_BAD` | `$http_proxy`/`$https_proxy` unusable | 2 |
| `TLS_FAILED` (+ status, policy reason, engine error, store report) | handshake, verification, or trust store | 16 |
| `REQUEST_FAILED` | could not send, or it does not fit | 4 |
| `BAD_HEAD` (+ `http_head_result_t`) | the reply is not HTTP | 6 |
| `UNSUPPORTED` | a framing, coding, or 101 this code cannot honestly undo | 14 — or 5 when the head is the server's refusal (a 404 in a coding nobody decodes is still a 404) |
| `SILENT` | the idle deadline passed | 6 (head) / 7 (body) |
| `REDIRECT_STOPPED` (+ the hop) | a verdict said stop | 15, or 2 for PROXY |
| `TOO_MANY_HOPS` | past `max_hops` | 15 |
| `CUT` | the peer closed before the framing was satisfied | 7 |
| `BROKE` | the connection failed mid-body | 7 |
| `CORRUPT` (+ gzip status) | the coding did not decode | 8 |
| `LIMIT` | `max_body` or the gzip expansion caps | 14 |
| `INTERRUPTED` | the predicate said so, or a read was interrupted | 130 |
| `NO_MEMORY` | | 9 |

The gzip caps move with the code and keep their numbers
(`URL_GZIP_RATIO_MAX`, `URL_GZIP_OUTPUT_MAX` → `OS64_FETCH_GZIP_*`), and
`LIMIT` is refused BEFORE the byte that would cross the cap is produced,
so a capped fetch never hands a consumer a prefix it then has to unlearn.

## Lifetime, the part that earned the fence

- **The stream owns the read buffer, the fetch owns the stream.** The
  head-read over-reads into the body (a socket has no message boundaries;
  http.c's `HTTP_BUF_SIZE` comment is the record), so the first body bytes
  live in the head's buffer. That buffer is inside the fetch object, whose
  life is `open` to `close`, and nothing hands a pointer into it across
  the API: `read` COPIES into the caller's buffer. The head's strings
  (`reason`, `content_type`, the final URL text) are arrays inside the
  head struct, inside the object, valid until `close` and never after.
- **A redirect closes its connection before the next dial**, unread
  courtesy body and all; keep-alive is not spoken, so nothing is kept.
  The transport object is re-initialized per hop, and its ERROR SNAPSHOT
  survives the close on purpose (url_io.c's rule: "error detail survives
  cleanup so the caller can explain a failed read after releasing I/O").
- **A fetch that failed is still closed**, and close is idempotent. The
  caller's discipline is one line: whatever `open` returned that is not
  NULL, `close` it.
- **The gzip decoder belongs to the fetch** and is created only when the
  head says gzip, sized by the caps, destroyed at close; a fetch that
  stopped mid-decode frees it without draining it.
- **The trust store's rule is above.** The TLS transport adopts the socket
  handle (`transport.c`: "The transport adopted the handle"), so exactly
  one of `handle` and `tls` owns the file descriptor at any moment — the
  same invariant url_io.c keeps today, kept.

## What the customers look like afterwards

**os64get** loses roughly six hundred lines and keeps its shape:
`fetch_url` becomes plan the destination → `open` → refuse anything but a
200 in its own words → read into the staged `.part`, ticking its meter
from `progress` → `close` → publish exactly as before. Its `on_hop`
narrates; its `cancelled` is `install_cancelled`; its exit codes come from
the table above; the proxy-terminates-TLS warning is printed from the
head's and each hop's `via_proxy` facts — which moves it from before the
dial to after the head arrives, still before a byte is written. The valet
path does not change by a byte. `os64get.conf`, the archive, `refresh`,
`-q`: untouched.

**The line-mode browser** (Opus's slice, after this one) is `open` → hand
`head->charset` and the reads to libhtml → `close`, with a callback that
lets a person follow a downgrade. **The gopher client** keeps spawning
os64get for `h` links today; when the browser speaks `gopher://`, the
scheme joins libfetch (booked below) and both consumers get it from one
place.

## Proof before integration

**The driver is testable on the host because the wire is a seam.** The
library reaches it only through libos64's syscall wrappers (`os64_dial`,
`os64_read_for`, `os64_write_for`, `os64_close`, `os64_ticks`,
`os64_getenv`) and libtls's transport calls, and `tools/test_fetch_host.sh`
stubs every one of them with a SCRIPTED peer — a table of replies keyed
by address — so `fetch.c`, `http.c`, `transport.c` and `proxy.c` run
UNMODIFIED against every path a real network
cannot be asked to produce on demand: a three-hop chain ending in a 200;
a chain that loops to itself; a downgrade with the default verdict and
with a permissive callback; a hop past the cap; a chunked gzip body cut
mid-chunk; a body that crosses `max_body` on a chunk boundary and one
that crosses it inside a chunk; the idle deadline; cancellation between
reads and inside a read; every status in the table above, each with the
`reason` sentence checked; and the same reply delivered one byte per read
and in random sizes, because the parser's bugs live where a token
straddles two reads. `test_http_host.sh` (the parser against Python's
`http.client`) and `test_os64get_io_host.sh` (the transport's deadline
arithmetic) move with their code and keep passing.

QEMU proves what the host cannot: os64get against `tools/httptestd.py`
(every case it already runs, byte-identical results); a real https fetch
through libtls; the P5's real-world addresses (BROWSER.md's list); the
gopher client unchanged; the late suite green; and `/sys/shlib` showing
one resident libfetch serving two programs at once.

## Booked before the first line (the known-debt rule)

| What | Why deferred | Trigger |
|---|---|---|
| `gopher://` as a libfetch scheme | the gopher client's own wire code works and its menu parser is the UI's; moving it buys nothing until a second program wants gopher bytes | the browser's first `gopher://` link |
| Keep-alive / a connection pool | not spoken today; the object is a FETCH, not a connection, so a pool slides underneath without an API change | `/sys/net/tcp` showing a page's image fetches paying a handshake apiece |
| `Range:` and resume (BROWSER.md 3e) | `extra_headers` can carry the header; 206 semantics and the resume discipline want a consumer | a download worth resuming |
| Cookies | a header the browser composes; the jar is the navigator's, not the fetch's | the first site that will not show a page without one |
| A `data:` scheme | inline images in HTML use it; it is a decoder, not a fetch | the graphical browser's first inline image |
| Progressive `open` (head available before the body starts, on a non-blocking loop) | every consumer today runs a fetch on a thread | a single-threaded UI that must not block |
