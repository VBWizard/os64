#!/usr/bin/env python3
"""telnettestd — a telnet server that misbehaves on purpose.

The real boards are the acceptance test and there is no substitute for them,
but they are also a moving target: blackflag negotiates one way today and
another after its next upgrade, and neither of us controls when. This is the
peer you can ask for the SAME conversation twice — the role gophertestd.py
plays for menus and httptestd.py for responses.

    tools/telnettestd.py --scenario bbs            # then point a client at 2323
    tools/telnettestd.py --scenario hostile --once

Every scenario ends by JUDGING WHAT THE CLIENT SAID: the exit code is 0 when
the client negotiated the way that scenario demands and 1 when it did not, so
this is a test and not only a fixture. What it checks is on the wire and
nothing else — it knows nothing about os64 and would judge BSD telnet the
same way.

From inside QEMU the host is 10.0.2.2 through slirp, so a guest can reach a
scenario running out here:  telnet 10.0.2.2 2323
"""
import argparse
import socket
import sys
import time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
GA, EL, EC, AYT, AO, IP, BRK, DM, NOP = 249, 248, 247, 246, 245, 244, 243, 242, 241
O_BINARY, O_ECHO, O_SGA, O_STATUS, O_TTYPE = 0, 1, 3, 5, 24
O_NAWS, O_TSPEED, O_LFLOW, O_LINEMODE, O_XDISPLOC, O_ENVIRON = 31, 32, 33, 34, 35, 39

VERBS = {WILL: "WILL", WONT: "WONT", DO: "DO", DONT: "DONT"}
CMDS = {249: "GA", 248: "EL", 247: "EC", 246: "AYT", 245: "AO", 244: "IP",
        243: "BRK", 242: "DM", 241: "NOP", 240: "SE", 250: "SB", 255: "IAC"}
OPTS = {0: "binary", 1: "echo", 3: "sga", 5: "status", 6: "timing-mark",
        24: "terminal-type", 25: "end-of-record", 31: "window-size",
        32: "terminal-speed", 33: "flow-control", 34: "line-mode",
        35: "x-display", 39: "environment"}


def opt_name(o):
    return OPTS.get(o, f"option {o}")


def spell(data):
    """A stream as the commands in it, runs of data collapsed."""
    out, i, run = [], 0, 0
    while i < len(data):
        if data[i] == IAC and i + 1 < len(data):
            c = data[i + 1]
            if c == IAC:
                run += 1
                i += 2
                continue
            if run:
                out.append(f"<{run} bytes>")
                run = 0
            if c in VERBS and i + 2 < len(data):
                out.append(f"IAC {VERBS[c]} {opt_name(data[i + 2])}")
                i += 3
                continue
            if c == SB:
                end = data.find(bytes([IAC, SE]), i)
                body = data[i + 3:end] if end > 0 else data[i + 3:]
                name = opt_name(data[i + 2]) if i + 2 < len(data) else "?"
                out.append(f"IAC SB {name} {body.hex()} IAC SE")
                i = end + 2 if end > 0 else len(data)
                continue
            out.append(f"IAC {CMDS.get(c, c)}")
            i += 2
            continue
        run += 1
        i += 1
    if run:
        out.append(f"<{run} bytes>")
    return out


class Peer:
    """One connected client, and everything it has said."""

    def __init__(self, sock, verbose=True):
        self.sock = sock
        self.heard = b""
        self.verbose = verbose

    def send(self, data, split=0, gap=0.0):
        """Write, optionally in `split`-byte pieces — because a sequence
        arriving in two TCP segments is a different test from one arriving
        whole, and it is the one that finds the bugs."""
        data = bytes(data)
        if self.verbose:
            for line in spell(data):
                print(f"  -> {line}")
        if split <= 0:
            self.sock.sendall(data)
            return
        for at in range(0, len(data), split):
            self.sock.sendall(data[at:at + split])
            if gap:
                time.sleep(gap)

    def listen(self, seconds=1.5):
        """Collect whatever arrives for a while. A client that says nothing
        is a legal outcome, not a hang."""
        self.sock.settimeout(seconds)
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            self.heard += chunk
            if self.verbose:
                for line in spell(chunk):
                    print(f"  <- {line}")
        return self.heard

    # ── What the client said, as questions a scenario can ask ───────────
    def said(self, verb, option):
        return bytes([IAC, verb, option]) in self.heard

    def subnegotiated(self, option):
        return bytes([IAC, SB, option]) in self.heard

    def data_bytes(self):
        """Everything the client sent that was NOT protocol."""
        out, i = bytearray(), 0
        while i < len(self.heard):
            if self.heard[i] == IAC and i + 1 < len(self.heard):
                c = self.heard[i + 1]
                if c == IAC:
                    out.append(IAC)
                    i += 2
                elif c in VERBS:
                    i += 3
                elif c == SB:
                    end = self.heard.find(bytes([IAC, SE]), i)
                    i = end + 2 if end > 0 else len(self.heard)
                else:
                    i += 2
                continue
            out.append(self.heard[i])
            i += 1
        return bytes(out)


# ── The scenarios ───────────────────────────────────────────────────────
#
# Each returns a list of (what, ok) — the judgements it wants made about the
# client's half of the conversation.


def scenario_unix(peer):
    """A Unix login: the full telnetd opening, then ECHO taken away for a
    password. The client MUST stop echoing, or the password is on the glass."""
    peer.send([IAC, DO, O_TTYPE, IAC, DO, O_TSPEED, IAC, DO, O_XDISPLOC,
               IAC, DO, O_ENVIRON, IAC, WILL, O_SGA, IAC, DO, O_NAWS,
               IAC, WILL, O_STATUS, IAC, DO, O_LFLOW])
    peer.listen()
    peer.send(b"\r\nos64 test host\r\n\r\nlogin: ")
    peer.listen()
    # The password prompt: the server takes ECHO, which is the whole reason
    # a refuse-everything client is dangerous rather than merely limited.
    peer.send([IAC, WILL, O_ECHO])
    peer.send(b"Password: ")
    peer.listen()
    return [
        ("agrees the server may suppress go-ahead", peer.said(DO, O_SGA)),
        ("agrees the server may echo", peer.said(DO, O_ECHO)),
        ("offers a window size", peer.said(WILL, O_NAWS)),
        ("sends the window size once asked", peer.subnegotiated(O_NAWS)),
        ("refuses terminal-type", peer.said(WONT, O_TTYPE)),
        ("refuses terminal-speed", peer.said(WONT, O_TSPEED)),
        ("refuses the environment", peer.said(WONT, O_ENVIRON)),
        ("never claims to echo at the server", not peer.said(WILL, O_ECHO)),
    ]


def scenario_bbs(peer):
    """An ANSI board, negotiating the way blackflag.acid.org actually does:
    it refuses NAWS, asks for a terminal type, and sends its art regardless."""
    peer.send([IAC, DO, O_XDISPLOC, IAC, WILL, O_ECHO, IAC, WILL, O_SGA,
               IAC, DO, O_BINARY, IAC, DO, O_TTYPE, IAC, DONT, O_NAWS])
    peer.listen()
    # A band of CP437 shading, drawn the way art is: cursor-right for the
    # gaps rather than runs of spaces.
    art = bytearray(b"\x1b(U\x1b[2J\x1b[H")
    for row, shade in enumerate((0xB0, 0xB1, 0xB2, 0xDB)):
        art += b"\x1b[%d;1H" % (row + 2)
        art += b"\x1b[%dC" % (10 + row * 2)
        art += bytes([shade]) * 20
    art += b"\x1b[8;1H\x1b[s\x1b[255B\x1b[6n\x1b[u"     # the height probe
    art += b"\r\n Graphics Mode -> "
    peer.send(art)
    peer.listen()
    return [
        ("refuses terminal-type", peer.said(WONT, O_TTYPE)),
        ("refuses binary mode", peer.said(WONT, O_BINARY)),
        ("refuses the x-display", peer.said(WONT, O_XDISPLOC)),
        ("agrees the board may echo", peer.said(DO, O_ECHO)),
        ("takes the NAWS refusal quietly", not peer.subnegotiated(O_NAWS)),
    ]


def scenario_halfduplex(peer):
    """A server from before SUPPRESS-GO-AHEAD: it never suppresses, and it
    hands over the line with IAC GA after every prompt. A client that prints
    unknown commands collects one of these per line."""
    peer.send([IAC, WONT, O_SGA, IAC, DONT, O_SGA])
    peer.listen()
    for n in range(3):
        peer.send(b"line %d> " % n + bytes([IAC, GA]))
        peer.listen(0.4)
    return [
        ("does not argue with the refusal", not peer.said(DO, O_SGA)
                                            or peer.heard.count(bytes([IAC, DO, O_SGA])) <= 1),
        ("sends nothing in reply to a go-ahead", peer.data_bytes() == b""),
    ]


def scenario_chatty(peer):
    """A peer that restates everything it has already said, five times over —
    the pre-RFC-1143 implementation. A client that acknowledges a request
    which changed nothing starts a negotiation loop with it."""
    for _ in range(5):
        peer.send([IAC, WILL, O_ECHO, IAC, WILL, O_SGA, IAC, DO, O_SGA])
        peer.listen(0.3)
    return [
        ("answers WILL echo exactly once",
         peer.heard.count(bytes([IAC, DO, O_ECHO])) == 1),
        ("answers WILL sga at most once",
         peer.heard.count(bytes([IAC, DO, O_SGA])) <= 1),
        ("answers DO sga at most once",
         peer.heard.count(bytes([IAC, WILL, O_SGA])) <= 1),
    ]


def scenario_hostile(peer):
    """Every sequence cut across a TCP segment, a subnegotiation that never
    ends, commands nobody defined, and an IAC left dangling at the close.
    None of it may reach the screen and none of it may wedge the parser."""
    peer.send([IAC, WILL, O_ECHO], split=1, gap=0.05)
    peer.listen(0.5)
    peer.send([IAC, SB, O_TTYPE] + list(b"\x01no terminator here"), split=1, gap=0.01)
    peer.listen(0.5)
    # ...and now a command mid-subnegotiation, which is the only thing that
    # can rescue a parser waiting for an SE that is never coming.
    peer.send([IAC, WILL, O_SGA], split=1, gap=0.05)
    peer.listen(0.5)
    peer.send([IAC, 99, IAC, NOP, IAC, GA, IAC, DM], split=1, gap=0.02)
    peer.send(b"visible\r\n")
    peer.listen(0.5)
    peer.send([IAC])                               # a dangling escape at the close
    peer.listen(0.3)
    return [
        ("answers a byte-at-a-time WILL echo", peer.said(DO, O_ECHO)),
        ("recovers from an unterminated subnegotiation", peer.said(DO, O_SGA)),
        ("sends nothing in reply to undefined commands", peer.data_bytes() == b""),
    ]


SCENARIOS = {
    "unix": scenario_unix,
    "bbs": scenario_bbs,
    "halfduplex": scenario_halfduplex,
    "chatty": scenario_chatty,
    "hostile": scenario_hostile,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--scenario", default="bbs", choices=sorted(SCENARIOS))
    ap.add_argument("--once", action="store_true",
                    help="serve one connection and exit with its verdict")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)
    if not args.quiet:
        print(f"telnettestd: {args.scenario} on {args.host}:{args.port}"
              f" (from a guest: telnet 10.0.2.2 {args.port})")

    failures = 0
    while True:
        sock, who = server.accept()
        if not args.quiet:
            print(f"\n─── {who[0]}:{who[1]} ───")
        peer = Peer(sock, verbose=not args.quiet)
        try:
            checks = SCENARIOS[args.scenario](peer)
        finally:
            sock.close()

        bad = [what for what, ok in checks if not ok]
        if not args.quiet:
            for what, ok in checks:
                print(f"  {'ok  ' if ok else 'FAIL'} {what}")
            print(f"  {len(checks) - len(bad)}/{len(checks)} checks passed")
        failures += len(bad)
        if args.once:
            return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
