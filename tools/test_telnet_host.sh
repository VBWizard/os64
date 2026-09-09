#!/bin/bash
# Drive /bin/telnet's protocol engine on the host, at every chunk size.
#
# THERE IS A REFERENCE IMPLEMENTATION HERE, which the gopher suite next door
# did not have: Python's telnetlib. It is deprecated and on its way out of the
# standard library, so the differential section SKIPS ITSELF when the import
# fails rather than failing the run — everything else in this file is a rule
# stated by hand from RFC 854, RFC 857/858, RFC 1073 and RFC 1143, and stands
# on its own.
#
# Where telnetlib is present it judges the one thing a hand-written
# expectation would only be the code's opinion twice: WHICH BYTES ARE DATA.
# Splitting a telnet stream into "screen" and "protocol" is the whole job of
# the parser, and two independent implementations disagreeing about it is
# exactly the bug that puts a negotiation on the glass. Its two known
# departures from os64 are excluded from the corpus and named where they are:
# telnetlib swallows DC1 (0x11) unconditionally, and it lets the option byte
# of a subnegotiation be escaped.
#
# Everything that reads bytes is driven at chunk sizes 1, 2, 3, 7, 17, 64 and
# whole, because a stream parser's bugs live where a token straddles two
# reads — and telnet's tokens are two, three and many bytes long, which is
# the entire reason its parse state lives in a struct. `0` means "hand over
# whatever is left".

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/apps/telnet -I userland/libos64/include -I abi/include \
   userland/apps/telnet/telnet_protocol.c userland/libos64/str.c \
   tools/test_telnet_host.c \
   -o "$work/test_telnet"

python3 - "$work" <<'PY'
import os
import pathlib
import random
import socket
import subprocess
import sys
import time
import warnings

work = pathlib.Path(sys.argv[1])
exe = str(work / "test_telnet")
failures = 0

# ── The bytes, spelled once ─────────────────────────────────────────────
IAC, DONT, DO, WONT, WILL, SB = 255, 254, 253, 252, 251, 250
GA, EL, EC, AYT, AO, IP, BRK, DM, NOP, SE = 249, 248, 247, 246, 245, 244, 243, 242, 241, 240
O_BINARY, O_ECHO, O_SGA, O_TTYPE, O_NAWS = 0, 1, 3, 24, 31

NOTE_ECHO, NOTE_SIZE, NOTE_AYT, NOTE_CONTRADICT = 1, 2, 4, 8

CHUNKS = (1, 2, 3, 7, 17, 64, 0)


def fail(what, detail):
    global failures
    failures += 1
    print(f"FAIL {what}: {detail}")


def run(*steps):
    done = subprocess.run([exe] + [str(s) for s in steps], capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
    out = {}
    for line in done.stdout.decode("latin-1").split("\n"):
        if "=" in line:
            key, _, value = line.partition("=")
            out[key] = value
    return out


def check(what, got, want):
    if got != want:
        fail(what, f"got {got!r} want {want!r}")


def hx(data):
    return bytes(data).hex()


# The C side prints control bytes as escapes so a fixture cannot repaint the
# screen of whoever reads the output. This is the inverse, so expectations can
# be written as plain bytes.
def unescape(text):
    out = bytearray()
    i = 0
    while i < len(text):
        if text[i] != "\\":
            out.append(ord(text[i]))
            i += 1
        elif text[i + 1] == "\\":
            out.append(0x5C)
            i += 2
        elif text[i + 1] == "x":
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        else:
            raise SystemExit(f"bad escape in {text!r}")
    return bytes(out)


def decode(stream, chunk=0, cap=None, extra=()):
    """What the engine shows the screen, and what it says back."""
    steps = ["--chunk", chunk]
    if cap is not None:
        steps += ["--cap", cap]
    steps += list(extra)
    steps.append("in:" + hx(stream))
    got = run(*steps)
    return unescape(got["data"]), bytes.fromhex(got["wire"]), got


# ── Data, and the bytes that are not data ───────────────────────────────
#
# The parser's first job: everything that is not an IAC sequence reaches the
# screen, and nothing that is ever does.

STREAMS = {
    "plain text": (b"hello, world", b"hello, world"),
    "a doubled escape is one byte": (bytes([ord("a"), IAC, IAC, ord("b")]),
                                     bytes([ord("a"), 255, ord("b")])),
    "a two-byte command vanishes": (bytes([ord("a"), IAC, NOP, ord("b")]), b"ab"),
    "GO-AHEAD vanishes": (bytes([ord("a"), IAC, GA, ord("b")]), b"ab"),
    "an option exchange vanishes": (bytes([ord("a"), IAC, WILL, O_SGA, ord("b")]), b"ab"),
    "a subnegotiation vanishes": (bytes([ord("a"), IAC, SB, O_TTYPE]) + b"IS xterm"
                                  + bytes([IAC, SE, ord("b")]), b"ab"),
    "an unknown command vanishes": (bytes([ord("a"), IAC, 99, ord("b")]), b"ab"),
    "a stray SE vanishes": (bytes([ord("a"), IAC, SE, ord("b")]), b"ab"),
    # RFC 854 gives NUL no meaning at the printer. CR NUL is the protocol's
    # bare carriage return and a lone NUL is terminfo's pad byte; dropping it
    # is what makes both arrive as what they mean.
    "CR NUL is a bare carriage return": (b"a\r\x00b", b"a\rb"),
    "CR LF is a newline": (b"a\r\nb", b"a\r\nb"),
    "a lone NUL is a pad byte": (b"a\x00b", b"ab"),
    "a lone CR survives": (b"a\rb", b"a\rb"),
    "high bytes are data": (bytes([0xC3, 0xA9, 0x80]), bytes([0xC3, 0xA9, 0x80])),
    "escapes are the remote program talking": (b"\x1b[31mred\x1b[0m", b"\x1b[31mred\x1b[0m"),
}

for name, (stream, want) in STREAMS.items():
    for chunk in CHUNKS:
        data, _, _ = decode(stream, chunk)
        check(f"{name} @{chunk}", data, want)
print(f"data: {len(STREAMS)} streams, each at chunk sizes {CHUNKS}")

# EVERY SPLIT POINT OF EVERY SEQUENCE, one byte at a time, is what chunk size
# 1 already covers — but a sequence split unevenly across two reads of
# different sizes is a different arithmetic. This walks the split through a
# stream that holds one of everything.
EVERYTHING = (b"a" + bytes([IAC, IAC]) + b"b"
              + bytes([IAC, WILL, O_SGA])
              + bytes([IAC, SB, O_TTYPE]) + b"x" + bytes([IAC, IAC, IAC, SE])
              + b"c\r\n" + bytes([IAC, DO, O_NAWS]) + b"\r\x00d")
whole, _, _ = decode(EVERYTHING, 0)
for split in range(1, len(EVERYTHING)):
    head = run("--chunk", 0, "in:" + hx(EVERYTHING[:split]))
    both = run("--chunk", 0, "in:" + hx(EVERYTHING[:split]),
               "in:" + hx(EVERYTHING[split:]))
    check(f"split at {split}", unescape(both["data"]), whole)
print(f"data: every one of {len(EVERYTHING) - 1} split points of a stream with one of everything")

# ── What the engine says back ───────────────────────────────────────────

# The client speaks first: don't make us wait for a turn, we won't make you
# wait for one, and here is a window size worth having. ECHO is deliberately
# not among them.
got = run("offer")
check("the opening offers", got["wire"],
      hx([IAC, DO, O_SGA, IAC, WILL, O_SGA, IAC, WILL, O_NAWS]))
check("offer fits", got["offer_ok"], "1")

# An option this client does not do is refused BY NAME, in the right
# direction: a WILL is answered DONT and a DO is answered WONT.
for verb, answer in ((WILL, DONT), (DO, WONT)):
    for option in (O_TTYPE, O_BINARY, 39, 200):
        _, wire, got = decode(bytes([IAC, verb, option]))
        check(f"refuse {verb}/{option}", wire, bytes([IAC, answer, option]))
        check(f"refused stays off {verb}/{option}", got["us.binary"], "0")

# The three that are in.
_, wire, got = decode(bytes([IAC, WILL, O_ECHO]))
check("the server may echo", wire, bytes([IAC, DO, O_ECHO]))
check("and then we do not", got["echo"], "0")
check("and the caller is told", int(got["notes"]) & NOTE_ECHO, NOTE_ECHO)

_, wire, got = decode(bytes([IAC, WILL, O_SGA]))
check("the server may suppress go-ahead", wire, bytes([IAC, DO, O_SGA]))
_, wire, got = decode(bytes([IAC, DO, O_SGA]))
check("and so may we", wire, bytes([IAC, WILL, O_SGA]))
_, wire, got = decode(bytes([IAC, DO, O_NAWS]))
check("we report a window size", wire, bytes([IAC, WILL, O_NAWS]))
check("and the caller is told to send one", int(got["notes"]) & NOTE_SIZE, NOTE_SIZE)

# NAWS IS OURS AND NOT HIS. A server has no window; a client that agreed
# would be promising to read a size it has nowhere to put.
_, wire, _ = decode(bytes([IAC, WILL, O_NAWS]))
check("a server does not do NAWS", wire, bytes([IAC, DONT, O_NAWS]))
# ECHO IS HIS AND NOT OURS. A client that echoes to the server is how you see
# every character twice.
_, wire, _ = decode(bytes([IAC, DO, O_ECHO]))
check("we do not echo at the server", wire, bytes([IAC, WONT, O_ECHO]))

# The server turning echo back off (a login shell after the password).
_, wire, got = decode(bytes([IAC, WILL, O_ECHO, IAC, WONT, O_ECHO]))
check("echo off again", wire, bytes([IAC, DO, O_ECHO, IAC, DONT, O_ECHO]))
check("and we echo again", got["echo"], "1")
print("negotiation: the three options in scope, and the refusals for everything else")

# ── RFC 1143: no repeated acknowledgements, no loops ────────────────────

# THE REPEAT. A peer that says the same thing five times is answered once:
# after the first, nothing has changed, and an answer to a request that
# changed nothing is the first half of a negotiation loop.
_, wire, _ = decode(bytes([IAC, WILL, O_ECHO]) * 5)
check("five WILLs, one DO", wire, bytes([IAC, DO, O_ECHO]))
_, wire, _ = decode(bytes([IAC, DO, O_SGA]) * 5)
check("five DOs, one WILL", wire, bytes([IAC, WILL, O_SGA]))
_, wire, _ = decode(bytes([IAC, WONT, O_ECHO]) * 5)
check("a WONT for an option already off is not answered at all", wire, b"")
_, wire, _ = decode(bytes([IAC, DONT, O_TTYPE]) * 5)
check("a DONT for an option already off is not answered at all", wire, b"")

# AND A NOTICE IS A CHANGE, NOT A RESTATEMENT. The caller ACTS on these — it
# repaints the echo policy, it puts a window size on the wire — so a peer that
# says again what is already true must provoke neither. `notes_last` is what
# the final step alone raised.
got = run("in:" + hx([IAC, WILL, O_ECHO]), "in:" + hx([IAC, WILL, O_ECHO]))
check("echo is announced once", int(got["notes"]) & NOTE_ECHO, NOTE_ECHO)
check("and not announced again", int(got["notes_last"]) & NOTE_ECHO, 0)
got = run("in:" + hx([IAC, DO, O_NAWS]), "in:" + hx([IAC, DO, O_NAWS]))
check("a size is asked for once", int(got["notes"]) & NOTE_SIZE, NOTE_SIZE)
check("and not asked for again", int(got["notes_last"]) & NOTE_SIZE, 0)

# THE CROSSING, which is the case RFC 1143 was written for. We ask, and his
# own request for the same option is already in flight. His WILL completes
# our DO instead of starting a fresh exchange.
got = run("offer", "in:" + hx([IAC, WILL, O_SGA, IAC, DO, O_SGA]))
check("a crossed SGA is settled in silence", got["wire"],
      hx([IAC, DO, O_SGA, IAC, WILL, O_SGA, IAC, WILL, O_NAWS]))
check("crossed: he suppresses", got["him.sga"], "1")
check("crossed: so do we", got["us.sga"], "1")

# And a refusal of what we asked for is taken quietly, not argued with.
got = run("offer", "in:" + hx([IAC, DONT, O_NAWS, IAC, WONT, O_SGA]))
check("a refusal is not argued with", got["wire"],
      hx([IAC, DO, O_SGA, IAC, WILL, O_SGA, IAC, WILL, O_NAWS]))
check("refused NAWS is off", got["us.naws"], "0")
check("refused SGA is off", got["him.sga"], "0")
print("negotiation: repeats answered once, crossings settled in silence")


# TWO OF THESE IN A ROOM MUST FALL SILENT. Both ends speak first, both ends
# want the same options, and each replays its whole history every round
# because a fresh process is a fresh engine. If the Q method were wrong this
# never terminates, which is the failure RFC 1143 exists to name.
def converse(rounds=12):
    scripts = [["offer"], ["offer"]]
    said = [b"", b""]
    for turn in range(rounds):
        wire = [bytes.fromhex(run(*s)["wire"]) for s in scripts]
        fresh = [wire[0][len(said[0]):], wire[1][len(said[1]):]]
        said = wire
        if not fresh[0] and not fresh[1]:
            return turn
        for side in (0, 1):
            if fresh[1 - side]:
                scripts[side].append("in:" + hx(fresh[1 - side]))
    return None


turns = converse()
if turns is None:
    fail("two clients in a room", "still talking after 12 rounds — a negotiation loop")
else:
    print(f"negotiation: two of these fall silent after {turns} rounds")


# AND A PEER FROM BEFORE 1990: one that acknowledges everything, including
# requests that changed nothing. That is precisely the implementation RFC 1143
# was written about, and our side must not be drawn into the trade.
def naive_peer(seen):
    answer = bytearray()
    i = 0
    while i < len(seen):
        if seen[i] == IAC and i + 2 < len(seen) and seen[i + 1] in (WILL, WONT, DO, DONT):
            verb, option = seen[i + 1], seen[i + 2]
            answer += bytes([IAC, {WILL: DO, WONT: DONT, DO: WILL, DONT: WONT}[verb], option])
            i += 3
        else:
            i += 1
    return bytes(answer)


script = ["offer"]
said = b""
for turn in range(12):
    wire = bytes.fromhex(run(*script)["wire"])
    fresh = wire[len(said):]
    said = wire
    if not fresh:
        break
    script.append("in:" + hx(naive_peer(fresh)))
else:
    fail("a peer that acknowledges everything", "still talking after 12 rounds")
print(f"negotiation: an acknowledge-everything peer is answered {turn} times and then not at all")

# ── Subnegotiations: read exactly, kept nowhere ─────────────────────────

SUBS = {
    "an ordinary one": bytes([IAC, SB, O_TTYPE]) + b"\x00xterm-256color" + bytes([IAC, SE]),
    "an empty one": bytes([IAC, SB, O_TTYPE, IAC, SE]),
    "one holding a doubled escape": bytes([IAC, SB, O_TTYPE, 1, IAC, IAC, 2, IAC, SE]),
    "one holding an SE that is data": bytes([IAC, SB, O_TTYPE, SE, SE, IAC, SE]),
    "one holding a whole fake sequence": bytes([IAC, SB, O_TTYPE]) + b"\x1b[2J" + bytes([IAC, SE]),
    # The option byte is NOT escaped, so option 255 (RFC 861's extended
    # options list) is an option and not an IAC. A parser that reads it as an
    # escape loses its place for the rest of the connection.
    "one for option 255": bytes([IAC, SB, 255]) + b"payload" + bytes([IAC, SE]),
    "a long one": bytes([IAC, SB, O_TTYPE]) + bytes(range(256)).replace(bytes([IAC]), b"") * 40
                  + bytes([IAC, SE]),
}
for name, stream in SUBS.items():
    for chunk in CHUNKS:
        data, wire, got = decode(b"a" + stream + b"b", chunk)
        check(f"subnegotiation {name} @{chunk}", data, b"ab")
        check(f"subnegotiation {name} answers nothing @{chunk}", wire, b"")
        check(f"subnegotiation {name} ends @{chunk}", got["mid"], "0")
print(f"subnegotiations: {len(SUBS)} read exactly and thrown away, at every chunk size")

# A SUBNEGOTIATION INTERRUPTED BY A COMMAND is abandoned, and the command is
# read as what it is. Only IAC SE ends one properly; waiting for an SE that is
# not coming would swallow the rest of the connection.
data, wire, got = decode(bytes([IAC, SB, O_TTYPE]) + b"junk"
                         + bytes([IAC, WILL, O_ECHO]) + b"tail")
check("an interrupted subnegotiation is abandoned", data, b"tail")
check("and its interruption is answered", wire, bytes([IAC, DO, O_ECHO]))
check("and the parser is back in data", got["mid"], "0")

data, wire, _ = decode(bytes([IAC, SB, O_TTYPE]) + b"junk" + bytes([IAC, AYT]) + b"tail")
check("an interrupting AYT abandons it too", data, b"tail")

# ── EOF in the middle of a word ─────────────────────────────────────────
#
# A connection is cut wherever the peer stopped talking, and "stopped
# mid-word" is a different report from "hung up".
TRUNCATED = {
    "after IAC": bytes([ord("a"), IAC]),
    "after IAC WILL": bytes([ord("a"), IAC, WILL]),
    "after IAC SB": bytes([ord("a"), IAC, SB]),
    "inside a subnegotiation": bytes([ord("a"), IAC, SB, O_TTYPE]) + b"half",
    "after an IAC inside one": bytes([ord("a"), IAC, SB, O_TTYPE, IAC]),
}
for name, stream in TRUNCATED.items():
    for chunk in CHUNKS:
        data, _, got = decode(stream, chunk)
        check(f"truncated {name} @{chunk}", got["mid"], "1")
        check(f"truncated {name} shows what it had @{chunk}", data, b"a")
for name, stream in (("plain data", b"abc"),
                     ("a whole command", bytes([IAC, NOP])),
                     ("a whole option", bytes([IAC, WILL, O_SGA])),
                     ("a whole subnegotiation", bytes([IAC, SB, O_TTYPE, IAC, SE]))):
    _, _, got = decode(stream)
    check(f"complete: {name}", got["mid"], "0")
print(f"end of input: {len(TRUNCATED)} truncations reported, whole sequences not")

# ── What a person types ─────────────────────────────────────────────────

# RETURN LEAVES AS CR NUL, which is 4.2BSD's default and RFC 854's spelling
# for "the key was pressed" as opposed to "start a new line". The difference
# is invisible to a Unix login and decisive against a program that reads a
# line and then reads a key — see telnet_set_eol_crlf. `crlf on` asks for the
# other spelling and is covered right below.
TYPED = {
    "ordinary text": (b"hello", b"hello"),
    "a 255 in a paste is doubled": (bytes([ord("a"), 255, ord("b")]),
                                    bytes([ord("a"), IAC, IAC, ord("b")])),
    "Enter is CR NUL": (b"\n", b"\r\x00"),
    "a carriage return is one too": (b"\r", b"\r\x00"),
    "and CR LF is ONE press": (b"\r\n", b"\r\x00"),
    "LF CR is two": (b"\n\r", b"\r\x00\r\x00"),
    "a pasted line": (b"ls -l\n", b"ls -l\r\x00"),
    "two pasted lines": (b"one\r\ntwo\r\n", b"one\r\x00two\r\x00"),
    "a control byte a program meant": (b"\x03\x04\x1b", b"\x03\x04\x1b"),
}
for name, (typed, want) in TYPED.items():
    for chunk in CHUNKS:
        got = run("--chunk", chunk, "text:" + hx(typed))
        check(f"typed {name} @{chunk}", bytes.fromhex(got["wire"]), want)
        check(f"typed {name} taken whole @{chunk}", got["accepted"], got["offered"])
print(f"typed: {len(TYPED)} shapes, each fed at chunk sizes {CHUNKS}")

# THE HALVES OF A NEWLINE CAN ARRIVE IN DIFFERENT CALLS, which is what
# happens when someone pastes across the boundary of a read. Chunk size 1
# above already proves it; this says so by name.
got = run("text:0d", "text:0a")
check("CR then LF in separate calls is one press", got["wire"], hx(b"\r\x00"))
got = run("text:0d", "text:41")
check("CR then a letter is a press and a letter", got["wire"], hx(b"\r\x00A"))
got = run("text:0d", "cmd:244", "text:0a")
check("a command between them ends the pair", got["wire"],
      hx(bytes([13, 0, IAC, IP, 13, 0])))

# THE OTHER SPELLING, on request. `crlf on` is 4.2BSD's `toggle crlf`, for a
# peer that wants a new line rather than a carriage return.
for typed, want in ((b"\n", b"\r\n"), (b"\r", b"\r\n"), (b"\r\n", b"\r\n"),
                    (b"ls -l\n", b"ls -l\r\n")):
    got = run("crlf:on", "text:" + hx(typed))
    check(f"crlf on: {typed!r} is CR LF", bytes.fromhex(got["wire"]), want)
got = run("crlf:on", "crlf:off", "text:0a")
check("crlf off goes back to CR NUL", got["wire"], hx(b"\r\x00"))
print("newline: CR NUL by default, CR LF on request, at every split point")

# Bare commands.
for command, name in ((IP, "interrupt"), (AYT, "are you there"), (BRK, "break")):
    got = run(f"cmd:{command}")
    check(f"send {name}", got["wire"], hx([IAC, command]))
    check(f"send {name} fits", got["cmd_ok"], "1")

# END OF INPUT IS AN ORDINARY BYTE, because RFC 854 has no command for it and
# LINEMODE's is refused with the rest of LINEMODE. This is what `send eof`
# puts on the wire.
got = run("text:04")
check("end of input is 0x04, sent as data", got["wire"], "04")

# ── AYT is the one command that asks for an answer ──────────────────────
_, _, got = decode(bytes([IAC, AYT]))
check("AYT is reported", int(got["notes"]) & NOTE_AYT, NOTE_AYT)
for quiet in (NOP, GA, DM, BRK, IP, AO, EC, EL):
    _, _, got = decode(bytes([IAC, quiet]))
    check(f"command {quiet} is not reported", int(got["notes"]) & NOTE_AYT, 0)
print("commands: AYT is reported, the rest are consumed in silence")

# ── The window size ─────────────────────────────────────────────────────

# Nothing goes out before the peer has agreed, and the size is remembered so
# that an agreement arriving later has something to announce.
got = run("size:80x24")
check("no size before agreement", got["wire"], "")
check("and it says so", got["size_ok"], "0")

got = run("size:80x24", "in:" + hx([IAC, DO, O_NAWS]), "size:80x24")
check("agreement is announced", int(got["notes"]) & NOTE_SIZE, NOTE_SIZE)
check("then the size goes out", got["wire"],
      hx([IAC, WILL, O_NAWS, IAC, SB, O_NAWS, 0, 80, 0, 24, IAC, SE]))

SIZES = {
    (80, 24): [0, 80, 0, 24],
    (132, 43): [0, 132, 0, 43],
    (1, 1): [0, 1, 0, 1],
    # THE BUG THIS OPTION IS FAMOUS FOR. A dimension of 255 is an IAC in the
    # middle of the subnegotiation; undoubled it ends the size early and every
    # byte after it is read as a command.
    (255, 24): [0, IAC, IAC, 0, 24],
    (80, 255): [0, 80, 0, IAC, IAC],
    (255, 255): [0, IAC, IAC, 0, IAC, IAC],
    # And the high byte can be 255 just as easily: 65280 is 0xFF00.
    (65280, 24): [IAC, IAC, 0, 0, 24],
    # 511 is 0x01FF — the 255 is the LOW byte, and it doubles like any other.
    (511, 24): [1, IAC, IAC, 0, 24],
}
for (cols, rows), payload in SIZES.items():
    got = run("in:" + hx([IAC, DO, O_NAWS]), f"size:{cols}x{rows}")
    check(f"size {cols}x{rows}", got["wire"],
          hx([IAC, WILL, O_NAWS, IAC, SB, O_NAWS] + payload + [IAC, SE]))
print(f"window size: {len(SIZES)} dimensions, escapes doubled inside the subnegotiation")

# ── Local echo ──────────────────────────────────────────────────────────
check("echo starts on", run()["echo"], "1")
check("the server taking echo turns it off",
      run("in:" + hx([IAC, WILL, O_ECHO]))["echo"], "0")
check("`echo on` overrules the server",
      run("in:" + hx([IAC, WILL, O_ECHO]), "echo:on")["echo"], "1")
check("`echo off` overrules its absence", run("echo:off")["echo"], "0")
check("and `echo auto` hands it back",
      run("in:" + hx([IAC, WILL, O_ECHO]), "echo:on", "echo:auto")["echo"], "0")

# ── Back-pressure, in both directions ───────────────────────────────────
#
# NOTHING IS DROPPED AND NOTHING GROWS WITHOUT A BOUND: when the outbound
# queue fills, the reader stops reading and hands back the bytes it did not
# take. That is what keeps a client responsive to its own escape key while a
# write is stalled.

limits = run()
out_max, reply_max = int(limits["out_max"]), int(limits["reply_max"])

# A peer sending nothing but options this client refuses: each one costs three
# bytes of queue and nothing drains.
flood = bytes([IAC, WILL, O_TTYPE]) * (out_max // 3 + 40)
got = run("--nodrain", "in:" + hx(flood))
pending = len(bytes.fromhex(got["pending"]))
if pending > out_max:
    fail("queue bound", f"queued {pending} bytes into a {out_max}-byte queue")
if int(got["unfed"]) == 0:
    fail("back-pressure", "the whole flood was consumed with nothing draining")
if pending > out_max - reply_max + 3:
    fail("queue headroom", f"{pending} of {out_max} queued, leaving no room to answer")
check("the flood is not lost, only unread",
      int(got["consumed"]) + int(got["unfed"]), len(flood))

# ...and draining lets exactly the same stream through.
got = run("in:" + hx(flood))
check("with a drain, the same flood is answered whole", int(got["unfed"]), 0)
check("and every refusal reached the wire", len(bytes.fromhex(got["wire"])), len(flood))

# The screen side: a one-byte buffer decodes the same stream as a whole one,
# because the caller is told how much was consumed and comes back for the rest.
for cap in (1, 2, 3, 5, 4096):
    data, _, got = decode(EVERYTHING, 0, cap=cap)
    check(f"a {cap}-byte screen buffer", data, whole)
    check(f"a {cap}-byte screen buffer consumes it all", int(got["unfed"]), 0)

# Typed text is never cut in half. A queue with one byte left cannot take a
# 255 (which needs two) or a newline (which needs two), so it takes neither.
paste = bytes([255]) * (out_max + 100)
got = run("--nodrain", "text:" + hx(paste))
queued = bytes.fromhex(got["pending"])
check("a paste of escapes is doubled", len(queued) % 2, 0)
check("and only whole ones are queued", int(got["accepted"]) * 2, len(queued))
if int(got["accepted"]) >= len(paste):
    fail("paste back-pressure", "a paste larger than the queue was taken whole")

newlines = b"\n" * (out_max + 100)
got = run("--nodrain", "text:" + hx(newlines))
queued = bytes.fromhex(got["pending"])
check("a paste of newlines is not cut between CR and its partner", len(queued) % 2, 0)
check("and each one is whole", queued, b"\r\x00" * (len(queued) // 2))

# A partial write leaves the rest in order.
got = run("offer", "--nodrain", "text:" + hx(b"hi"), "sent:2", "sent:1", "sent:99")
check("a partial write drains in order", bytes.fromhex(got["wire"]),
      bytes([IAC, DO, O_SGA, IAC, WILL, O_SGA, IAC, WILL, O_NAWS]) + b"hi")
check("and leaves nothing behind", got["pending"], "")
print("back-pressure: the queue bounds the reader, and nothing is cut in half")

# ── Names ───────────────────────────────────────────────────────────────
names = run()
check("a known option has a name", names["name.option"], "window-size")
check("a known command has a name", names["name.command"], "IP")
check("an unknown option is named by number", names["name.unknown_option"], "option 200")
check("an unknown command is named by number", names["name.unknown_command"], "command 200")
check("two unknowns in one line do not share a buffer",
      names["name.pair"], "option 201 option 202")

# ── The differential: which bytes are data ──────────────────────────────
#
# telnetlib is the second opinion. Where it and this engine disagree about
# what reaches the screen, one of them is putting protocol on the glass.
with warnings.catch_warnings():
    warnings.simplefilter("ignore")
    try:
        import telnetlib
    except ImportError:
        telnetlib = None

if telnetlib is None:
    print("differential: telnetlib is gone from this Python — skipped")
else:
    def reference(stream):
        peer = telnetlib.Telnet()
        peer.set_option_negotiation_callback(lambda sock, cmd, opt: None)
        peer.rawq = stream
        peer.irawq = 0
        peer.process_rawq()
        return peer.cookedq

    # 0x11 is excluded: telnetlib swallows DC1 unconditionally, which is a
    # terminal's business and not a telnet parser's. Option 255 in a
    # subnegotiation is excluded: telnetlib treats that byte as escapable, and
    # RFC 854 does not.
    DATA_BYTES = [b for b in range(256) if b not in (IAC, 0x11)]
    COMMANDS = [NOP, GA, DM, BRK, IP, AO, EC, EL, AYT, SE]
    VERBS = [WILL, WONT, DO, DONT]

    def stream_for(rng, length):
        out = bytearray()
        while len(out) < length:
            roll = rng.random()
            if roll < 0.50:
                out += bytes(rng.choices(DATA_BYTES, k=rng.randrange(1, 8)))
            elif roll < 0.60:
                out += bytes([IAC, IAC])
            elif roll < 0.72:
                out += bytes([IAC, rng.choice(COMMANDS)])
            elif roll < 0.88:
                out += bytes([IAC, rng.choice(VERBS), rng.randrange(0, 256)])
            else:
                payload = bytearray()
                for _ in range(rng.randrange(0, 12)):
                    if rng.random() < 0.2:
                        payload += bytes([IAC, IAC])
                    else:
                        payload.append(rng.choice(DATA_BYTES))
                out += bytes([IAC, SB, rng.randrange(0, 255)]) + payload + bytes([IAC, SE])
        return bytes(out)

    rng = random.Random(854)
    cases = 0
    disagreements = 0
    for case in range(400):
        stream = stream_for(rng, rng.randrange(1, 200))
        if case % 5 == 0:                      # cut a fifth of them mid-word
            stream = stream[:rng.randrange(1, len(stream) + 1)]
        want = reference(stream)
        for chunk in (1, 3, 17, 0):
            data, _, _ = decode(stream, chunk)
            cases += 1
            if data != want:
                disagreements += 1
                if disagreements <= 3:
                    fail(f"differential case {case} @{chunk}",
                         f"stream {stream.hex()}\n      os64 {data!r}\n      telnetlib {want!r}")
    if disagreements > 3:
        fail("differential", f"{disagreements} disagreements in {cases} comparisons")
    print(f"differential: {cases} comparisons against telnetlib, every stream at four chunk sizes")

# ── The engine on a real socket, against a peer that misbehaves ─────────
#
# Everything above hands the engine bytes from memory. This hands it bytes
# from a TCP connection, in whatever pieces the network chose, and lets
# tools/telnettestd.py judge the client's half of the conversation by its own
# rules — which is the half no self-test can judge honestly. It is loopback
# only and needs nothing but python3, so it is not an "integration test" that
# gets skipped; set OS64_NO_LIVE=1 if you want the memory half alone.
#
# The engine is a byte machine, so this needs no os64 and no /bin/telnet: each
# round replays the whole conversation through the same driver and sends
# whatever it newly wants to say. What the server sees is exactly what the
# client will send it.
if os.environ.get("OS64_NO_LIVE"):
    print("live: skipped (OS64_NO_LIVE)")
else:
    def free_port():
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        return port

    def connect_when_ready(port, tries=60):
        # NO READINESS PROBE: a --once server serves whatever connects first,
        # so a probe connection IS the client and the real one arrives to a
        # closed socket. Retrying the real connect is both simpler and the
        # only thing that cannot consume the scenario.
        for _ in range(tries):
            try:
                return socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(0.05)
        raise SystemExit(f"telnettestd never came up on {port}")

    def converse_over_tcp(port, rounds=24, patience=0.4):
        sock = connect_when_ready(port)
        sock.settimeout(patience)
        script, said = ["offer", "size:80x25"], b""
        try:
            for _ in range(rounds):
                state = run(*script)
                wire = bytes.fromhex(state["wire"])
                fresh, said = wire[len(said):], wire
                if fresh:
                    sock.sendall(fresh)
                # A window size cannot go out before the peer has agreed to
                # take one, so the engine latches a notice and the caller
                # comes back. This is that caller — the client's own loop
                # will have this shape.
                if int(state["notes"]) & NOTE_SIZE and "size:80x25" not in script[2:]:
                    script.append("size:80x25")
                    continue
                # READ, THEN ANSWER PROMPTLY. A greedy drain with a timeout
                # longer than the peer's own cadence never returns while the
                # peer keeps talking, so the client hears the whole
                # conversation and replies after it is over — which is the
                # deafness ruling 4 is about, in miniature. Block for the
                # first arrival, take whatever is already queued behind it,
                # and go answer.
                heard = b""
                try:
                    heard = sock.recv(4096)
                    if heard:
                        sock.settimeout(0.02)
                        while True:
                            more = sock.recv(4096)
                            if not more:
                                break
                            heard += more
                except socket.timeout:
                    pass
                finally:
                    sock.settimeout(patience)
                if heard:
                    script.append("in:" + hx(heard))
            return run(*script)
        finally:
            sock.close()

    LIVE = {
        # what the ENGINE must be able to say about itself afterwards; the
        # server judges what it heard, and the two halves are different
        # questions on purpose.
        "unix":       {"him.echo": "1", "echo": "0", "us.naws": "1", "mid": "0"},
        "bbs":        {"him.echo": "1", "echo": "0", "us.naws": "0", "mid": "0"},
        "halfduplex": {"him.sga": "0", "echo": "1", "mid": "0"},
        "chatty":     {"him.echo": "1", "him.sga": "1", "mid": "0"},
        "hostile":    {"him.echo": "1", "him.sga": "1", "mid": "1"},
    }

    for scenario, wanted in LIVE.items():
        port = free_port()
        server = subprocess.Popen(
            [sys.executable, "tools/telnettestd.py", "--scenario", scenario,
             "--port", str(port), "--once", "--quiet"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            got = converse_over_tcp(port)
        except Exception as exc:                       # noqa: BLE001 - reported, not raised
            server.kill()
            fail(f"live {scenario}", f"{type(exc).__name__}: {exc}")
            continue

        try:
            verdict = server.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.kill()
            verdict = None
        if verdict != 0:
            fail(f"live {scenario}",
                 f"telnettestd judged the client's half unacceptable"
                 f" (exit {verdict}); rerun it without --quiet to see which check")
        for key, want in wanted.items():
            check(f"live {scenario} {key}", got[key], want)

    print(f"live: {len(LIVE)} scenarios over loopback, each judged from both ends")

if failures:
    print(f"test_telnet_host: {failures} FAILED")
    sys.exit(1)
print("test_telnet_host: all checks passed")
PY
