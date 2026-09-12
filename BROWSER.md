# BROWSER.md — the campaign constitution

*2026-08-31/09-01, the insomnia session. Chris named the boss fight — a
graphical browser, os64 as daily driver — and ratified the shape: tiny
steps, each a small PR off `userland`, each testable and standalone, each
feature branched off the last and merged forward as reviews complete.
Written by Fable for whoever holds a slice of this campaign (Opus included);
read CLAUDE.md and SUCCESSION.md first, as always. The eyes slice
(`/sys/net/tcp`, PR #46) is this arc's first landing.*

## The two real bosses (and the one that isn't)

HTML parsing is not the hard part — a tag-soup tokenizer for the real web's
common elements is a weekend. The bosses are:

1. **TLS.** The modern web is HTTPS-or-nothing. RULED: os64 BORROWS its TLS
   — recommendation BearSSL (no malloc, no syscalls, caller-owned buffers,
   constant-time), living in ring 3 as `/lib/libtls.so` with the trust
   store selected by `tls.conf` on the conf ladder (default `/etc/certs/roots.pem`, whole-store replacement). The kernel never learns about
   TLS. Rolling our own was considered and rejected on merit: the hazard is
   thirty years of side-channel and oracle attacks, and surviving them
   teaches no kernel lessons.
   Native HTTPS in os64get uses this library for direct connections; see
   [OS64GET_HTTPS.md](OS64GET_HTTPS.md) for its integration and controlled
   acceptance. Public root-bundle selection remains separate. Explicit
   `$https_proxy` retains the terminating `tools/tlsproxy.py` helper, with its
   plaintext-leg disclosure; `$no_proxy` bypass uses native TLS. CONNECT
   tunneling is deferred until needed.

2. **Layout.** Block flow, inline flow, the box model. Distant; the ladder
   climbs there via the gopher/line-mode client's UI. Not yet designed —
   deliberately.

## The stance: the ladder is a TCP shakedown wearing costumes

Our TCP (`kernel/src/driver/net/tcp.c`) is honest, LAN-calibrated v1 —
no SACK, no Nagle, no listener (what it HAS the chaos rig and the P5
paid for, one debt at a time: reassembly, a measured RTO, a send window
with congestion control, scaled windows). Every omission is a decision stated in tcp.h and
booked. The P5's fetch ran
with ZERO retransmits: the LAN has never tested the hard half. Each ladder
slice points the stack at traffic the last one didn't — and
**`/sys/net/tcp` is the instrument**: when `retransmits` and
`out_of_order_dropped` start moving on real internet traffic, THAT is the
evidence deciding which kernel debt gets paid, with data instead of theory.

## The ladder

1. **The eyes — DONE (PR #46).** `/sys/net/tcp`: machine-wide counters,
   then one row per connection; the morgue keeps closed connections listed
   `TCP_MORGUE_TICKS` (15s) so failure aftermath is readable; `rst` in
   flags splits refused from timed-out. netstat(1) is CHRIS'S tool,
   whenever he feels like writing it — the file is its food.

2. **whois (port 43) — DONE.** First conversation with a machine nobody in
   this house administers: `whois example.com` in the guest got IANA's
   answer through slirp, and `-h whois.verisign-grs.com` got the
   registry's. `userland/apps/whois` → /bin. v1 dials `tcp!<server>!43`,
   sends `<query>\r\n` (CRLF, not bare LF — 1982 protocols mean it), reads
   until EOF, prints raw. Default server `whois.iana.org`, `-h <server>`
   to override; referral-chasing (IANA → registry → registrar) is booked
   in DEBTS, not built. The slice's seam work: the dial-refusal words
   moved into libos64 as `os64_dial_reason` — os64get and ping each
   carried a private copy, and whois was the third customer, which is the
   consumer-driven moment a table becomes a library's. Watch /sys/net/tcp
   while it happens; that's what it's for.

3. **os64get learns URLs — Opus-suitable, in INCREMENTS (each its own
   commit, maybe its own PR).** Bare-name operands keep meaning the valet
   (dialect untouched); an operand shaped like `http://host[:port]/path`
   means the world.
   - (a) **DONE 2026-09-02.** URL parse + HTTP/1.0 GET: request line,
     `Host:` header, status line + headers parse, `Content-Length` read,
     read-until-close as the fallback the length-less server forces. The
     machinery lived in `userland/apps/os64get/http.{c,h}` — the seam a libfetch
     would eventually be sawn along — until it was, on 2026-09-11
     (LIBFETCH.md; it is `userland/libfetch/http.c` now). Two rulings the
     increment forced, both written down where they bind: a URL fetch does
     not use the filename routing in `os64get.conf` (whose `* = /bin` rule
     would install a web page as a program); and a framing or coding os64get cannot undo is
     REFUSED BY NAME rather than written to disk, because a `.html` full of
     chunk lengths looks like a successful download. URL downloads use managed
     scratch cleanup but do not make replacement backups; the archive belongs
     to valet installations. See [OS64GET.md](OS64GET.md) for current behavior.
     The validation observations below record the earlier HTTP increments,
     including their temporary-file behavior before installer cleanup. Proof:
     `tools/test_http_host.sh` drives the parser against http.client and
     urllib.parse at every chunk size from one byte up, and
     `tools/httptestd.py` is the deterministic server the guest is pointed
     at — the awkward routes included (no length, cut mid-body, 301, a
     coding to refuse). Verified in QEMU over slirp: sixteen fetches, every
     exit code as designed, a 1 MiB body byte-identical host-side, a cut
     transfer left as `cut.part` with nothing published, and the valet
     dialect still checksumming and installing. And against the real web:
     `http://textfiles.com/computers/` came back 70392 bytes BYTE-IDENTICAL
     to curl's copy on the host — a real server, a real path ending in
     '/', a real index.html. neverssl.com did NOT answer under QEMU — DNS
     resolved it, the SYN went out and was retransmitted three times with no
     SYN-ACK, and `/sys/net/tcp` said exactly that in one `cat`
     (`connect_timeouts: 2`, `rexmit 3`, mss still 536 because nothing ever
     came back to negotiate one). **Chris then fetched the same page on the
     P5 the same day**, which settles what that was: slirp not relaying to
     that host, not a debt in the stack. Worth writing down for the shape of
     the evidence rather than the verdict — the counters answered "who
     failed" in one command, and a second machine answered "whose fault"
     in one try. Neither cost a debugging session.
   - (d) **DONE 2026-09-03.** `Content-Encoding: gzip` streams through
     libgzip into the same managed staging that the identity path uses, and
     the name is published only after `OS64_GZIP_DONE` has verified every
     member CRC/size and ruled out trailing data. `Accept-Encoding` now names
     exactly `gzip, identity` both directly and through `tlsproxy.py`.
     Length-framed replies may expand at most 100x, with a 1 MiB usability
     floor and 16 MiB absolute ceiling; a chunked or close-framed gzip body
     gets the ceiling, because its wire length is not known in advance.
     Identity downloads are unchanged. **The coding comes off DOWNSTREAM of
     the framing** (os64get's body receiver — libfetch's `read_gzip` since
     the extraction — reads through 3(b)'s body reader and
     decides only what the bytes ARE), so a gzip body may arrive chunked and
     the two envelopes come off in the order they went on — and the framing's
     verdict outranks the decoder's: a length-framed gzip reply that closes
     early is a SHORT transfer (exit 7, temporary files cleaned), never a hang and never
     "corrupt". Two `Content-Encoding: gzip` field lines are the list
     `gzip, gzip` (RFC 7230 §3.2.2, a body encoded twice), kept as the value
     and refused by name rather than unwrapped once and published (both from
     Codex's review of PR #54). Host ASan/UBSan parser and gzip suites
     passed; in QEMU over slirp, length-, close- and chunk-framed gzip all
     decoded byte-identically, a bad CRC and explicit trailing bytes exited 8
     without replacing the old name, a 1 MiB + 1 compression fixture stopped
     at exactly 1048576 staged bytes with exit 14, `/gzip-twice` exited 14
     with nothing written, `/gzip-cut` exited 7. The shared DEFLATE decoder
     is also the engine beneath libpng's zlib framing.
   - (b) **DONE 2026-09-03.** HTTP/1.1 and chunked transfer coding. The
     two are one slice because the version is a promise: a 1.0 client is
     owed a length or a close, a 1.1 client must read chunks, and saying
     1.1 without reading them would be the lie 1.0 was chosen to avoid.
     The body reader (`http_body_open/read`, http.h) now owns the framing
     — length, chunked, or the close — and os64get's receive loop sees only
     the file's bytes and one verdict at the end: DONE, CUT, BROKE, or a
     framing that stopped being HTTP. `Connection: close` stays in the
     request because keep-alive is still not spoken. Extensions are
     ignored whole, trailers read to their end and ignored, both bounded
     by the head's own caps. Proof: the host harness runs every body
     through the reference's de-chunker at every split size, and a table of
     28 damaged chunk streams pins the verdict AND the byte count handed
     back before it. Verified in QEMU against httptestd's `/chunked`
     (200000 bytes, sizes 1 to 65536, an extension, a trailer —
     byte-identical) and `/chunked-cut` (exit 7, `.part` left).
   - (c) **DONE 2026-09-03.** Redirects followed: 301/302/307/308 and 303
     (whose whole meaning is "GET this instead" — the famous 301-vs-307
     distinction is about rewriting a METHOD, and os64get only ever sends
     GET), hop cap 5 (RFC 2068 §10.3's own number), each hop announced and
     each judged by the rules the typed address was. The 3xx codes NOT
     followed are refused BY NAME with what each one means: 300 is a list
     for a person to pick from, 305 is a stranger choosing this machine's
     route, 304 answers a conditional request nobody made. Three rulings
     the increment forced. **A REDIRECT NEVER NAMES THE FILE** — the
     destination is settled from the typed address before the first request
     goes out, or a server answering `/download` with a redirect to
     `/.profile` would be choosing a name in somebody's directory (wget
     spells this as `--trust-server-names`, off by default; os64get does
     not offer the switch, since DEST already says "call it this").
     **THE PROXY IS RE-ASKED AT EVERY HOP**, because `$https_proxy` and
     `$http_proxy` are chosen by SCHEME: a plain-HTTP page redirecting to
     https is carried by a variable that had nothing to do with the first
     request. Direct HTTPS uses libtls when no proxy applies; an HTTPS-to-HTTP
     redirect is refused with its target displayed. And a new exit code, **15**, for a road that
     did not arrive (hop cap, a circle, an unreachable target) — distinct
     from 5, the server's final answer about the page, because the thing to
     change is on a different side. `http_url_absolute` (libos64's
     `os64_url_absolute` since 2026-09-11) grew into RFC 3986
     §5.2's full reference resolution to do it (relative refs, `.`/`..`,
     query-only refs), so there is no longer such a thing as a `Location`
     os64get cannot spell. Proof: `tools/test_http_host.sh` runs RFC 3986
     §5.4's OWN vector table — the abnormal examples included — against
     `urllib.parse.urljoin`; `tools/httptestd.py` grew the trails
     (`/redirect/N`, a loop, a page-relative Location, 303/307/308, 300,
     305, `mailto:`); and in QEMU 23 cases came back with every exit code
     as designed, nothing written on any refusal, no `.part` left behind,
     e2fsck clean. Against the real web: httpbin's relative- and
     absolute-Location chains both arrived at `/get`, and
     `http://www.rfc-editor.org/rfc/rfc1945.txt` — a plain-HTTP address
     that 301s to https — refused honestly with no proxy set, then went
     `301 -> https://...` through `$https_proxy` and landed 137582 bytes
     byte-identical to curl's copy.
   - (e) `Range:`/resume — later, wants a consumer first.
   DESIGN CONSTRAINT, and its payoff: the HTTP machinery was kept in
   cleanly separable functions so that the extraction into a shared
   library could wait for a consumer (the house rule). **The consumer
   arrived with the browser arc and the extraction is DONE (2026-09-11):
   `/lib/libfetch.so`, LIBFETCH.md is the record.** os64get is its first
   customer, the line-mode browser its second.
   Verification: `tools/httptestd.py` behind the harness for deterministic
   tests (it grew out of the `python3 -m http.server` this line used to name —
   a well-behaved library will not produce a reply with no Content-Length or
   a connection cut mid-body on request); neverssl.com / textfiles.com /
   frogfind.com for the real world. FOR AN END-TO-END IMAGE TEST, which
   wants a format libimage actually reads, the modern web is nearly useless
   — everything is PNG or JPEG now — but John Burkardt's data archive at the
   University of South Carolina still serves real 24-bit Windows BMPs:
   `https://people.math.sc.edu/Burkardt/data/bmp/blackbuck.bmp` (786486
   bytes, 512x512x24), and `lena.bmp` and `snail.bmp` beside it. https, so
   they exercise the proxy too, and `os64get.conf` already routes `*.bmp` to
   /home/images where gview will find them. Written down because finding
   them took longer than fetching them.

4. **gopher (port 70) — IN TWO PARTS, and the first one is not a
   protocol.** The shape was discussed 2026-09-03, the conversation this
   rung was held open for. Fetch-and-close is a weekend: selector + CRLF,
   read to EOF, a lone `.` ends a menu, whose lines are
   `<type><display>\t<selector>\t<host>\t<port>` (the type character is
   GLUED to the display name — the classic parsing trip). The UI is the
   whole point, because this is where the browser's face is born.

   **CHRIS'S RULING: the links are chosen with the ARROW KEYS.** Numbered
   menus read as oddly backwards to him, and he is right that a browser
   picks links by pointing at them. That decides everything below, because
   pointing needs a screen that can be repainted, and os64's terminal has
   never repainted anything.

   - (a) **The terminal grows a voice.** Three of the four pieces already
     exist and nobody noticed: arrow keys have arrived as `ESC [ A/B/C/D`
     since 2026-08-04 (keyboard.c chose the VT100 spelling for exactly
     this kind of interop); `os64_tty_read` already answers rows and cols,
     which is how `less` pages; and `renderer_glass_putc_bg_locked` already
     paints a cell with a named foreground AND background, because
     overlays needed it. What is missing is that a tty cell cannot
     REMEMBER anything but a foreground, so nothing survives a repaint —
     and nothing in the system has ever sent an escape sequence.
     **Scope, and CHRIS'S RULING on how to choose it (2026-09-03): an
     escape is implemented when something asks for it, and not before.**
     The gopher browser asks for five — `ESC[2J` clear, `ESC[<r>;<c>H`
     position, `ESC[K` erase to end of line, `ESC[<n>m` SGR (reset, bold,
     reverse, the 16 foregrounds and the 16 backgrounds). Scroll regions,
     insert/delete line, the alternate screen, DEC private modes and
     256-color wait for a consumer that names them.
     **Where the state goes is the pretty part:** `tty_cell_t` is 8 bytes
     of which three are padding — ABI-pinned and static-asserted against
     `os64_pty_cell_t`, so the pad is already spoken for by the format and
     wasted by the content. ANSI has exactly 16 background colours and a
     handful of attributes, so a background INDEX and an attribute byte
     fit in that padding: no growth, no ABI change, and per-cell
     highlighting for free. (Storing a second full XRGB would double the
     fleet's ~4MB of scrollback for colours nobody can name.)
     **A property to keep on purpose:** an app that POSITIONS rather than
     scrolls never pushes a line into the scrollback ring, so os64 needs
     none of the alternate-screen dance real terminals invented. Both
     renderers honour the cell — the glass through BasicRenderer, gterm
     through the PTY grid, where its existing batch-by-colour becomes
     batch-by-colour-and-attribute.
     **Its other customers were waiting:** `ls` in colour, errors in red,
     green PASS and red FAIL in the suite, and Chris's `$PROMPT` — which
     wants one small thing more, a way to SPELL an escape byte in a
     variable, since no shell vocabulary here has one yet.
   - (b) **DONE 2026-09-04. The client, `/bin/gopher` — its own program,
     not a mode of os64get.** The protocols differ, but the KIND differs more: os64get
     is a one-shot fetcher that writes a file and exits with a code, and
     this is an interactive session with a screen and a history stack.
     What they share is the dial and the URL parser — `gopher://` URLs are
     how gopherspace is written down, so the client takes one (Chris,
     emphatically, 2026-09-03). That made the URL half of `http.c` its
     second customer and the first honest occasion to ask whether it
     should move, and **the answer was yes (2026-09-04, Fable's ruling on
     Opus's reading of the code): `os64_url_parse` is libos64's now**
     (`os64/url.h`). The argument is a hazard, not a tidiness: the host
     alphabet, the right-to-left colon search, fold-host-never-path, and
     "`host:` with nothing after it is a refusal" each have a security
     edge, gopher needs every one of them, and two copies of an edged rule
     drift until the looser copy is the one somebody reaches. What did NOT
     move THEN was `http_url_absolute` — RFC 3986 §5.2 reference resolution
     had exactly one customer, because gopher has no relative links. (It
     moved on 2026-09-11, as `os64_url_absolute`, the day the browser's
     navigator became its second: LIBFETCH.md.) This is
     the `os64_dial_reason` hoist, not the libfetch one: pure string
     arithmetic with a differential harness already in the tree, no
     streams, no buffer ownership across a read boundary. Bindings are lynx's, whose path this re-walks in the
     right order: Up/Down to move, Enter to follow, LEFT ARROW for back,
     `q` to quit. Item types: `0` text through the pager, `1` menu, `7`
     search (prompt, resend `selector\tquery`), `9`/`I`/`g`/`s` saved to a
     file, `i` shown but not selectable, `3` the server's error shown as
     one, and **anything unknown shown but NOT followable** — guessing
     what a type means is how you download a thing that is not what it
     said. A TYPE MEANS THE SAME THING WHEREVER THE ADDRESS CAME FROM:
     `gopher gopher://host/9/thing.zip` on the command line saves it and
     exits with a code, exactly as pressing Enter on that item would,
     because the framing is the type's answer and not the menu's. Type `h` carries a `URL:http://…` link and is **handed to
     os64get**, which is only as good as os64get's exit codes are precise
     (Chris's condition, and the reason 3(a)–(c)'s code table was worth
     the care it got: 5 refused, 7 short, 13 bad address, 14 a coding it
     cannot read, 15 a redirect it could not follow).
     **THE HANDOFF SHOWS THE PAGE, AND TRANSLATES THE NUMBER** (both
     2026-09-04, from Chris's first hour in real gopherspace). os64get
     fetches into a temp file and the client displays it in the same
     scroller a gopher text file gets, because a person who followed a
     link and got their menu repainted with `exited 0` at the bottom has
     not been shown anything. It is HTML source — os64 has no renderer
     yet — and that is strictly more than nothing. Every code becomes a
     sentence. Direct HTTPS uses os64get's selected trust store; status 16
     names trust, identity or handshake failure. Invalid proxy settings,
     unusable URLs and refused redirects retain their distinct statuses.
     **A DEAD LINK IS A PAGE THAT DID NOT LOAD, NOT THE END OF THE
     SESSION** (2026-09-04, Chris asking whether a "cannot reach" always
     killed the client — it did). The fetch lands in a second page and the
     current one is untouched until it succeeds, so a burrow that has gone
     off the air costs a sentence on the status bar and nothing else; Back
     spends its crumb only if the page it names comes back. The FIRST fetch
     stays fatal, and that asymmetry is the design: `gopher dead.host` is a
     command that failed and owes an exit code. `q` asks before leaving for
     the same reason — the session and its history are what it discards.
     **AND TYPE `h` MEANS TWO THINGS.** A selector without a `URL:` is
     type h's ORIGINAL meaning — an HTML file served by that gopher
     server, predating the convention — and is fetched over gopher as
     text. Reading it as a broken URL would refuse pages that work.
     A PAGE IS A STRANGER'S BYTES ON YOUR TERMINAL, and (a) makes the
     terminal obey more of them — so it takes TWO guards, because there
     are two kinds of page. A MENU LINE is refused whole where it is
     parsed, which is the only place that can stop a doctored item being
     FOLLOWED rather than merely drawn. A TEXT FILE and an `h` link's
     fetched HTML pass through no parse at all — they are somebody's
     document, and refusing one for a stray byte would be refusing the
     page — so they are escaped where they are DRAWN, in `cat -v`'s
     notation, by the one clip-to-width function every painted string on
     that screen goes through. The slice shipped with only the first
     guard and the doctrine "refused once at the parse, never escaped at
     each print", which read as settled because the half anyone checks —
     menus — was the half it covered; a type-0 file full of `ESC[2J`
     owned the screen. **A rule that names where a check goes is only as
     good as its census of what arrives.**
     WHAT THE SLICE SETTLED. The wire lives in `apps/gopher/wire.{c,h}`
     and takes its bytes from a source function, so `tools/test_gopher_host.sh`
     drives every case at chunk sizes 1, 2, 3, 7, 17, 64 and whole. There is
     NO reference implementation to diff against — Python dropped gopherlib
     in 3.0 — so the expectations are stated by hand, except the address
     half, which is cross-checked against `urllib.parse`. Two bugs the host
     suite caught before the OS ever ran it: `%00` in a selector decoded to a
     NUL that every downstream check was blind to (they are all C string
     operations, and they all stop AT it), and a menu line's tail could be
     read past its cap. **A bar is painted with a BACKGROUND, not with
     reverse video** — `ESC[K` fills with the pen's background and clears
     attributes, and padding with spaces to the last column instead writes
     the bottom-right cell, which SCROLLS the screen: the first boot lost its
     title bar off the top edge and drew every row one line high.
     Harness: `tools/gophertestd.py` for deterministic menus, the way
     httptestd made HTTP testable, and Floodgap for the real world.
   Gopherspace is alive: Floodgap, SDF, magical.fish (Chris can vouch for
   the games menu).

5. **telnet (port 23).** The long-lived-interactive shakedown: tiny
   segments both ways, server-initiated data, half-close — everything
   fetch-and-close never exercises, plus a workout for gterm's escape
   handling. Protocol v1: refuse every IAC option (WONT/DONT), pass bytes.
   Shares a BODY with ssh (terminal plumbing, SIGWINCH propagation) but no
   protocol DNA; when ssh's day comes, dropbear is the canonical borrow.
   NOTE: the kernel already names `telnetd` as TCP listen()'s future
   customer (syscall.c) — the LISTEN slice itself is kernel work, not this.

6. **FTP, PASV mode (port 21) — DONE 2026-09-09.** The multi-connection
   shakedown: control channel + a data connection per transfer,
   ephemeral-port churn — the browser's connection pattern rehearsed before
   the browser exists. `/bin/ftp` is the interactive 4.2BSD client, passive
   only, `TYPE I` always, LIST printed rather than parsed; `FTP.md` carries
   the rulings and the deferrals, VERIFICATION.md § FTP client acceptance
   carries the evidence. The rung delivered what it was put on the ladder
   for: two live connections, a fresh ephemeral port per transfer, and the
   NAT case — a server advertising an address the client cannot reach —
   arriving for free through slirp rather than being staged.
   An `ftp://` scheme for os64get is the increment this leaves cheap; the
   protocol half is already a syscall-free file with a host harness.

Beside the ladder, BUILT (2026-09-02): the **chaos rig**, `tools/cable.py`
— a frame-level cable with weather in it (loss, delay, jitter, reorder,
duplication, link cuts, a blackhole after N), plugged into the QEMU
harness through `filter-redirector`, no root. VERIFICATION.md § The chaos
rig is the manual. It is the instrument the TCP debts get measured with:
its first day priced v1's no-reassembly at 29s against 2s for one 100KB
fetch under 30% reordering, CRC-clean both times, while 200ms round trips
with order kept cost a download nothing (VERIFICATION.md has the table). It is QEMU-only by
construction; the P5's chaos rig is the internet itself, read through the
same counters. Stream-level nastiness for HTTP (slow dribbles, truncated
bodies, RST mid-body) is the HTTP lane's own fixture — an os64serve.py
flag someday — not this tool.

## The face: the line-mode browser

The ladder is climbed and the two shared libraries exist: `libfetch`
(LIBFETCH.md — a URL in, decoded body bytes out) and `libhtml`
(LIBHTML.md — bytes in, the standard's tree out). The face is the first
program that puts them together, and its job is the ladder's stance one
more time: point the whole stack at REAL PAGES and find out what breaks.
**It is called `wend`**, chosen 2026-09-11 by Chris's own method from the
names three models offered. To wend is to go somewhere by an indirect and
curious route, and it is old enough that its past tense wandered off to
become the "went" everybody uses for a verb that now has no present tense of
its own. Nobody marches through the web. It lives in `userland/apps/wend/` —
`wend.c` is the session, `render.c` the tree walk.

What follows is what it does. Where the build departed from the spec this
section began as, the departure is marked **(departure)** and argued where
it is.

**What it is.** A full-screen text program in the gopher client's shape
(`userland/apps/gopher/gopher.c` is the model: a title row, content rows,
a status row, `key_read` with patience for the `ESC [ A` arrows, `confirm`
and `prompt` for questions, `history_push` for back). Lynx-shaped on
purpose and DELIBERATELY THROWAWAY as a renderer: it walks the tree and
prints, it does not lay out. BROWSER.md's second boss (layout) is the
graphical browser's, and a cell-based layout engine would be a rough
draft of the wrong thing. What is NOT throwaway is everything the
renderer sits on — the fetch, the parse, and the navigator — which the
graphical browser inherits whole.

**Where it runs: anywhere there is a tty.** The text VTs and a gterm
both, with no code that knows which. The size comes from
`/proc/self/tty` (`rows`/`cols`, the way `/tests/winchtest` reads them);
SIGWINCH (signal 28) says it changed, so install a handler and re-wrap.
Raw mode (`raw` written to `/proc/self/tty`, SIGINT.md § Raw mode) is
what telnet uses so that Ctrl+C reaches the program; the face wants it
too, so a page fetch can be cancelled with the key everybody presses,
through libfetch's `cancelled` predicate. The kernel restores cooked at
exit. Colour and attributes are the SGR subset the terminal draws
(CLAUDE.md § The terminal's escape sequences); `ESC[2J`/`ESC[H` to paint,
`ESC[K` to erase a row.

**The pipeline, per page.**

1. `os64_fetch_open(url, &opt)` with `user_agent` set — a meaningful part
   of the web refuses a request without one, and this one says
   `wend/1.0 (os64)` — `accept = "text/html, text/plain"`,
   `max_body` = libhtml's `max_bytes` (8 MB — the same page, the same
   cap), `cancelled` = the Ctrl+C flag, and an `on_hop` that ASKS for a
   DOWNGRADE (https → http) with `confirm` and returns FOLLOW or STOP —
   a person may choose what a script may not, which is why the callback
   exists. Every other hop takes the default verdict.
2. Read the head. `status` is shown in the status row whatever it is; a
   404 is a page and is shown as one. The request's `Accept` names every
   media type this step will render, so a server choosing between
   representations is told the truth rather than a narrower list it might
   answer 406 to. `content_type` decides the path:
   `text/html`, `application/xhtml+xml`, and a reply that names no type
   at all → libhtml; any other `text/*` → shown preformatted as it is
   **(departure: the spec said `text/plain`, and every other `text/*` is
   a document a person can read — markdown, a stylesheet, a CSV — where
   the alternative was refusing it as "not a page")**; anything else is
   not a page — say what it is and offer `os64get '<url>'` to save it,
   quoted the way os64get's `print_by_hand` quotes an address (husk
   splits at `;`). **A text body's high bytes are read as UTF-8 only
   when the reply's charset says so (departure)**: the old web's `.txt`
   files are Latin-1, and decoding those as UTF-8 turns every accented
   name into a question mark. **Its line endings may be any of the three**:
   a carriage return ends a row as surely as a newline does, and a CRLF
   pair is one ending rather than two, so a file written on a machine that
   ended its lines the other way is still a file with lines. (libhtml
   normalises both before a tree exists, so this is the raw-text path's
   business alone.)
3. `os64_html_parser_new` with `charset = head->charset` (the transport's
   label, which outranks the page's own — libhtml applies the ladder),
   then `feed` every read, then `finish`. A refusal by name (too large,
   too deep, work exhausted) still yields a tree; show what there is and
   say why it stopped.
4. Render the tree to LINES (below), wrapped at `cols`; keep the lines,
   the spots and the forms; paint the visible window.
5. The final address is `head->url_text`, and it — not what was typed —
   is the base every `href` resolves against, with `os64_url_absolute`
   (libos64 `url.h`), unless the page carries `<base href>`, which wins.

**The renderer: tree → lines.** Text is UTF-8 in the tree and Latin-1 on
the glass, so every character passes through the fold (below) on its way
to a cell. Whitespace collapses to one space except inside `pre` (and
`textarea`, `listing`), which is why libhtml kept it verbatim.

- **Block elements** start a new line: `p`, `div`, `h1`–`h6`, `ul`, `ol`,
  `li`, `dl`, `dt`, `dd`, `blockquote`, `pre`, `hr`, `table`, `tr`,
  `form`, `fieldset`, `address`, `center`, `section`/`article`/`nav`/
  `aside`/`header`/`footer`/`main`, and `br` breaks without a blank.
  The same-family elements a modern page is built from join them
  **(departure)** — `figure`/`figcaption`/`caption`/`summary`/`details`,
  the table's own `thead`/`tbody`/`tfoot`, and `menu`/`dir`, which are
  lists — because a caption glued to the next paragraph reads as a
  rendering fault rather than as a shorter list of tags.
  `p` and the headings get a blank line before and after; `li` gets a
  bullet (`* ` for `ul`, `1. ` counting for `ol`) and its nesting depth
  as indent; `blockquote` and `dd` indent; `hr` is a row of `-`; a
  heading is drawn bold (SGR 1). A `table` is rows: `tr` is a line and
  `td`/`th` cells are separated by two spaces — no column alignment in
  the first cut, and the old web's table LAYOUT (a page that is one big
  table) reads acceptably as a sequence of rows, which is what lynx shows.
- **Inline elements** change the pen: `b`/`strong` bold, `i`/`em`/`u`
  underline (SGR 4), `code`/`tt`/`kbd` plain (there is one font), `a`
  with an `href` is a LINK: numbered in document order, drawn as
  `[n]text` with the link colour, and entered in the spot table with its
  resolved address AND, kept beside it, the `#name` the href asked for —
  the resolver drops a fragment because a fragment never crosses the
  wire, and dropping it here too would turn a table of contents into a
  row of links that each refetch the article and show its top. `img`
  draws `[alt]` when there is alt text and `[image]` when the attribute
  is ABSENT; `alt=""` draws nothing at all, because an empty alt is the
  page saying the picture is decoration and has no words — a modern
  article's icons and tracking pixels all say it. An `img` inside an `a`
  is the link's text.
- **A form's controls are SPOTS TOO**, drawn in their own colour and
  numbered in the same sequence as the links: a box you type in as
  `[n][value___]` at the page's own `size`, a tick box as `[n][x]` or
  `[n][ ]`, one of a radio group as `[n](*)` or `[n]( )`, a list as
  `[n][v the chosen option]`, a button as `[n][its words]`. A `textarea`
  is a box with its text as the starting value — kept VERBATIM, because
  that text is the field's value and collapsing its spacing would send
  the server something the page did not put there — shown on one row,
  because a line-mode browser has no second row to give it. A control
  that does nothing this browser can honour — a reset, a plain button —
  is drawn and is NOT a spot, because landing on it would promise
  something. A DISABLED control is the same: drawn, not landed on, and
  never sent, which is what the page disabled it to arrange — and a
  `fieldset` that is disabled disables everything under it, since that is
  how a page greys out a whole section and the controls inside carry no
  attribute saying so. The words in its first `legend` are the exception,
  the standard's: a section's title was never a control. A HIDDEN
  field is neither drawn nor landed on: it is remembered against the
  form, whose data it is. **A PASSWORD is drawn as its length**, never
  its value, and echoes stars while it is typed — a page that prefills
  one is not a reason to put it on a screen somebody is standing behind.
- **Skipped whole:** `head` and everything in it (`title` goes to the
  title row), `script`, `style`, `iframe` (a different document, which
  showing would mean fetching), `noscript`'s CONTENTS are shown (we run
  no script, so the standard parsed them as markup — that is the point),
  `template` contents (the fragment branch), comments, and every subtree
  whose `ns` is not HTML (SVG and MathML draw nothing in this face).
  **A `frame` is the exception (departure):** a frameset page has no
  body and no prose anywhere, so skipping it paints an empty screen for
  a whole era of the web. Each frame becomes a link to the document it
  names, which is what the page was going to show you anyway.
- **Wrapping** is by words at `cols`; a word longer than the row is broken
  at the row. A line is runs of `{text, attrs, spot-or-0}` so the painter
  can start and stop SGR at run boundaries and the spot table can map a
  row back to what is on it. **A flowed row is trimmed of the trailing spaces
  the renderer itself left** — a cell separator that turned out to end a
  row, an indent under nothing — which paint as nothing and would widen a
  selection over nothing. Inside `pre` they are the author's and they
  stay.
- **An indent, a pen or `pre` is given back only after the rows it governs
  are down.** A word and a row are both still held when an element's
  children are finished, so restoring first wraps that last word at the
  outer margin, or paints the space before it in the wrong ink, or trims
  spacing the author typed. This is the renderer's one recurring trap and
  the harness has a case for each shape of it.

**The fold: UTF-8 → the glass.** The terminal draws Latin-1 by default
(`ESC ( U` selects CP437 for art; the face stays in Latin-1). The rule,
applied per code point with `os64_utf8_decode`:
a code point below 256 is its byte; a short table of the punctuation the
web is full of maps to a byte that reads right (curly quotes → `'`/`"`,
en/em dash → `-`, ellipsis → `...`, non-breaking space → space, bullet →
`*`, the arrows → `<`/`>`/`^`/`v`, the trademark/copyright to `(tm)`/
`(c)` when Latin-1 lacks them — it has `©`); anything else draws `?`.
`?` and not a blank, because a blank hides that something was there
(LIBHTML.md's "reads as missing rather than as corruption", the other way
round). **Two kinds of code point are the exception to "below 256 is its
byte" (departure).** The C0 and C1 control ranges draw `?` like anything
else undrawable: a control byte reaching the terminal is a stranger
steering the glass rather than writing on it. And the code points that
are invisible BY DEFINITION — a soft hyphen, the zero-width joiners, a
word joiner, a byte-order mark — draw nothing at all, because they mark
where a word MAY break, and Latin-1's soft hyphen at 0xAD would put a
dash in the middle of a word that has none. This table is the FACE's and
lives beside its renderer; the decode helpers are libos64's
(`os64_utf8_decode` / `os64_utf8_encode`, `str.h`), because every future
consumer of the tree needs them.

**The navigator.** A history stack of addresses with the scroll position
and selection at the time of leaving, exactly `history_push`'s shape;
`b` (and Backspace) pops it and refetches — a cache is the graphical
browser's problem.

**THE ARROWS WALK THE SPOTS** (Chris, 2026-09-11, on seeing the first cut
do it the other way): a browser picks a link by pointing at it, which is
the gopher client's ruling and the one a person's fingers arrive with.
Up and Down move the selection to the previous or next spot; `n`/`p` and
Tab/Shift-Tab are the same move under other names; PAST the last spot in
that direction the same key SCROLLS, which is what keeps the arrows
useful on the prose below the last link and on a page with no links at
all. `PgUp`/`PgDn`/space/`Home`/`End` move the page by screenfuls and
take the selection with them — a choice left off the screen would make
the next arrow jump backwards, so it is replaced by the first spot on the
new screen or by nothing.

Enter (and Right) DOES THE SELECTED THING, which is one key because to
the person pressing it there is one question: a link is followed, a box
is opened to type in, a tick box is ticked, a list steps to its next
option, a button sends its form. **A link into the page you are already
on is a MOVE, not a fetch**: every element carrying an `id`, and every
old-style `<a name>`, records the row its content opens on, so a `#name`
is answered by scrolling there. A page that does not carry the name says
so and stays where it is. Typing a number then Enter does the same
to the spot wearing that number. `g` prompts for an address (a bare
`host/path` gets `http://` in front, the way gopher reads a bare host);
`r` refetches; `?` shows the keys; `q` quits, after asking, because
leaving throws away the session and its history and `q` sits one key from
the arrows. Left goes back, lynx's arrangement and the gopher client's.
A `gopher://` link is not the face's in the first cut (libfetch's gopher
scheme is booked); say so in the status row rather than failing quietly.

**Four pens, and each answers a different question.** A link is cyan —
"you can go there". A form control is green — "you can put something
here", a different promise. The selection is black on cyan, where a text
interface has kept its highlight since Turbo Vision, and deliberately not
the black-on-white the title and status bars wear, or it reads as a third
piece of furniture instead of as the cursor. Everything else is the
terminal's own ink, because a page is mostly prose and painted prose is
harder to read. Underline is asked for where a page says italic, which is
what italic means on a terminal; this glass has no underline and consumes
the request.

**Filling something in, and sending it.** A box is opened with Enter and
edited on the status row, starting from whatever it already holds; Enter
again keeps the text and Escape leaves it as it was. **A value is stored
as UTF-8** — which is what the page put there and what a server that sent
a UTF-8 page expects back — and converted at each end: folded to Latin-1
to be shown, encoded again as the typed bytes are taken. **The values
live BESIDE the page, indexed by spot number**, because a re-wrap throws
the page away and builds another, and what was typed into a search box
has to survive the window changing width; the walk is deterministic for
one tree, so the same number carries the same value into the next render.

Sending builds the address in `render.c` (`wend_form_url`) rather than in
the session, because it is pure computation over the page and that is
where the harness can check what would go on the wire. The form's action,
or the page itself when it names none; the query REPLACED, not appended
to, which is what a GET form does; the hidden fields first, then every
successful control in document order — a box that is not ticked sends
nothing, a ticked one with no value of its own sends `on`, a list sends
its option's VALUE rather than the words shown for it, and of two buttons
only the one pressed says so — and **the button that was pressed may
overrule its form**, because the standard lets it carry its own action and
its own method. The METHOD matters most to a browser that sends only one
of them: a GET form with a `formmethod=post` button is a POST, and sending
it as a GET would put whatever it collected into an address that servers
and proxies write down. An action's `#name` is kept beside the address the
same way a link's is, and applied once the answer arrives.

**A form with exactly ONE THING TO ANSWER
sends itself when you finish that thing**, because there is nowhere else
in it to go and stopping to hunt for a button is the step nobody expects.
Anything else to fill in — a second box, a tick, a list — and it waits
for its button, since sending early would send the rest at their defaults
before a person working down the page ever reached them.

**A form off an HTTPS page whose action is plain `http` asks first.**
libfetch's downgrade callback cannot see that one: it judges the
redirects INSIDE a fetch, and this fetch begins at http, so nothing in
the library learns where the values came from. What is being sent is what
somebody typed, which makes it a stronger case for asking than an
ordinary downgrade, not a weaker one. The question is asked of the
address the PAGE came from and not of its base, because `<base href>` can
move the base to http while the page that collected the values stays
encrypted. And **a security question drops type-ahead**: keys struck
while a page was loading are held for whoever asks next, and a `y` meant
for something else must not answer a question it never saw.

**Errors are the library's sentence, in the status row.** Every libfetch
refusal has `os64_fetch_reason`; a TLS refusal now names the alert or the
certificate problem (Quinn's #96). A page that will not fetch leaves the
previous page on screen with the reason under it; nothing clears a page
to show an error.

**Proof.** The renderer is pure computation — a tree in, lines out — so
it gets the host harness the two libraries got: `tools/test_wend_host.sh`
feeds the saved corpus pages (`tools/html_corpus/*.html`) through libhtml
and the renderer and diffs against checked-in dumps beside them, so a
rendering change is a reviewable diff to a page and not "it looks
different". The fold gets its own cases and a sweep of the whole code
space, the wrapper gets its edges written down as markup-in/rows-out, and
an allocation failure is injected at every step of one page. What a FORM
would ask for is pure computation too, so the suite checks the address
before any wire carries it. VERIFICATION.md § wend acceptance carries the
run commands, what the guest was driven through, and the four defects the
harness caught before the OS ran a byte.

**Review tier.** The parser and the fetch have been through the
gauntlet; the face is app code and is reviewed here (Fable) and merged
after Chris's test, per CLAUDE.md's "match the reviewer to the risk". A
Codex round is Chris's call.

**Booked before the first line:**

| What | Why deferred | Trigger |
|---|---|---|
| POST forms | libfetch sends no request body, and that is fetch machinery — Fable-tier by the campaign's own split. A form that posts is refused BY NAME rather than turned into a GET, because a login quietly sent as a query puts a password in somebody's server log | the first thing worth doing that only posts |
| A file-upload control | it is a POST with a body made of parts, so it waits on the row above and on a file picker this browser has no screen for | a page worth uploading to |
| Editing longer than a status row | a box is edited on the bottom row, so a long value is a scrolling window onto itself; fine for a query, thin for a comment | the first time somebody writes prose into a page |
| `gopher://` links | libfetch's gopher scheme is booked; the gopher client still owns the protocol | the browser's first gopher link |
| Column-aligned tables | rows read fine for the old web's layout tables; alignment is layout, the graphical browser's boss | a data table that is unreadable as rows |
| CP437 / a second charset on the glass | the face is Latin-1; art pages are the gopher client's | a page whose meaning needs box drawing |
| A page cache for `b` | refetching is honest and simple; a cache is a lifetime problem for later | when back-and-forth on a slow link hurts |
| Cookies | libfetch's row; the jar is the navigator's when it comes | the first site that will not show a page without one |

## NOT in the ladder's lane (Fable-tier — do not start these)

- **Any change to tcp.c's protocol behavior**: window
  scaling (RFC 1323), SACK, Nagle, LISTEN.
  Counter-driven, concurrency-heavy, and the review tier is Fable +
  Codex-by-Chris's-hand. The eyes exist so these are paid at the right
  moment, not speculatively.
- **libfetch itself** (LIBFETCH.md). The extraction is done; what stays
  Fable-tier is any change to the FETCH machinery — streams, heads,
  bodies, buffer ownership across a read boundary — which is where
  lifetime bugs live. The URL parser was never that and has gone to
  libos64 already; see 4(b).
- Anything touching park loops, the scheduler, or signal delivery.

## Process (the campaign's working agreement)

- Small PRs off `userland`; Chris branches each feature off the last and
  merges forward as reviews complete.
- Chris tests before ANY commit; nothing merges without Fable's review
  (standing ruling).
- Codex auto-review is OFF (2026-09-01). Requesting a Codex round is
  CHRIS'S act alone — never post `@codex review`, never assume a round.
- Harness: `make run-net` (virtio + slirp + pcap; plain `make run` has no
  NIC). Guest reaches the host's loopback at 10.0.2.2; slirp NATs outbound
  to the real internet. The valet test dialect: `GET <name>\n` →
  `OK <len> <crc32-hex8>\n` + bytes (CRC-32/ISO-HDLC = zlib.crc32). The
  morgue gives you 15 seconds to `cat /sys/net/tcp` after anything dies —
  use it.
- The labor division holds: protocol clients and library plumbing are
  model work; the pretty-printers and utilities (netstat, and whatever
  else the ladder tempts) are Chris's joy. Build the seam, hand him one
  example, get out of the way.
