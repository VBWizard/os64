#!/usr/bin/env python3
r"""telnetwalk — drive a telnet service by an expect table, and probe what a
prompt actually accepts.

    python3 telnetwalk.py <host> <port> [--on 'PATTERN=RESPONSE']... \
                          [--turns N] [--eol nul|crlf|cr] [--probe]

WHY IT EXISTS. Reproducing an interop bug means getting back to the screen it
happens on, and on a BBS that is a dozen prompts deep behind a login. Driving
those through QEMU with `sendkey` costs a minute a screen and goes wrong when
a prompt is slow; from here it is a few seconds and it is the same every time.
This is NOT a test of os64's client — it negotiates the way ours does so the
far end behaves the same, and then the client under suspicion is taken back to
the screen this found.

    # walk a board to its poker door and ask that door what Return it wants
    python3 telnetwalk.py x-bit.org 23230 --turns 14 --probe \
        --on 'ansi color graphics=\r\0'  --on 'first name=Os64\r\0' \
        --on 'last name=Kernel\r\0'      --on 'password=SECRET\r\0' \
        --on '< enter >=\r\0'            --on 'scan for your messages=N' \
        --on 'press any key=\x20'        --on 'main menu -=D'

A PATTERN is a lowercase regular expression matched against the last three
lines received; the FIRST rule that matches wins, so order them from most
specific to least. A RESPONSE is sent verbatim after `\r \n \0 \e \xNN`
decoding — the escapes matter, because the whole point may be which of them
a prompt accepts.

`--probe` is the instrument the door hunt needed: at whatever prompt the walk
stops on, send each spelling of Return in turn and report how many bytes came
back. Silence is an answer here — a prompt that ignores `CR NUL` and moves on
a bare `CR` is telling you the far end is not doing NVT translation, which is
a fact about IT and not about you (DEBTS § Networking).

Escape sequences are stripped from what is printed, so a screen full of ANSI
art collapses to its words. That makes prompts readable and makes art
unreadable; use telnettap.py when the bytes themselves are the question.
"""
import argparse
import re
import socket
import sys
import time

IAC, SB, SE, WILL, WONT, DO, DONT = 255, 250, 240, 251, 252, 253, 254
O_ECHO, O_SGA = 1, 3

# Enough of the escape grammar to take the paint off the words: CSI, the
# two-byte forms, a character-set selection, and an OSC up to its BEL.
ESCAPES = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Za-z0-9]"
                     rb"|\x1b\][^\x07]*\x07|\x1b.")

# What Return can be spelled as. CR NUL is RFC 854's "the Return key was
# pressed" and what BSD telnet sends by default; CR LF is "start a new line";
# a bare CR is legal only inside BINARY and is what a DOS door expects.
EOL = {"nul": b"\r\x00", "crlf": b"\r\n", "cr": b"\r"}


def as_text(raw):
    """The words, with the paint taken off: escapes stripped, blank lines
    dropped. Art becomes unreadable and prompts become readable, which is the
    trade this tool exists to make (telnettap.py is for the other one)."""
    flat = ESCAPES.sub(b"", bytes(raw)).replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    lines = [ln.rstrip() for ln in flat.decode("latin-1").split("\n")]
    return [ln for ln in lines if ln.strip()]


class Session:
    """A telnet client that refuses everything except ECHO and SGA, which is
    the option set os64's own client holds (TELNET.md ruling 1). NAWS is not
    offered: nothing here has a window."""

    def __init__(self, host, port, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.5)
        self.sock.sendall(bytes([IAC, DO, O_SGA, IAC, WILL, O_SGA]))

    def _negotiate(self, data):
        """Answer the options, and return what was left over as text."""
        reply, text, i = bytearray(), bytearray(), 0
        while i < len(data):
            c = data[i]
            if c == IAC and i + 1 < len(data):
                n = data[i + 1]
                if n in (WILL, WONT, DO, DONT) and i + 2 < len(data):
                    opt = data[i + 2]
                    if n == WILL:
                        reply += bytes([IAC, DO if opt in (O_ECHO, O_SGA)
                                        else DONT, opt])
                    elif n == DO:
                        reply += bytes([IAC, WILL if opt == O_SGA else WONT, opt])
                    i += 3
                    continue
                if n == SB:
                    end = data.find(bytes([IAC, SE]), i)
                    i = len(data) if end < 0 else end + 2
                    continue
                i += 2
                continue
            text.append(c)
            i += 1
        if reply:
            self.sock.sendall(bytes(reply))
        return text

    def read_until_idle(self, quiet=2.5, cap=30, recognized=None):
        """Everything until the far end has been silent for `quiet` seconds —
        a prompt is the SILENCE after a screen, which is the only end-marker a
        board offers, since nothing in the protocol says "your turn".

        `recognized(text)` is the escape hatch for a prompt that never falls
        silent. A countdown ("You have 20 seconds") repaints once a second, so
        waiting for quiet means waiting for it to EXPIRE; a caller that can
        already tell what it is looking at says so and gets it now."""
        end, last, got = time.time() + cap, time.time(), bytearray()
        while time.time() < end and time.time() - last < quiet:
            try:
                chunk = self.sock.recv(8192)
            except socket.timeout:
                if recognized and recognized(as_text(got)):
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            got += self._negotiate(chunk)
            last = time.time()
            if recognized and recognized(as_text(got)):
                break
        return bytes(got)

    def text_until_idle(self, quiet=2.5, cap=30, recognized=None):
        return as_text(self.read_until_idle(quiet, cap, recognized))

    def send(self, data):
        try:
            self.sock.sendall(data)
            return True
        except OSError:
            return False   # the far end hung up; the caller decides

    def close(self):
        self.sock.close()


def unescape(text):
    r"""`\r \n \0 \e \xNN \\` in a command-line response, decoded."""
    out, i = bytearray(), 0
    simple = {"r": 13, "n": 10, "t": 9, "0": 0, "e": 27, "\\": 92}
    while i < len(text):
        if text[i] == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt == "x" and i + 3 < len(text):
                out.append(int(text[i + 2:i + 4], 16))
                i += 4
                continue
            if nxt in simple:
                out.append(simple[nxt])
                i += 2
                continue
        out.append(ord(text[i]) & 0xFF)
        i += 1
    return bytes(out)


def probe(session):
    """Ask the prompt we stopped on which Return it takes. Silence IS the
    answer for the ones it does not."""
    print("--- what does this prompt accept? ---")
    for name, data in list(EOL.items()) + [("space", b" ")]:
        if not session.send(data):
            print(f"  {name:5} -> the far end hung up; nothing more to ask")
            return
        back = session.text_until_idle(quiet=2.0, cap=8)
        size = sum(len(ln) for ln in back)
        verdict = f"{size} chars back" if size else "NOTHING"
        print(f"  {name:5} {data!r:12} -> {verdict}")
        if back:
            print("        " + " / ".join(back[-2:])[:120])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--on", action="append", default=[], metavar="PATTERN=RESPONSE",
                    help="answer a prompt matching PATTERN with RESPONSE")
    ap.add_argument("--turns", type=int, default=12)
    ap.add_argument("--eol", choices=sorted(EOL), default="nul",
                    help="how a bare `\\r` in a response is spelled on the wire")
    ap.add_argument("--probe", action="store_true",
                    help="at the prompt the walk stops on, try every Return")
    ap.add_argument("--quiet-secs", type=float, default=2.5,
                    help="how long a silence has to be to count as a prompt")
    ap.add_argument("--window", type=int, default=6, metavar="N",
                    help="how many trailing lines a PATTERN is matched against "
                         "(default 6 — a prompt drawn inside a box is several "
                         "lines above the bottom of the screen)")
    args = ap.parse_args()

    rules = []
    for spec in args.on:
        if "=" not in spec:
            print(f"telnetwalk: --on wants PATTERN=RESPONSE, got {spec!r}",
                  file=sys.stderr)
            return 2
        pattern, response = spec.split("=", 1)
        # However a response spells Return, it leaves as the ONE spelling
        # --eol names — that is what makes re-running a walk under a
        # different Return a single flag rather than an edit of every rule.
        body = re.sub(rb"\r\x00|\r\n|\r", EOL[args.eol], unescape(response))
        rules.append((re.compile(pattern.lower()), body))

    def window(lines):
        return "\n".join(lines[-args.window:])

    def matched(lines):
        low = window(lines).lower()
        return any(pat.search(low) for pat, _ in rules)

    session = Session(args.host, args.port)
    for turn in range(args.turns):
        lines = session.text_until_idle(quiet=args.quiet_secs, recognized=matched)
        tail = window(lines)
        print(f"===== turn {turn} =====")
        print(tail[-400:] if tail else "(silence)")
        answer = next((body for pat, body in rules if pat.search(tail.lower())), None)
        if answer is None:
            # Running out of rules is the normal end, not a failure: it means
            # "here is the screen you asked to be taken to".
            print("  -- no rule for this prompt")
            break
        session.send(answer)

    if args.probe:
        probe(session)
    session.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
