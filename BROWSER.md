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
   store at `/etc/certs` on the conf ladder. The kernel never learns about
   TLS. Rolling our own was considered and rejected on merit: the hazard is
   thirty years of side-channel and oracle attacks, and surviving them
   teaches no kernel lessons.
   **THE STOPGAP THIS RULING NAMED IS BUILT (2026-09-02): `tools/tlsproxy.py`.**
   Chris asked for it after reading a badly-named test route (`/tohttps`,
   since renamed) as a promise that the server would make the TLS call for
   him — which is exactly what this line had sanctioned and nobody had yet
   written. It speaks the proxy dialect every proxy has spoken since CERN's
   in 1994 (the whole URL in the request line, "absolute-form", RFC 7230
   §5.3.2), fetches over TLS with the host's own trust store, and hands the
   answer back in plain HTTP. os64get picks it up from `$https_proxy`, with
   `$http_proxy` and `$no_proxy` beside it; **the scheme picks the variable**,
   which is load-bearing rather than tidy — one setting covering both
   silently rerouted the local test fetches through a machine that could not
   reach them (10.0.2.2 means nothing off the guest), and 502 was the first
   anyone knew. **Verified: `https://example.com/` and the 137582-byte
   `https://www.rfc-editor.org/rfc/rfc1945.txt` both arrived byte-identical
   to curl's copies** — an OS with no TLS reading the HTTP/1.0 specification
   over TLS. What it COSTS is printed on every proxied fetch and never
   softened: the proxy terminates the TLS, so it holds the page in the clear
   and the leg from os64 to it is plain text. Public reading, not secrets.
   Plain-HTTP sources (neverssl.com, textfiles.com, FrogFind, mirrors) still
   work with no proxy at all.
2. **Layout.** Block flow, inline flow, the box model. Distant; the ladder
   climbs there via the gopher/line-mode client's UI. Not yet designed —
   deliberately.

## The stance: the ladder is a TCP shakedown wearing costumes

Our TCP (`kernel/src/driver/net/tcp.c`) is honest, LAN-calibrated v1 —
stop-and-wait send, out-of-order dropped, fixed 64KB window, fixed 1s RTO.
Every omission is a decision stated in tcp.h and booked. The P5's fetch ran
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
     machinery lives in `apps/os64get/http.{c,h}` — the seam a libfetch
     would eventually be sawn along, not the library. Two rulings the
     increment forced, both written down where they bind: a URL fetch does
     NOT route through `os64get.conf` (whose last rule is `* = /bin` — a
     web page would install as a program) and keeps NO archive (a download
     is not an install); and a framing or coding os64get cannot undo is
     REFUSED BY NAME rather than written to disk, because a `.html` full of
     chunk lengths looks like a successful download. Proof:
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
     request, and an https target is a dead end or an ordinary hop
     depending only on that. And a new exit code, **15**, for a road that
     did not arrive (hop cap, a circle, an unreachable target) — distinct
     from 5, the server's final answer about the page, because the thing to
     change is on a different side. `http_url_absolute` grew into RFC 3986
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
   - (d) `Content-Encoding: gzip` — stream through libgzip with a response
     expansion cap, keep the output provisional until `OS64_GZIP_DONE`, and
     reject its explicit trailing-data result; libpng uses the same shared
     DEFLATE decoder beneath PNG's zlib framing.
   - (e) `Range:`/resume — later, wants a consumer first.
   DESIGN CONSTRAINT: keep the HTTP machinery in cleanly separable
   functions — the extraction into a shared library (FreeBSD libfetch's
   shape; future customers: gopher client, the browser) is a LATER,
   Fable-reviewed slice. Do not build the .so speculatively
   (consumer-driven growth, the house rule).
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
   - (b) **The client, `/bin/gopher` — its own program, not a mode of
     os64get.** The protocols differ, but the KIND differs more: os64get
     is a one-shot fetcher that writes a file and exits with a code, and
     this is an interactive session with a screen and a history stack.
     What they share is the dial and the URL parser — `gopher://` URLs are
     how gopherspace is written down, so the client takes one (Chris,
     emphatically, 2026-09-03), and that makes the URL half of `http.c`
     its second customer and the first honest occasion to ask whether it
     should move. Bindings are lynx's, whose path this re-walks in the
     right order: Up/Down to move, Enter to follow, LEFT ARROW for back,
     `q` to quit. Item types: `0` text through the pager, `1` menu, `7`
     search (prompt, resend `selector\tquery`), `9`/`I`/`g`/`s` saved to a
     file, `i` shown but not selectable, `3` the server's error shown as
     one, and **anything unknown shown but NOT followable** — guessing
     what a type means is how you download a thing that is not what it
     said. Type `h` carries a `URL:http://…` link and is **handed to
     os64get**, which is only as good as os64get's exit codes are precise
     (Chris's condition, and the reason 3(a)–(c)'s code table was worth
     the care it got: 5 refused, 7 short, 13 bad address, 14 a coding it
     cannot read, 15 a redirect it could not follow).
     A MENU IS A STRANGER'S BYTES ON YOUR TERMINAL, and (a) makes the
     terminal obey more of them — so os64get's rule travels with the
     handoff: control bytes are refused once, where the line is parsed,
     never escaped at each print.
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

6. **FTP, PASV mode (port 21).** The multi-connection shakedown: control
   channel + a data connection per transfer, ephemeral-port churn — the
   browser's connection pattern rehearsed before the browser exists.

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

## NOT in the ladder's lane (Fable-tier — do not start these)

- **Any change to tcp.c's protocol behavior**: reassembly buffer, window
  scaling (RFC 1323), RTT-measured RTO (Jacobson), send window, LISTEN.
  Counter-driven, concurrency-heavy, and the review tier is Fable +
  Codex-by-Chris's-hand. The eyes exist so these are paid at the right
  moment, not speculatively.
- **TLS integration** and the libfetch extraction (seams cross review
  tiers).
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
