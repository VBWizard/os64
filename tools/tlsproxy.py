#!/usr/bin/env python3
r"""tlsproxy — the machine that has a TLS, fetching for the machine that does not.

os64 has no TLS and is not going to grow one: BROWSER.md's first ruling is
that it gets BORROWED when its day comes, because thirty years of side-channel
and oracle attacks teach no kernel lessons. That same ruling names the stopgap
— "a TLS-terminating proxy on the valet" — and this is it. os64get asks in
plain HTTP, this fetches over TLS with the trust store the host machine
already maintains, and the answer comes back in plain HTTP.

    export https_proxy=http://<this-machine>:8888/    (in the guest)
    os64get https://example.com/

WHAT IT COSTS, said here as plainly as os64get says it on every fetch: THIS
PROGRAM SEES EVERYTHING IN THE CLEAR. The encrypted leg runs from here to the
origin. The leg from os64 to here is plain text on the wire, and the whole
conversation passes through this process unencrypted. For reading public pages
that is exactly the trade BROWSER.md intends. It is NOT end-to-end encryption
and nothing carrying a password should go through it.

It is also an OPEN proxy while it runs — anyone who can reach the port can
fetch through it, appearing to the far side as this machine. That is fine on a
home LAN for as long as you are using it, and it is why it prints what it is
doing and why you should stop it when you are done.

    python3 tlsproxy.py [--port N] [--verbose]

WHERE TO RUN IT, the same split every tool here has:
  - QEMU, from WSL2: run it in WSL2; the guest reaches the host's loopback at
    10.0.2.2, so `export https_proxy=http://10.0.2.2:8888/`.
  - THE P5, or anything on the LAN: run it on the WINDOWS side, because WSL2
    lives behind a NAT of its own and a listener inside it is not reachable
    from the room (os64serve.py's header tells the same story at length):

        cd /mnt/c/temp
        python3.exe '\\wsl$\<distro>\home\<you>\src\os64\tools\tlsproxy.py'

    Say yes to the firewall prompt. `wsl -l` names the distro.

THE PROTOCOL is the one every proxy has spoken since CERN's in 1994: the
request line carries the WHOLE URL rather than a path ("absolute-form", RFC
7230 §5.3.2), because the connection came here and the path alone would name
a file on this machine instead of a page on the web.

    GET https://example.com/ HTTP/1.0
    Host: example.com

WHAT IT PROMISES ABOUT COMPLETENESS, because it sits inside a tool whose
whole point is that "complete" and "correct" are different claims: a body
that does not arrive whole becomes a 502, never a short file. A length the
origin stated is forwarded so os64get can police it; a length nobody stated
is established here by spooling the body before announcing it, so an
upstream failure still happens while there is somewhere to put the error.
A transfer-coding this proxy did not actually decode is refused rather than
passed on with its framing stripped and its label removed.

Deliberately NOT implemented: CONNECT. A real proxy tunnels TLS through it
untouched, which is the right thing when the client can do TLS — and os64
cannot, so a tunnel would hand it bytes it has no way to read. Terminating is
not a shortcut here, it is the entire point.
"""

import http.client
import socketserver
import ssl
import tempfile
import sys
import urllib.parse

USAGE = "python3 tlsproxy.py [--port N] [--verbose]"

# What NOT to pass back to os64get, and why each one would lie.
#
# Transfer-Encoding: http.client has already un-chunked the body by the time
#   we see it, so forwarding the header would describe a framing that is no
#   longer there. (This is also why a chunked origin works through the proxy
#   while os64get cannot read chunked directly — an HTTP/1.1-to-1.0 downgrade
#   is a proxy's ordinary job, not a trick.)
# Connection / Keep-Alive: hop-by-hop by definition (RFC 7230 §6.1); they
#   describe THIS connection, not the origin's.
# Content-Length: dropped here and decided in relay(), because whether the
#   origin's number still describes what os64get will receive depends on
#   whether the body was un-chunked on the way through. Forwarded when the
#   origin stated one and was not chunked; omitted otherwise, and an omitted
#   length means the close is the length.
HOP_BY_HOP = {"transfer-encoding", "connection", "keep-alive", "proxy-authenticate",
              "proxy-authorization", "te", "trailer", "upgrade", "content-length"}

# The unknown-length spool. Small bodies stay in memory; past SPOOL_RAM the
# temp file rolls over to disk, so the memory bound survives. SPOOL_MAX is
# the point at which a lengthless origin that never stops is refused rather
# than allowed to fill the disk instead of the RAM — the same hazard moved,
# not the same hazard solved.
SPOOL_RAM = 8 * 1024 * 1024
SPOOL_MAX = 512 * 1024 * 1024

# The request head this proxy will read from a client: one line at a time,
# bounded in length, in count, and in total. os64get sends five short lines;
# these are generous next to that and finite next to a client that is not
# playing.
MAX_LINE = 65536
MAX_HEAD = 262144
MAX_HEAD_LINES = 200

VERBOSE = False


def bad(message):
    print(f"tlsproxy: {message}", file=sys.stderr)
    print(f"usage: {USAGE}", file=sys.stderr)
    raise SystemExit(2)


class Handler(socketserver.StreamRequestHandler):
    # A PER-CONNECTION READ TIMEOUT, because this binds to every interface and
    # anything that can reach the port can open a connection and then say
    # nothing. socketserver applies this to the socket in setup(), so a client
    # that never finishes its request head is dropped instead of parking a
    # worker thread and a file descriptor forever — repeat that and the proxy
    # stops being able to serve the guest at all. It bounds the upstream wait
    # too, which was already 30s by its own timeout.
    # (Codex review round 3, 2026-09-02.)
    timeout = 30

    # Set the moment a reply head goes out. Once it has, THERE IS NO LONGER
    # A PLACE TO PUT AN ERROR: a 502 written after the headers would land in
    # the middle of the body as bytes of the file. A failure past this point
    # can only hang up, which os64get already reads as a broken transfer.
    # (The hazard arrived with streaming — buffering the body meant nothing
    # was sent until everything had succeeded.)
    headersSent = False

    def handle(self):
        try:
            request, ok = self.read_head()
        except OSError as error:
            # A timeout or a reset while the request was still arriving. There
            # is nothing to answer yet and nobody waiting to read an answer.
            print(f"    !! request head never finished: {error}", flush=True)
            return
        if not ok:
            return

        parts = request.split()
        if len(parts) < 2:
            self.refuse(400, "Bad Request", "that was not a request line")
            return

        method, target = parts[0], parts[1]
        if method.upper() != "GET":
            self.refuse(405, "Method Not Allowed",
                        f"this proxy fetches, it does not {method}")
            return

        split = urllib.parse.urlsplit(target)
        if split.scheme not in ("http", "https") or not split.hostname:
            self.refuse(400, "Bad Request",
                        "the request line needs a whole URL "
                        "(absolute-form), e.g. GET https://example.com/ HTTP/1.0")
            return

        print(f"  {method} {target}", flush=True)
        try:
            self.relay(split)
        except (ssl.SSLError, ssl.SSLCertVerificationError) as error:
            # The one failure worth naming precisely: this is the proxy doing
            # the job os64 borrowed it for, and saying no.
            self.refuse(502, "Bad Gateway", f"TLS refused the far end: {error}")
        except http.client.HTTPException as error:
            # IncompleteRead and friends are HTTPException, NOT OSError, so
            # without this arm a body that stops mid-chunk escapes the handler
            # entirely: the thread dies, the socket shuts abruptly, and the
            # reason never reaches anyone. It is caught here so the failure is
            # SAID — and, when it happens during the spool, said as a 502.
            self.refuse(502, "Bad Gateway", f"the origin broke off: {error!r}")
        except OSError as error:
            self.refuse(502, "Bad Gateway", f"could not reach {split.hostname}: {error}")


    def read_head(self):
        """The request line and the headers after it, bounded in both
        directions. Returns (request-line, ok); a refusal has already been
        sent when ok is false.

        The drain has a cap for the same reason the timeout exists: a client
        that keeps sending short header lines forever is as effective at
        parking a worker as one that sends nothing at all, and neither needs
        to be malicious to do it."""
        request = self.rfile.readline(MAX_LINE).decode("latin-1", "replace").strip()

        used = 0
        lines = 0
        while True:
            line = self.rfile.readline(MAX_LINE)
            if line in (b"\r\n", b"\n", b""):
                break
            used += len(line)
            lines += 1
            if used > MAX_HEAD or lines > MAX_HEAD_LINES:
                self.refuse(431, "Request Header Fields Too Large",
                            "more request head than this proxy will read")
                return request, False

        return request, True

    def relay(self, split):
        port = split.port or (443 if split.scheme == "https" else 80)
        path = split.path or "/"
        if split.query:
            path += "?" + split.query

        if split.scheme == "https":
            # The host's own trust store, which is the entire reason this
            # program exists — os64 has no certificates and no opinion about
            # who signed what.
            upstream = http.client.HTTPSConnection(split.hostname, port, timeout=30,
                                                   context=ssl.create_default_context())
        else:
            upstream = http.client.HTTPConnection(split.hostname, port, timeout=30)

        try:
            # identity, for the same reason os64get asks for it: nothing
            # downstream can inflate anything yet.
            upstream.request("GET", path, headers={
                "Host": split.netloc,
                "User-Agent": "os64 tlsproxy/1",
                "Accept-Encoding": "identity",
                "Connection": "close",
            })
            reply = upstream.getresponse()

            # ONLY STRIP A FRAMING WE ACTUALLY REMOVED. http.client de-chunks
            # exactly when the header reads "chunked" and nothing else, so a
            # legal chain like `Transfer-Encoding: gzip, chunked` arrives
            # still coded and still framed — and dropping the header that says
            # so would hand os64get chunk lengths to publish as the file.
            # reply.chunked IS the question "did we decode it", so ask that
            # rather than assuming the header's presence proves it.
            # (Codex review round 2, 2026-09-02.)
            coding = reply.getheader("Transfer-Encoding")
            if coding is not None and not reply.chunked:
                self.refuse(502, "Bad Gateway",
                            f"the origin used transfer-coding '{coding}', which this "
                            f"proxy does not decode — passing it on would look like a file")
                return

            declared = reply.getheader("Content-Length")
            if coding is not None:
                declared = None      # de-chunked: the origin's length is not ours

            if declared is not None:
                # THE LENGTH IS KNOWN, so stream it and let os64get police it:
                # a body that stops early contradicts the Content-Length we
                # forwarded, and os64get already fails loudly on that.
                self.deliver(reply, reply, declared)
                return

            # THE LENGTH IS NOT KNOWN, and that is the dangerous shape. With no
            # Content-Length the close IS the end, so an upstream failure part
            # way through — an unterminated chunked body, a timeout, a reset —
            # reaches os64get as a clean EOF and gets PUBLISHED as a complete
            # file. Streaming it would trade a memory hazard for a silent
            # truncation, which is the worse of the two: this program exists
            # inside a tool whose whole point is that "complete" and "correct"
            # are different claims. (Codex review round 2, 2026-09-02, who
            # reproduced it with a chunked origin that omitted its terminator.)
            #
            # So an unknown-length body is SPOOLED first and only announced
            # once it has arrived whole. Memory is still bounded — the spool
            # rolls over to disk past SPOOL_RAM — and a failure now happens
            # before any head has gone out, so it can still be said out loud
            # as a 502 rather than mimed as a short file.
            with tempfile.SpooledTemporaryFile(max_size=SPOOL_RAM, mode="w+b") as spool:
                size = 0
                while True:
                    piece = reply.read(65536)
                    if not piece:
                        break
                    size += len(piece)
                    if size > SPOOL_MAX:
                        self.refuse(502, "Bad Gateway",
                                    f"the origin declared no length and passed "
                                    f"{SPOOL_MAX} bytes — refusing to spool more")
                        return
                    spool.write(piece)
                spool.seek(0)
                self.deliver(reply, spool, str(size), spooled=True)
        finally:
            upstream.close()

    def deliver(self, reply, body, declared, spooled=False):
        """Send the head, then the body. Past the head there is no way to
        report a failure — see headersSent — so everything that can go wrong
        upstream must already have gone right before this is called."""

        head = f"HTTP/1.0 {reply.status} {reply.reason}\r\n"
        # REDIRECTS ARE PASSED THROUGH, NOT FOLLOWED. os64get is the client;
        # deciding what a 301 means is its business, and a proxy that followed
        # them would hide the one thing it asked to see.
        for name, value in reply.getheaders():
            if name.lower() in HOP_BY_HOP:
                continue
            head += f"{name}: {value}\r\n"
        head += f"Content-Length: {declared}\r\n"
        head += "Via: 1.0 os64-tlsproxy\r\n"
        head += "\r\n"
        self.send(head.encode("latin-1"))
        self.headersSent = True

        moved = 0
        while True:
            piece = body.read(65536)
            if not piece:
                break
            self.send(piece)
            moved += len(piece)

        if VERBOSE:
            print(f"    -> {reply.status} {reply.reason}, {moved} bytes"
                  f"{' (spooled)' if spooled else ''}", flush=True)

    def refuse(self, status, reason, detail):
        if self.headersSent:
            # Too late to say anything but goodbye.
            print(f"    !! {detail} (after headers — hanging up)", flush=True)
            return
        print(f"    !! {status} {reason}: {detail}", flush=True)

        # THE BODY IS UTF-8 AND THE HEAD IS LATIN-1, which is not fussiness:
        # header field values are latin-1 by the spec, a message body is
        # whatever Content-Type says, and every explanation this program
        # writes is English prose with an em-dash in it. Encoding the body as
        # latin-1 raised UnicodeEncodeError from inside the error path —
        # so a correctly detected refusal died on the way to being reported,
        # which is worse than reporting it wrongly. (Found while testing the
        # round-2 fixes, 2026-09-02.)
        body = f"tlsproxy: {detail}\n".encode("utf-8")
        self.send(f"HTTP/1.0 {status} {reason}\r\n"
                  f"Content-Type: text/plain; charset=utf-8\r\n"
                  f"Content-Length: {len(body)}\r\n"
                  f"\r\n".encode("latin-1") + body)

    def send(self, data):
        try:
            self.wfile.write(data)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False


def main():
    global VERBOSE
    port = 8888

    args = sys.argv[1:]
    while args:
        token = args.pop(0)
        if token in ("-h", "--help"):
            print(__doc__)
            return
        if token == "--verbose":
            VERBOSE = True
        elif token == "--port" or not token.startswith("-"):
            text = args.pop(0) if token == "--port" and args else token
            if token == "--port" and text == token:
                bad("--port needs a number after it")
            if text == "":
                bad("--port was given nothing (an empty argument?)")
            try:
                port = int(text)
            except ValueError:
                bad(f"'{text}' is not a port number")
            if not 1 <= port <= 65535:
                bad(f"port {port} is not in 1..65535")
        else:
            bad(f"unknown option {token}")

    print(f"tlsproxy listening on 0.0.0.0:{port}")
    print("  In the guest:  export https_proxy=http://<this-machine>:%d/" % port)
    print("  (THE SCHEME PICKS THE VARIABLE: os64get reads $https_proxy for https,")
    print("   $http_proxy for http. This exists for https, so that is the one to set.)")
    print("  IT SEES EVERY PAGE IN THE CLEAR. Public reading only; stop it when done.")
    print("  Ctrl-C to stop.  (Windows may ask about the firewall - say yes.)")
    try:
        with Server(("0.0.0.0", port), Handler) as server:
            server.serve_forever()
    except KeyboardInterrupt:
        print("\ntlsproxy: stopped.")
    except OSError as error:
        bad(f"cannot listen on port {port}: {error}")


if __name__ == "__main__":
    main()
