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
   TLS. Until that slice: plain-HTTP sources (neverssl.com, textfiles.com,
   FrogFind, mirrors) and, if wanted, a TLS-terminating proxy on the valet.
   Rolling our own was considered and rejected on merit: the hazard is
   thirty years of side-channel and oracle attacks, and surviving them
   teaches no kernel lessons.
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

2. **whois (port 43) — next; Opus-suitable.** First conversation with a
   machine nobody in this house administers. `userland/apps/whois` → /bin
   (a person runs it). v1: dial `tcp!<server>!43` (the dial string
   resolves hostnames already — os64get's `build_dialstring` is the
   pattern), send `<query>\r\n` (CRLF, not bare LF — 1970s protocols mean
   it), read until EOF, print raw. Default server `whois.iana.org`, `-h
   <server>` to override; referral-chasing (IANA → registry → registrar)
   is EXPLICITLY deferred — book it, don't build it. Errors through the
   house refusal vocabulary (os64get's `dial_reason` is the model).
   Verification: `make run-net` — slirp NATs outbound TCP to anything the
   host can reach, so the guest can whois the actual IANA from QEMU. Watch
   /sys/net/tcp while it happens; that's what it's for.

3. **os64get learns URLs — Opus-suitable, in INCREMENTS (each its own
   commit, maybe its own PR).** Bare-name operands keep meaning the valet
   (dialect untouched); an operand shaped like `http://host[:port]/path`
   means the world.
   - (a) URL parse + HTTP/1.0 GET: request line, `Host:` header, status
     line + headers parse, `Content-Length` read (read-until-close as the
     fallback the length-less server forces).
   - (b) chunked transfer-encoding.
   - (c) redirects: 301/302/307/308, `Location:`, hop cap ~5, refuse
     https:// targets HONESTLY (name the reason: no TLS yet).
   - (d) `Content-Encoding: gzip` — WAITS for the DEFLATE arc (Codex is
     designing tar/gzip separately; one decoder, three customers).
   - (e) `Range:`/resume — later, wants a consumer first.
   DESIGN CONSTRAINT: keep the HTTP machinery in cleanly separable
   functions — the extraction into a shared library (FreeBSD libfetch's
   shape; future customers: gopher client, the browser) is a LATER,
   Fable-reviewed slice. Do not build the .so speculatively
   (consumer-driven growth, the house rule).
   Verification: a local `python3 -m http.server` behind the harness for
   deterministic tests; neverssl.com / textfiles.com / frogfind.com for
   the real world.

4. **gopher client (port 70).** Fetch-and-close: selector + CRLF, read to
   EOF; menus are lines of `<type>\t<display>\t<selector>\t<host>\t<port>`.
   The numbered-menu UI is the line-mode browser's face being born — this
   slice is where the browser's UI lineage starts, so hold it until 2–3
   have landed and the shape can be discussed. Gopherspace is alive:
   Floodgap, SDF, magical.fish (Chris can vouch for the games menu).

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

Rising in parallel, unscheduled: a **chaos rig** on the host/valet
(toxiproxy-shape: injected latency, loss, stalls, mid-stream resets) so the
internet's weather becomes a reproducible harness fixture.

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
