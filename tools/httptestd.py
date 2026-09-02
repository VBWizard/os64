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

    /hello.txt      a small text file, Content-Length      the happy path
    /big.bin        1 MiB, Content-Length                  the 64KB write loop
    /slow.txt       Content-Length, dribbled out           the progress meter
    /nolength.txt   HTTP/1.0, no length, close-delimited   RFC 1945 framing
    /dir/           a page at a path ending in '/'         the index.html rule
    /query?x=1&y=2  a page behind a query string           the query survives
    /missing        404                                    a refusal
    /redirect       301 -> /hello.txt                      reported, not followed
    /redirect-https 302 -> an https:// address              the honest TLS answer
    /chunked        Transfer-Encoding: chunked             refused, not written
    /gzipped        Content-Encoding: gzip                 refused, not written
    /cut            half the promised body, then hangs up  a short body is a failure
    /junk           not HTTP at all                        a bad header line

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

HELLO = b"hello from the host, over HTTP.\n"
DIRPAGE = b"<html><body><h1>a directory</h1></body></html>\n"
QUERYPAGE = b"the query survived the trip\n"
NOLENGTH = b"no Content-Length here; the close is the length.\n" * 40
SLOW = b"dribbled out, a piece at a time, so the meter has something to do.\n" * 700


def big_bytes(size=1024 * 1024, seed=0x05064A17):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


BIG = big_bytes()


def head(status, reason, headers, version="HTTP/1.1"):
    text = f"{version} {status} {reason}\r\n"
    for name, value in headers:
        text += f"{name}: {value}\r\n"
    return (text + "\r\n").encode("latin-1")


class Handler(socketserver.StreamRequestHandler):
    # A fetch-and-close client gets a fetch-and-close server: every reply
    # ends by hanging up, which is HTTP/1.0's framing and the only one
    # os64get speaks at this rung of the ladder.
    def handle(self):
        request = self.rfile.readline(8192).decode("latin-1", "replace").strip()
        while True:                       # drain the rest of the head
            line = self.rfile.readline(8192)
            if line in (b"\r\n", b"\n", b""):
                break

        parts = request.split()
        path = parts[1] if len(parts) >= 2 else "/"
        print(f"  {request}", flush=True)

        route = path.split("?", 1)[0]
        query = path.split("?", 1)[1] if "?" in path else ""

        if route == "/hello.txt":
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
        elif route == "/redirect-https":
            self.send(head(302, "Found",
                           [("Location", "https://example.com/"), ("Content-Length", 0)]))
        elif route == "/chunked":
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Transfer-Encoding", "chunked")]))
            self.send(b"20\r\nthirty-two bytes of chunked body\r\n0\r\n\r\n")
        elif route == "/gzipped":
            body = zlib.compress(HELLO)
            self.send(head(200, "OK", [("Content-Type", "text/plain"),
                                       ("Content-Encoding", "gzip"),
                                       ("Content-Length", len(body))]) + body)
        elif route == "/cut":
            self.send(head(200, "OK", [("Content-Type", "application/octet-stream"),
                                       ("Content-Length", 100000)]))
            self.send(BIG[:40000])
        elif route == "/junk":
            self.send(b"220 this is not an HTTP server\r\n\r\nwhatever\n")
        else:
            body = f"no such thing here: {route}\n".encode("latin-1")
            self.send(head(404, "Not Found", [("Content-Type", "text/plain"),
                                              ("Content-Length", len(body))]) + body)

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
                       ("nolength.txt", NOLENGTH), ("index.html", DIRPAGE)):
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
