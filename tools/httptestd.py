#!/usr/bin/env python3
r"""httptestd — a deterministic HTTP server for driving os64get's URL half.

BROWSER.md's verification note calls for "a local python3 -m http.server
behind the harness"; this is that, with the awkward cases added, because the
answers worth testing are the ones a well-behaved library will not produce on
request: a reply with no Content-Length, a connection cut mid-body, a 301, a
content coding os64get must refuse rather than write to disk.

Every route's bytes are fixed by a seed, so the file the guest downloads can
be compared against the file this server meant to send — host-side, byte for
byte, after the guest has exited.

    python3 httptestd.py [--port N] [--dump DIR]

`--port` is spelled the way os64serve.py spells it, deliberately: these two
sit in the same directory, get started the same way from the same prompt, and
one of them taking `--port 6464` while the other took a bare number is the
kind of difference you rediscover at 1am. A bare number still works.

WHERE TO RUN IT DEPENDS ON WHICH MACHINE IS FETCHING, and this is the part
that costs an afternoon if it is not said out loud:

  - QEMU, from WSL2: run it here. The guest reaches the host's loopback at
    10.0.2.2 through slirp, so `os64get http://10.0.2.2:8080/hello.txt` in
    the guest finds a listener started from any WSL2 shell.
  - THE P5, or anything else on the LAN: run it on the WINDOWS side, exactly
    as os64serve.py's header explains for the same reason — WSL2 lives behind
    a NAT of its own, so a listener started inside it is not reachable from
    the room. Windows reads the file over \\wsl$ and serves it in place:

        cd /mnt/c/temp
        python3.exe '\\wsl$\<distro>\home\<you>\src\os64\tools\httptestd.py'

    Windows will prompt about the firewall the first time Python binds — say
    yes, or the P5's connections are dropped before this program sees them.
    `wsl -l` names the distro.

Routes, and what each is FOR:

    /               this text                              the map, served
    /hello.txt      a small text file, Content-Length      the happy path
    /big.bin        1 MiB, Content-Length                  the 64KB write loop
    /slow.txt       Content-Length, dribbled out           the progress meter
    /nolength.txt   HTTP/1.0, no length, close-delimited   RFC 1945 framing
    /dir/           a page at a path ending in '/'         the index.html rule
    /query?x=1&y=2  a page behind a query string           the query survives
    /missing        404                                    a refusal
    /redirect       301 -> /hello.txt                      the ordinary hop
    /redirect/N     301 -> /redirect/N-1, then /hello.txt  a trail, and the hop cap
    /redirect-page  302 -> hello.txt (page-relative)        RFC 3986 §5.2's merge
    /redirect-303   303 -> /hello.txt                      "GET this instead"
    /redirect-307   307 -> /hello.txt                      the method-keeping pair
    /redirect-308   308 -> /hello.txt                      ditto, permanent
    /redirect-loop  302 -> itself                          a circle, named as one
    /redirect-none  302 with no Location                   nowhere to go
    /redirect-300   300 + Location                         a choice, not an order
    /redirect-305   305 + Location                         "use my proxy": refused
    /redirect-https 302 -> an https:// address              the honest TLS answer
    /redirect-tricky 302 -> /hello.txt;reboot               a path, never a command
    /redirect-relative 302 -> //example.com/elsewhere       scheme-relative, off-host
    /redirect-broken 302 -> /bad path                       unusable, said so
    /redirect-mail  302 -> mailto:...                       not a page at all
    /chunked        200000 bytes chunked, ext + trailer    the framing comes off
    /dots/..        a URL with no usable basename          DEST must still be honored
    /chunked-cut    chunked, terminator never sent         truncation must not pass
    /framing-spaced that framing, plus a blank before the colon
    /coding-chain   Transfer-Encoding: gzip, chunked       a coding nothing decoded
    /framing-hidden that framing, padded past a line cap    refused, not dropped
    /gzipped        Content-Encoding: gzip                 refused, not written
    /cut            half the promised body, then hangs up  a short body is a failure
    /junk           not HTTP at all                        a bad header line
    /reason-latin1  404 with a Latin-1 reason phrase       high bytes are glyphs
    /stall          half a head, then 45s of silence       the idle deadline

NOTE ON /redirect-https: it REDIRECTS to an https address, it does not fetch
one. The first name for it was `/tohttps`, which read to Chris as "the server
will make the TLS call for me and hand back the result" — a reasonable reading
of a bad name, and a description of something else entirely (BROWSER.md
sanctions a TLS-terminating proxy as the stopgap until os64 borrows a TLS).
This route only produces the 302 that lets os64get prove it refuses an https
target honestly instead of writing something wrong to disk.
"""

import random
import socketserver
import sys
import time
import zlib

USAGE = "python3 httptestd.py [--port N] [--dump DIR]"

# How much request head a connection may send before it is refused — the
# same three bounds tlsproxy.py keeps, for the same reason its Handler has a
# timeout: the port is on the LAN.
MAX_LINE = 65536
MAX_HEAD = 262144
MAX_HEAD_LINES = 200


def visible(text):
    """`text` with every byte a terminal would OBEY spelled as an escape —
    C0 controls, DEL and the C1 range as `\\xHH`, backslash as `\\\\` — so a
    request line off the LAN is logged as glyphs and cannot clear or rewrite
    the operator's screen. tlsproxy.py keeps the same rule for the same
    reason: both ports are open to the room. (Codex review round 5,
    2026-09-03.)"""
    out = []
    for ch in text:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif code < 0x20 or 0x7F <= code < 0xA0:
            out.append(f"\\x{code:02x}")
        else:
            out.append(ch)
    return "".join(out)

HELLO = b"hello from the host, over HTTP.\n"
DIRPAGE = b"<html><body><h1>a directory</h1></body></html>\n"
QUERYPAGE = b"the query survived the trip\n"
NOLENGTH = b"no Content-Length here; the close is the length.\n" * 40
SLOW = b"dribbled out, a piece at a time, so the meter has something to do.\n" * 700


def big_bytes(size=1024 * 1024, seed=0x05064A17):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


BIG = big_bytes()
CHUNKED = BIG[:200000]
INDEX = (__doc__.strip() + "\n").encode("utf-8")


def chunked(body, sizes):
    """`body` in chunked transfer coding, cut at `sizes` (cycled), with a
    chunk extension on the first piece and one trailer field — the optional
    parts of the grammar, so a client that only reads the mandatory ones is
    found out here rather than by the first real server that uses them."""
    out = b""
    at = 0
    i = 0
    while at < len(body):
        n = min(sizes[i % len(sizes)], len(body) - at)
        size = f"{n:x}" + (";os64=first" if i == 0 else "")
        out += size.encode() + b"\r\n" + body[at:at + n] + b"\r\n"
        at += n
        i += 1
    return out + b"0\r\nX-Body-Bytes: %d\r\n\r\n" % len(body)


def head(status, reason, headers, version="HTTP/1.1"):
    text = f"{version} {status} {reason}\r\n"
    for name, value in headers:
        text += f"{name}: {value}\r\n"
    return (text + "\r\n").encode("latin-1")


class Handler(socketserver.StreamRequestHandler):
    # A PER-CONNECTION READ TIMEOUT, and a bounded head drain, for the same
    # reason tlsproxy.py has both: this binds every interface (the P5 case
    # needs it to), so anything on the LAN can open a connection and then
    # send nothing — or send short header lines forever. Either parks a
    # worker thread and a file descriptor for good, and enough of them and
    # the guest's fetches stop connecting. socketserver applies `timeout` to
    # the socket in setup(). (Codex review round 4, 2026-09-03.)
    timeout = 30

    # A fetch-and-close client gets a fetch-and-close server: every reply
    # ends by hanging up, whatever else frames it — os64get asks for the
    # close and does not speak keep-alive.
    def handle(self):
        try:
            request = self.read_head()
        except OSError as error:
            # A timeout or a reset while the request was still arriving;
            # nothing to answer and nobody waiting to read one.
            print(f"  !! request head never finished: {error}", flush=True)
            return
        if request is None:
            return

        parts = request.split()
        path = parts[1] if len(parts) >= 2 else "/"
        print(f"  {visible(request)}", flush=True)

        route = path.split("?", 1)[0]
        query = path.split("?", 1)[1] if "?" in path else ""

        if route == "/":
            # The map, served by the territory: this module's own docstring,
            # so the page a client reads and the source a person reads are one
            # text and cannot disagree about what a route is for.
            self.send(head(200, "OK", [("Content-Type", "text/plain; charset=utf-8"),
                                       ("Content-Length", len(INDEX))]) + INDEX)
        elif route == "/hello.txt":
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Length", len(HELLO))]) + HELLO)
        elif route == "/big.bin":
            self.send(head(200, "OK", [("Content-Type", "application/octet-stream"),
                                       ("Content-Length", len(BIG))]) + BIG)
        elif route == "/slow.txt":
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Length", len(SLOW))]))
            for start in range(0, len(SLOW), 1400):
                self.send(SLOW[start:start + 1400])
                time.sleep(0.05)
        elif route == "/nolength.txt":
            self.send(head(200, "OK", [("Content-Type", "text/plain")],
                           version="HTTP/1.0") + NOLENGTH)
        elif route == "/dir/":
            self.send(head(200, "OK", [("Content-Type", "text/html"),
                                       ("Content-Length", len(DIRPAGE))]) + DIRPAGE)
        elif route == "/query":
            body = QUERYPAGE + query.encode("latin-1") + b"\n"
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Length", len(body))]) + body)
        elif route == "/redirect":
            self.send(head(301, "Moved Permanently",
                           [("Location", "/hello.txt"), ("Content-Length", 0)]))
        elif route.startswith("/redirect/"):
            # A TRAIL OF N HOPS, so the same server can prove both halves of
            # the rule: a chain shorter than the cap arrives, and one longer
            # than it stops with the address it stopped at. The body is a
            # courtesy page a browser would show and a fetcher must discard —
            # sent with a length so a client that reads it instead of the
            # real file is caught by the size, not by luck.
            rest = route[len("/redirect/"):]
            left = int(rest) if rest.isdigit() else 0
            where = f"/redirect/{left - 1}" if left > 1 else "/hello.txt"
            page = b"<html><body>this way</body></html>\n"
            self.send(head(301, "Moved Permanently",
                           [("Location", where), ("Content-Type", "text/html"),
                            ("Content-Length", len(page))]) + page)
        elif route == "/redirect-page":
            # RELATIVE TO THE PAGE, the form RFC 3986 §5.2.3's merge exists
            # for: `hello.txt` is not an address until it is joined to the
            # directory of the page that said it.
            self.send(head(302, "Found",
                           [("Location", "hello.txt"), ("Content-Length", 0)]))
        elif route in ("/redirect-303", "/redirect-307", "/redirect-308"):
            code = int(route[-3:])
            reason = {303: "See Other", 307: "Temporary Redirect",
                      308: "Permanent Redirect"}[code]
            self.send(head(code, reason,
                           [("Location", "/hello.txt"), ("Content-Length", 0)]))
        elif route == "/redirect-loop":
            # Pointing at itself. The hop cap would eventually stop this, five
            # requests later and in words about counting; a client that
            # notices should say what actually happened.
            self.send(head(302, "Found",
                           [("Location", "/redirect-loop"), ("Content-Length", 0)]))
        elif route == "/redirect-none":
            self.send(head(302, "Found", [("Content-Length", 0)]))
        elif route == "/redirect-300":
            # A LIST TO CHOOSE FROM. Its Location is the server's preference,
            # not an instruction, and a fetcher choosing on somebody's behalf
            # is guessing.
            self.send(head(300, "Multiple Choices",
                           [("Location", "/hello.txt"), ("Content-Length", 0)]))
        elif route == "/redirect-305":
            # "Route your traffic through this machine" — from a stranger.
            # Every browser dropped 305 for that reason; os64get must not
            # obey it either.
            self.send(head(305, "Use Proxy",
                           [("Location", "http://10.0.2.2:9/"), ("Content-Length", 0)]))
        elif route == "/redirect-dots":
            # An absolute Location with dot segments. §5.2.2 squashes them in
            # every branch; sent raw, `/dir/../hello.txt` asks this server
            # what `..` means, and this server (like many) says 404.
            self.send(head(302, "Found",
                           [("Location", f"http://10.0.2.2:{self.server.server_address[1]}/dir/../hello.txt"),
                            ("Content-Length", 0)]))
        elif route == "/redirect-dead":
            # A hop to a port nothing listens on: a road that does not
            # arrive, which is exit 15 — not 3, which names the address that
            # was TYPED as unreachable.
            self.send(head(302, "Found",
                           [("Location", "http://10.0.2.2:1/nothing"),
                            ("Content-Length", 0)]))
        elif route == "/redirect-two":
            # Two different Locations: a redirect with no destination.
            self.send(head(302, "Found",
                           [("Location", "/hello.txt"), ("Location", "/missing"),
                            ("Content-Length", 0)]))
        elif route == "/redirect-mail":
            self.send(head(302, "Found",
                           [("Location", "mailto:someone@example.com"),
                            ("Content-Length", 0)]))
        elif route == "/redirect-https":
            self.send(head(302, "Found",
                           [("Location", "https://example.com/"), ("Content-Length", 0)]))
        elif route == "/redirect-relative":
            # Scheme-relative: a different host, the scheme inherited. It is
            # resolved BEFORE it is judged, or nothing is being judged — and
            # following it leaves this server for a real one, which is the
            # point: only the guest's own dial says whether that host answers.
            self.send(head(302, "Found",
                           [("Location", "//example.com/elsewhere"), ("Content-Length", 0)]))
        elif route == "/redirect-broken":
            # Root-relative and malformed: resolved, it is a URL os64get
            # refuses, and it must say so rather than dial something it made
            # up out of the readable part.
            self.send(head(302, "Found",
                           [("Location", "/bad path"), ("Content-Length", 0)]))
        elif route == "/redirect-tricky":
            # A Location that is a legal path AND a husk command separator.
            # It is FOLLOWED, as a path, and this server has nothing there —
            # so the fetch ends in an honest 404 rather than in `reboot`. Any
            # message that offers an address as a command to type must quote
            # it. (Codex review round 5, PR #52.)
            self.send(head(302, "Found",
                           [("Location", "/hello.txt;reboot"), ("Content-Length", 0)]))
        elif route == "/framing-hidden":
            # A framing header PADDED past a client's line limit. RFC 7230 lets
            # any amount of optional whitespace follow the colon, so a server
            # picks how long its own headers are — and a client that drops long
            # lines to be tolerant will miss this one and publish raw chunk
            # framing as the file. os64get must refuse it. (Codex P1, PR #52.)
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Transfer-Encoding", " " * 3000 + "chunked")]))
            self.send(b"20\r\nthirty-two bytes of chunked body\r\n0\r\n\r\n")
        elif route == "/dots/..":
            # A URL whose last path segment is unusable as a file name. With
            # a DEST naming a file, the fetch must still work — the basename
            # is only wanted when nothing else says where the bytes go.
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Length", len(HELLO))]) + HELLO)
        elif route == "/chunked-cut":
            # A chunked body that never sends its terminating chunk, then
            # hangs up. Through a proxy that strips the framing and states no
            # length, this arrives downstream as a clean EOF and publishes as
            # a COMPLETE file — truncation wearing success. (Codex round 2.)
            self.send(head(200, "OK", [("Content-Type", "application/octet-stream"),
                                       ("Transfer-Encoding", "chunked")]))
            self.send(b"10000\r\n" + BIG[:65536] + b"\r\n")
        elif route == "/framing-spaced":
            # The framing header padded past a client's line cap AND carrying
            # the blank before the colon that the SHORT form is already
            # refused for. Both shapes must earn the same answer, or a server
            # picks which parser it faces by padding a line. Written as raw
            # bytes because head() cannot spell a malformed name.
            # (Codex round 3.)
            self.send(b"HTTP/1.1 200 OK\r\n"
                      b"Content-Type: text/plain\r\n"
                      b"Transfer-Encoding :" + b" " * 3000 + b"chunked\r\n"
                      b"\r\n")
            self.send(b"20\r\nthirty-two bytes of chunked body\r\n0\r\n\r\n")
        elif route == "/coding-chain":
            # A legal transfer-coding CHAIN. http.client de-chunks only when
            # the header is exactly "chunked", so this arrives still coded and
            # still framed — and a proxy that drops the header on sight would
            # hand the raw framing on as a file.
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Transfer-Encoding", "gzip, chunked")]))
            self.send(b"20\r\nthirty-two bytes of chunked body\r\n0\r\n\r\n")
        elif route == "/chunked":
            # The grammar's whole vocabulary in one reply: sizes from one
            # byte to past the client's read buffer, an extension, a trailer.
            # The body is BIG's first 200000 bytes, so `--dump` has the file
            # to compare against.
            self.send(head(200, "OK", [("Content-Type", "application/octet-stream"),
                                       ("Transfer-Encoding", "chunked")]))
            self.send(chunked(CHUNKED, [1, 7, 100, 4096, 9000, 65536, 3]))
        elif route == "/gzipped":
            body = zlib.compress(HELLO)
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Encoding", "gzip"),
                                       ("Content-Length", len(body))]) + body)
        elif route == "/cut":
            self.send(head(200, "OK", [("Content-Type", "application/octet-stream"),
                                       ("Content-Length", 100000)]))
            self.send(BIG[:40000])
        elif route == "/reason-latin1":
            # A reason phrase in Latin-1 (obs-text, legal), which os64get
            # prints. Every byte a program prints must be a glyph the console
            # can draw; this route is how that is checked from ring 3.
            self.send(head(404, "Nicht gefunden - déjà vu üñå", [("Content-Length", 0)]))
        elif route == "/junk":
            self.send(b"220 this is not an HTTP server\r\n\r\nwhatever\n")
        elif route == "/stall":
            # Half a head, then silence for longer than os64get's idle
            # deadline. The fetch must FAIL on its own, saying the server
            # went silent, not sit until someone presses Ctrl+C.
            self.send(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n")
            time.sleep(45)
        else:
            body = f"no such thing here: {route}\n".encode("latin-1")
            self.send(head(404, "Not Found", [("Content-Type", "text/plain"),
                                              ("Content-Length", len(body))]) + body)

    def read_head(self):
        """The request line, with the headers after it drained and bounded.
        Returns None when the head is more than this server will read; a
        client that keeps sending short header lines forever is as good at
        parking a worker as one that sends nothing at all."""
        request = self.rfile.readline(MAX_LINE).decode("latin-1", "replace").strip()
        used = 0
        lines = 0
        while True:
            line = self.rfile.readline(MAX_LINE)
            if line in (b"\r\n", b"\n", b""):
                return request
            used += len(line)
            lines += 1
            if used > MAX_HEAD or lines > MAX_HEAD_LINES:
                print("  !! more request head than this server will read", flush=True)
                self.send(head(431, "Request Header Fields Too Large",
                               [("Content-Length", 0)]))
                return None

    def send(self, data):
        try:
            self.wfile.write(data)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    # Don't wait for handler threads at shutdown. They are daemons and die
    # with the process; without this, Ctrl-C during a deliberately slow route
    # waits for that route to finish dribbling.
    block_on_close = False

# CTRL-C ALREADY WORKS HERE, and it is worth saying why, because the valet
# beside this needed a deliberate fix for the same thing. WinSock's blocking
# accept() does not return for a console signal, so a raw accept loop on
# Windows only notices Ctrl-C when the next client arrives — read by everyone
# as "does not stop". socketserver.serve_forever() does not block in accept:
# it waits in a selector with a poll interval and comes back every half
# second whether or not anyone knocked, which is when Python gets to raise
# the KeyboardInterrupt. Same cure, already built in.

def dump_bodies(where):
    """Write the deterministic bodies where a host-side comparison can find
    them: the point of a fixed seed is that the file the guest downloaded can
    be diffed against the one this meant to serve."""
    import pathlib
    out = pathlib.Path(where)
    out.mkdir(parents=True, exist_ok=True)
    for name, body in (("hello.txt", HELLO), ("big.bin", BIG), ("slow.txt", SLOW),
                       ("nolength.txt", NOLENGTH), ("index.html", DIRPAGE),
                       ("chunked", CHUNKED)):
        (out / name).write_bytes(body)
    print(f"bodies written to {out}")


def bad(message):
    print(f"httptestd: {message}", file=sys.stderr)
    print(f"usage: {USAGE}", file=sys.stderr)
    raise SystemExit(2)


def parse_port(text):
    # An EMPTY argument is the one worth naming, because it is what a shell
    # hands you when a variable expands to nothing — and cmd.exe does that
    # without a murmur. `int('')` answers with a traceback, which tells the
    # reader about Python instead of about their command line.
    if text == "":
        bad("--port was given nothing (an empty argument — a variable that expanded to nothing?)")
    try:
        port = int(text)
    except ValueError:
        bad(f"'{text}' is not a port number")
    if not 1 <= port <= 65535:
        bad(f"port {port} is not in 1..65535")
    return port


def main():
    port = 8080
    where = None

    args = sys.argv[1:]
    while args:
        token = args.pop(0)
        if token in ("-h", "--help"):
            print(__doc__)
            return
        elif token == "--port":
            if not args:
                bad("--port needs a number after it")
            port = parse_port(args.pop(0))
        elif token == "--dump":
            if not args:
                bad("--dump needs a directory after it")
            where = args.pop(0)
        elif token.startswith("-"):
            bad(f"unknown option {token}")
        else:
            # A bare number still means the port, because that is how this
            # started and a harness script may still spell it that way.
            port = parse_port(token)

    if where is not None:
        dump_bodies(where)
        return

    print(f"httptestd serving on 0.0.0.0:{port} — every interface, so a machine "
          f"across the room can reach it.", flush=True)
    print("Ctrl-C to stop.", flush=True)
    try:
        with Server(("0.0.0.0", port), Handler) as server:
            server.serve_forever()
    except KeyboardInterrupt:
        print("\nhttptestd: stopped.")
    except OSError as error:
        bad(f"cannot listen on port {port}: {error}")


if __name__ == "__main__":
    main()
