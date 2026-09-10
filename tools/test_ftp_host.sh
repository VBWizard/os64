#!/bin/bash
# Drive /bin/ftp's wire parser on the host, at every chunk size.
#
# THIS SUITE HAS A REFERENCE IMPLEMENTATION, which the gopher suite next door
# did not: Python's ftplib ships the same three parsers this file tests, as
# module functions — `parse227` for the passive-mode tuple and `parse257` for
# the quoted path — and `FTP.getmultiline` for the multiline reply. So the
# expectations for those are DIFFED against a reference rather than stated as
# the code's own opinion written twice.
#
# The two deliberate departures from ftplib are asserted by hand, and each is
# a place where matching it would be worse:
#   * a run of six numbers in parentheses wins over an earlier bare run
#     (ftplib takes the first match anywhere);
#   * a 227 naming port 0 is refused rather than returned, because there is
#     nothing to dial.
#
# Everything that reads bytes is driven at chunk sizes 1, 2, 3, 7, 17, 64 and
# whole, because a stream parser's bugs live where a token straddles two
# reads. `0` means "hand over whatever is left".

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/apps/ftp -I userland/libos64/include -I abi/include \
   userland/apps/ftp/wire.c userland/libos64/str.c \
   tools/test_ftp_host.c \
   -o "$work/test_ftp"

python3 - "$work" <<'PY'
import ftplib
import pathlib
import subprocess
import sys

work = pathlib.Path(sys.argv[1])
exe = str(work / "test_ftp")
failures = 0
checks = 0

CHUNKS = [1, 2, 3, 7, 17, 64, 0]


def fail(what, detail):
    global failures
    failures += 1
    print(f"FAIL {what}: {detail}")


def check(what, got, want):
    global checks
    checks += 1
    if got != want:
        fail(what, f"got {got!r}, want {want!r}")


def run(mode, *args, data=b""):
    done = subprocess.run([exe, mode] + [str(a) for a in args],
                          input=data, capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
    rows = []
    for line in done.stdout.decode("latin-1").split("\n"):
        if not line:
            continue
        fields = {}
        # `text=` and `wire=` are last on their line and may hold anything the
        # escaper leaves, spaces included — so the split is on the FIRST '=' of
        # each token, and the last field takes the rest of the line.
        rest = line
        while rest:
            key, _, rest = rest.partition("=")
            if key in ("text", "wire", "path"):
                fields[key] = rest
                rest = ""
            else:
                value, _, rest = rest.partition(" ")
                fields[key] = value
        rows.append(fields)
    return rows


def unescape(s):
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            if s[i + 1] == "\\":
                out.append(0x5C)
                i += 2
                continue
            if s[i + 1] == "x":
                out.append(int(s[i + 2:i + 4], 16))
                i += 4
                continue
        out.append(ord(s[i]))
        i += 1
    return out.decode("latin-1")


# ── Replies ─────────────────────────────────────────────────────────────
#
# Every case is fed at every chunk size and must give the same answer, and
# every case reads the stream to its END — a desync shows up as the second
# reply, never the first.

def replies(name, wire, expect_end="END"):
    for chunk in CHUNKS:
        rows = run("reply", chunk, data=wire)
        got = [(int(r["code"]), unescape(r["text"])) for r in rows if "code" in r]
        end = [r for r in rows if "end" in r]
        yield chunk, got, end
        if end and end[0]["end"] != expect_end:
            fail(f"{name} chunk={chunk}", f"ended {end[0]['end']}, want {expect_end}")


def expect_replies(name, wire, want, expect_end="END"):
    for chunk, got, _end in replies(name, wire, expect_end):
        check(f"{name} chunk={chunk}", got, want)


expect_replies(
    "single line",
    b"220 Service ready\r\n",
    [(220, "220 Service ready")],
)

expect_replies(
    "two replies in a row",
    b"220 Ready\r\n331 Password required\r\n",
    [(220, "220 Ready"), (331, "331 Password required")],
)

# THE TRAP. The banner's second line would pass for a complete reply on its
# own; only the repeated 220 with a space closes the real one. A parser that
# takes the bait reports the FIRST reply short and then answers the USER
# command with the leftovers.
expect_replies(
    "multiline hiding a reply line",
    b"220-Welcome\r\n220 This looks like the end\r\n"
    b"220-and this looks like a start\r\n220 Ready.\r\n"
    b"331 Password required\r\n",
    [(220, "220-Welcome\n220 This looks like the end"),
     (220, "220-and this looks like a start\n220 Ready."),
     (331, "331 Password required")],
)

# A different code inside a multiline reply is TEXT. RFC 959 §4.2 allows it and
# ftplib agrees; a client that closed on it would desync by one reply.
expect_replies(
    "multiline carrying a foreign code",
    b"230-Login ok\r\n500 Not a close\r\n230 Done\r\n",
    [(230, "230-Login ok\n500 Not a close\n230 Done")],
)

expect_replies(
    "hyphen line repeating the code is not a close",
    b"220-One\r\n220-Two\r\n220 Three\r\n",
    [(220, "220-One\n220-Two\n220 Three")],
)

expect_replies(
    "bare LF, no CR",
    b"220 Ready\n331 Password\n",
    [(220, "220 Ready"), (331, "331 Password")],
)

expect_replies(
    "a lone CR inside the text stays a byte",
    b"220 A\rB\r\n",
    [(220, "220 A\rB")],
)

expect_replies(
    "final line with no terminator is still a reply",
    b"221 Goodbye",
    [(221, "221 Goodbye")],
)

expect_replies(
    "empty text after the code",
    b"200 \r\n",
    [(200, "200 ")],
)

expect_replies(
    "a bare code with no separator closes",
    b"200\r\n",
    [(200, "200")],
)

expect_replies(
    "not a reply at all",
    b"hello there\r\n",
    [],
    expect_end="MALFORMED",
)

expect_replies(
    "a code below 100 is not a reply",
    b"099 Nope\r\n",
    [],
    expect_end="MALFORMED",
)

expect_replies(
    "a multiline that never closes ends the stream",
    b"220-Welcome\r\n220-Still talking\r\n",
    [],
    expect_end="END",
)

# The consume ceiling: a multiline reply that goes on past
# FTP_REPLY_CONSUME_MAX is a peer that has stopped speaking the protocol.
filler = b"".join(b"220-line %05d padded out to make this go faster\r\n" % i
                  for i in range(2000))
expect_replies(
    "a multiline past the consume ceiling",
    b"220-Welcome\r\n" + filler + b"220 Done\r\n",
    [],
    expect_end="TOO_LONG",
)

# An over-long single line is truncated and the tail consumed, so the NEXT
# reply still parses — losing the tail of a banner beats losing the session.
long_line = b"220 " + b"x" * 4000 + b"\r\n331 Password\r\n"
for chunk in CHUNKS:
    rows = run("reply", chunk, data=long_line)
    codes = [int(r["code"]) for r in rows if "code" in r]
    check(f"over-long line keeps the channel chunk={chunk}", codes, [220, 331])
    first = [r for r in rows if "code" in r][0]
    check(f"over-long line is flagged chunk={chunk}", first["truncated"], "1")

# ── A deadline is not a broken connection ───────────────────────────────
#
# `replystall` makes the feeder answer FTP_SOURCE_STALLED where it would
# otherwise say end-of-input, which is what a short-deadline read of a quiet
# server does. Two rules, and the client's drain after an interrupted transfer
# depends on both:
#
#   * a stall BETWEEN replies leaves the channel usable, so asking again gives
#     STALLED again and never FAILED. This is the regression guard for a real
#     bug: the stall and the broken connection shared one sticky flag, so the
#     first quiet moment poisoned the control channel for the rest of the
#     session and the next command reported a failure that had not happened.
#   * a stall PART WAY through a reply is FAILED, because the opening line is
#     already consumed and the next read would begin inside it.

for chunk in CHUNKS:
    rows = run("replystall", chunk, data=b"220 Ready\r\n331 Password\r\n")
    codes = [int(r["code"]) for r in rows if "code" in r]
    end = [r for r in rows if "end" in r][0]
    check(f"stall after whole replies chunk={chunk}", codes, [220, 331])
    check(f"stall between replies chunk={chunk}", end["end"], "STALLED")
    check(f"a stalled channel is still usable chunk={chunk}",
          end["again"], "STALLED")

for chunk in CHUNKS:
    rows = run("replystall", chunk, data=b"220-Welcome\r\n220-still going\r\n")
    end = [r for r in rows if "end" in r][0]
    check(f"stall mid-reply is a failure chunk={chunk}", end["end"], "FAILED")
    check(f"a failed channel stays failed chunk={chunk}", end["again"], "FAILED")

# ── The multiline reply, against ftplib ─────────────────────────────────
#
# ftplib's own reader, fed the same bytes through a fake socket file. It keeps
# every line exactly as the server wrote it and joins them with '\n', which is
# the shape this parser reports — so the comparison is byte for byte with no
# adjustment on either side, and the multiline rule is diffed rather than
# asserted.

class FakeFTP(ftplib.FTP):
    def __init__(self, wire):
        self.file = __import__("io").StringIO(wire.decode("latin-1"))
        self.maxline = ftplib.FTP.maxline


REFERENCE_CASES = [
    b"220 Service ready\r\n",
    b"220-Welcome\r\n220 This looks like the end\r\n",
    b"230-Login ok\r\n500 Not a close\r\n230 Done\r\n",
    b"220-One\r\n220-Two\r\n220 Three\r\n",
    b"220-Welcome\r\n220-  234 indented digits are text\r\n220 Ready\r\n",
]

for wire in REFERENCE_CASES:
    reference = FakeFTP(wire).getmultiline()
    for chunk in CHUNKS:
        rows = run("reply", chunk, data=wire)
        got = [(int(r["code"]), unescape(r["text"])) for r in rows if "code" in r]
        check(f"vs ftplib {wire[:24]!r} chunk={chunk}",
              got[0], (int(reference[:3]), reference))

# ── 227, against ftplib.parse227 ────────────────────────────────────────

PASV_CASES = [
    "227 Entering Passive Mode (10,0,2,2,195,80).",
    "227 Entering Passive Mode (127,0,0,1,4,1)",
    "227 =10,0,2,2,195,80",
    "227 Passive mode ok. 192,168,1,50,200,1",
    "227 (0,0,0,0,255,255)",
]

for line in PASV_CASES:
    ref_host, ref_port = ftplib.parse227(line)
    rows = run("pasv", data=line.encode())
    check(f"227 ip {line!r}", rows[0]["ip"], ref_host)
    check(f"227 port {line!r}", int(rows[0]["port"]), ref_port)
    check(f"227 result {line!r}", rows[0]["result"], "OK")

# Refusals, and the two departures from ftplib.
REFUSALS = [
    ("227 Entering Passive Mode", "NO_TUPLE"),
    ("227 Entering Passive Mode (10,0,2,2,195)", "NO_TUPLE"),
    ("227 Entering Passive Mode (10,0,2,2,195,80,7)", "NO_TUPLE"),
    ("227 Entering Passive Mode (10,0,2,300,195,80)", "RANGE"),
    ("227 Entering Passive Mode (10,0,2,2,0,0)", "PORT_ZERO"),
]
for line, want in REFUSALS:
    rows = run("pasv", data=line.encode())
    check(f"227 refusal {line!r}", rows[0]["result"], want)

# ftplib's regex takes the FIRST run it finds anywhere; the parenthesised run
# is the one the server meant, so this parser prefers it. Asserted by hand
# because the reference is the thing being departed from.
line = "227 Protocol 1,2,3,4,5,6 superseded; use (10,0,2,2,195,80)"
rows = run("pasv", data=line.encode())
check("227 parentheses win", rows[0]["ip"], "10.0.2.2")
check("227 parentheses win port", int(rows[0]["port"]), 195 * 256 + 80)
check("227 ftplib would disagree here",
      ftplib.parse227(line)[0], "1.2.3.4")

# With no parentheses anywhere, the FIRST run stands — which is ftplib's rule.
line = "227 10,0,2,2,195,80 and later 1,2,3,4,5,6"
rows = run("pasv", data=line.encode())
check("227 first run without parens", rows[0]["ip"], "10.0.2.2")
check("227 first run matches ftplib", rows[0]["ip"], ftplib.parse227(line)[0])

# ── 257, against ftplib.parse257 ────────────────────────────────────────

PATH_CASES = [
    '257 "/" is the current directory',
    '257 "/pub/archives" is the current directory',
    '257 "/home/chris"',
    '257 "/a""b" created',          # a doubled quote is one literal quote
    '257 ""',                        # the empty path
    '257 "/trailing"" quote"',
]

for line in PATH_CASES:
    reference = ftplib.parse257(line)
    rows = run("path257", data=line.encode())
    check(f"257 result {line!r}", rows[0]["result"], "OK")
    check(f"257 path {line!r}", unescape(rows[0]["path"]), reference)

for line in ['257 no quotes here', '257 "unterminated']:
    rows = run("path257", data=line.encode())
    check(f"257 refusal {line!r}", rows[0]["result"], "NONE")

# ── Commands ────────────────────────────────────────────────────────────

check("command with an argument",
      unescape(run("command", "RETR", data=b"/pub/file.txt")[0]["wire"]),
      "RETR /pub/file.txt\\x0d\\x0a".replace("\\x0d", "\r").replace("\\x0a", "\n"))

check("command with no argument",
      unescape(run("command", "PASV", data=b"")[0]["wire"]),
      "PASV\r\n")

# THE INJECTION DOOR. A name carrying CRLF is a second command nobody typed.
for evil in [b"a\r\nDELE /etc/passwd", b"a\nQUIT", b"a\rb"]:
    rows = run("command", "RETR", data=evil)
    check(f"command refuses {evil!r}", rows[0]["result"], "NEWLINE")

rows = run("command", "RETR", data=b"x" * 4000)
check("command refuses an over-long name", rows[0]["result"], "TOO_LONG")

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
PY
