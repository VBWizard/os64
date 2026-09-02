#!/usr/bin/env python3
r"""tlsproxy — the machine that has a TLS, fetching for the machine that does not.

os64 has no TLS and is not going to grow one: BROWSER.md's first ruling is
that it gets BORROWED when its day comes, because thirty years of side-channel
and oracle attacks teach no kernel lessons. That same ruling names the stopgap
— "a TLS-terminating proxy on the valet" — and this is it. os64get asks in
plain HTTP, this fetches over TLS with the trust store the host machine
already maintains, and the answer comes back in plain HTTP.

    export http_proxy=http://<this-machine>:8888/     (in the guest)
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
    10.0.2.2, so `export http_proxy=http://10.0.2.2:8888/`.
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

Deliberately NOT implemented: CONNECT. A real proxy tunnels TLS through it
untouched, which is the right thing when the client can do TLS — and os64
cannot, so a tunnel would hand it bytes it has no way to read. Terminating is
not a shortcut here, it is the entire point.
"""

import http.client
import socketserver
import ssl
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
# Content-Length: re-derived below, because the body's length after
#   un-chunking is ours to state, not the origin's to remember.
HOP_BY_HOP = {"transfer-encoding", "connection", "keep-alive", "proxy-authenticate",
              "proxy-authorization", "te", "trailer", "upgrade", "content-length"}

VERBOSE = False


def bad(message):
    print(f"tlsproxy: {message}", file=sys.stderr)
    print(f"usage: {USAGE}", file=sys.stderr)
    raise SystemExit(2)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        request = self.rfile.readline(65536).decode("latin-1", "replace").strip()
        while True:                          # drain the rest of the head
            line = self.rfile.readline(65536)
            if line in (b"\r\n", b"\n", b""):
                break

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
        except OSError as error:
            self.refuse(502, "Bad Gateway", f"could not reach {split.hostname}: {error}")

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
            body = reply.read()
        finally:
            upstream.close()

        # REDIRECTS ARE PASSED THROUGH, NOT FOLLOWED. os64get is the client;
        # deciding what a 301 means is its business, and a proxy that followed
        # them would hide from it the one thing it asked to see.
        head = f"HTTP/1.0 {reply.status} {reply.reason}\r\n"
        for name, value in reply.getheaders():
            if name.lower() in HOP_BY_HOP:
                continue
            head += f"{name}: {value}\r\n"
        head += f"Content-Length: {len(body)}\r\n"
        head += "Via: 1.0 os64-tlsproxy\r\n"
        head += "\r\n"

        if VERBOSE:
            print(f"    -> {reply.status} {reply.reason}, {len(body)} bytes", flush=True)
        self.send(head.encode("latin-1") + body)

    def refuse(self, status, reason, detail):
        print(f"    !! {status} {reason}: {detail}", flush=True)
        body = f"tlsproxy: {detail}\n".encode("latin-1")
        self.send(f"HTTP/1.0 {status} {reason}\r\n"
                  f"Content-Type: text/plain\r\n"
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
    print("  In the guest:  export http_proxy=http://<this-machine>:%d/" % port)
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
