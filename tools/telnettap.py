#!/usr/bin/env python3
r"""telnettap — sit between a telnet client and a real host, and name every
byte the client sends.

    python3 telnettap.py <listen-port> <host> <host-port> [--seconds N] [--both]
    python3 telnettap.py 2340 x-bit.org 23230

WHY IT EXISTS. "Is it us or is it them?" is the question every interop bug
opens with, and a screen cannot answer it — a screen shows you what arrived,
never what left. This shows what left, spelled as telnet spells it: IAC DO 3,
CR NUL, ESC, 'y'. Point the guest at 10.0.2.2:<listen-port> instead of the
real host and the whole session goes through here on its way.

It settled one argument already. A 1993 DOS door on a board ignored `CR NUL`
and `CR LF` and moved on a bare `0D`, while the board's OWN prompts took
`CR NUL` everywhere — two facts that together say "the door path, not the
client", and neither of which is visible from the glass (DEBTS § Networking).

The host's direction is SIZED rather than named by default, because a board
paints thousands of bytes of art per screen and the client's half is the half
under suspicion. `--both` names it too when the question runs the other way.

RUNNING IT is the same geography as httptestd.py and gophertestd.py: from
WSL2 the QEMU guest reaches this at 10.0.2.2, and a machine on the LAN reaches
it at this host's address. One client at a time — it accepts once, relays, and
exits when either end closes or the clock runs out.
"""
import argparse
import socket
import sys
import threading
import time

IAC, SB, SE = 255, 250, 240
VERB = {251: "WILL", 252: "WONT", 253: "DO", 254: "DONT"}
CMD = {249: "GA", 248: "EL", 247: "EC", 246: "AYT", 245: "AO", 244: "IP",
       243: "BRK", 242: "DM", 241: "NOP", 240: "SE", 250: "SB", 255: "IAC"}
OPT = {0: "binary", 1: "echo", 3: "sga", 5: "status", 24: "ttype",
       31: "naws", 32: "tspeed", 33: "lflow", 34: "linemode", 39: "environ"}
# The bytes worth a name of their own: the ones an argument is usually about.
NAMED = {0: "NUL", 7: "BEL", 8: "BS", 9: "TAB", 10: "LF", 13: "CR",
         27: "ESC", 127: "DEL"}


def render(data):
    """One line of telnet, as a person would read it aloud."""
    out, i = [], 0
    while i < len(data):
        c = data[i]
        if c == IAC and i + 1 < len(data):
            n = data[i + 1]
            if n in VERB and i + 2 < len(data):
                opt = data[i + 2]
                out.append(f"IAC {VERB[n]} {OPT.get(opt, opt)}")
                i += 3
                continue
            if n == SB:
                # A subnegotiation is its own sentence; show it whole so the
                # doubled IACs inside it stay visible.
                end = data.find(bytes([IAC, SE]), i)
                body = data[i + 2:end if end >= 0 else len(data)]
                opt = body[0] if body else -1
                rest = " ".join(f"{b:02X}" for b in body[1:])
                out.append(f"IAC SB {OPT.get(opt, opt)} {rest} IAC SE".strip())
                i = len(data) if end < 0 else end + 2
                continue
            out.append(f"IAC {CMD.get(n, n)}")
            i += 2
            continue
        if c in NAMED:
            out.append(NAMED[c])
        elif 32 <= c < 127:
            out.append(f"'{chr(c)}'")
        else:
            out.append(f"{c:02X}")
        i += 1
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("listen_port", type=int)
    ap.add_argument("host")
    ap.add_argument("host_port", type=int)
    ap.add_argument("--seconds", type=int, default=1800,
                    help="give up after this long (default 1800)")
    ap.add_argument("--both", action="store_true",
                    help="name the host's bytes too, not only the client's")
    args = ap.parse_args()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.listen_port))
    srv.listen(1)
    print(f"telnettap: listening on {args.listen_port} "
          f"-> {args.host}:{args.host_port}", flush=True)

    client, addr = srv.accept()
    upstream = socket.create_connection((args.host, args.host_port))
    print(f"telnettap: {addr[0]}:{addr[1]} <-> {args.host}:{args.host_port}",
          flush=True)

    start = time.time()
    done = []

    def pump(src, dst, label, name_bytes):
        try:
            while not done:
                data = src.recv(4096)
                if not data:
                    break
                shown = render(data) if name_bytes else f"{len(data)} bytes"
                print(f"[{time.time() - start:8.2f}] {label} {shown}", flush=True)
                dst.sendall(data)
        except OSError:
            pass
        done.append(1)

    threading.Thread(target=pump, daemon=True,
                     args=(client, upstream, "CLIENT->host", True)).start()
    threading.Thread(target=pump, daemon=True,
                     args=(upstream, client, "host->CLIENT", args.both)).start()

    deadline = start + args.seconds
    while not done and time.time() < deadline:
        time.sleep(0.2)
    client.close()
    upstream.close()
    print("telnettap: done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
