#!/bin/bash
# Drive /bin/gopher's wire parser on the host, at every chunk size.
#
# THERE IS NO REFERENCE IMPLEMENTATION TO DIFF AGAINST, and that is worth
# saying out loud because the HTTP suite next door has two (http.client and
# urllib.parse). Python removed gopherlib in 3.0 and nothing replaced it, so
# gopher's rules are stated here by hand — which they can be, because there
# are about a dozen of them and RFC 1436 is nine pages.
#
# The one exception is the address half: urllib.parse still knows the gopher
# scheme, so host/port/path splitting IS cross-checked against it. That is
# the half where a hand-written expectation would just be the code's opinion
# written twice.
#
# Everything that reads bytes is driven at chunk sizes 1, 2, 3, 7, 17, 64 and
# whole, because a stream parser's bugs live where a token straddles two
# reads. `0` means "hand over whatever is left".

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/apps/gopher -I userland/libos64/include -I abi/include \
   userland/apps/gopher/wire.c userland/libos64/url.c userland/libos64/str.c \
   userland/libos64/fmt.c tools/test_gopher_host.c \
   -o "$work/test_gopher"

python3 - "$work" <<'PY'
import pathlib
import subprocess
import sys
import urllib.parse

work = pathlib.Path(sys.argv[1])
exe = str(work / "test_gopher")
failures = 0


def fail(what, detail):
    global failures
    failures += 1
    print(f"FAIL {what}: {detail}")


def run(*args):
    done = subprocess.run([exe] + [str(a) for a in args], capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
    text = done.stdout.decode("latin-1")
    # `url` and `item` open with a verdict word; `lines` and `raw` have no
    # verdict to give and open straight into fields. A line with no '=' is
    # the verdict, everything else is a field — so both shapes read the same
    # way and neither swallows the other's first line.
    rows = text.split("\n")
    verdict = rows[0] if "=" not in rows[0] else ""
    out = {}
    for line in rows:
        if "=" in line:
            key, _, value = line.partition("=")
            out.setdefault(key, value)
    return verdict, out


def check(what, got, want):
    if got != want:
        fail(what, f"got {got!r} want {want!r}")


# The harness prints control bytes as escapes so a hostile fixture cannot
# repaint the screen of whoever is reading the output. Expectations are
# written the same way.
def visible(text):
    out = []
    for ch in text:
        code = ord(ch)
        out.append(f"\\x{code:02x}" if code < 0x20 or code == 0x7F else ch)
    return "".join(out)


# ── Addresses, cross-checked against urllib.parse where it can judge ─────
CROSS = [
    "gopher://gopher.floodgap.com/1/world",
    "gopher://gopher.floodgap.com:7070/0/file.txt",
    "gopher://example.com/",
    "gopher://example.com/1",
    "gopher://sdf.org:70/1/users",
]
for url in CROSS:
    verdict, got = run("url", url)
    if verdict != "ok":
        fail(f"url {url}", f"refused: {got.get('reason')}")
        continue
    ref = urllib.parse.urlsplit(url)
    check(f"host {url}", got["host"], ref.hostname)
    check(f"port {url}", int(got["port"]), ref.port or 70)
    # The type is the first byte of the path and the selector is the rest —
    # the split urllib does not make, since it has no opinion about gopher's
    # path shape. Everything before that split is what it IS checking.
    path = ref.path or "/"
    want_type = path[1] if len(path) > 1 else "1"
    check(f"type {url}", got["type"], want_type)
    check(f"selector {url}", got["selector"], visible(path[2:]))
    check(f"roundtrip {url}", got["roundtrip"], "same")
print(f"addresses: {len(CROSS)} cross-checked against urllib.parse")

# A bare host is a gopher host — this program's guess, not the library's.
verdict, got = run("url", "gopher.floodgap.com")
check("bare host verdict", verdict, "ok")
check("bare host port", got["port"], "70")
check("bare host type", got["type"], "1")
check("bare host selector", got["selector"], "")
check("bare host request", got["request"], "\\x0d\\x0a")

verdict, got = run("url", "example.com:7070")
check("bare host:port verdict", verdict, "ok")
check("bare host:port port", got["port"], "7070")

# A search: %09 is the query separator, a '?' is an ordinary byte.
verdict, got = run("url", "gopher://host/7/search%09what%20I%20want")
check("search verdict", verdict, "ok")
check("search selector", got["selector"], "/search")
check("search query", got["query"], "what I want")
check("search request", got["request"], "/search\\x09what I want\\x0d\\x0a")
check("search roundtrip", got["roundtrip"], "same")

verdict, got = run("url", "gopher://host/1/a?b=c")
check("question mark kept", got["selector"], "/a?b=c")

# Refusals, each by name.
REFUSED = {
    "http://a.com/x":          "not a gopher address",
    "https://a.com/x":         "not a gopher address",
    "gopher://":               "the address names no host",
    "gopher://u@a.com/1":      "the host holds something a host name cannot"
                               " (a user@, an IPv6 literal in brackets, or junk)",
    "gopher://[::1]/1":        "the host holds something a host name cannot"
                               " (a user@, an IPv6 literal in brackets, or junk)",
    "gopher://a.com:0/1":      "the port is not a number from 1 to 65535",
    "gopher://a.com:/1":       "the port is not a number from 1 to 65535",
    "gopher://a.com:99999/1":  "the port is not a number from 1 to 65535",
    "gopher://a.com/1/%zz":    "a '%' is not followed by two hex digits",
    "gopher://a.com/1/%":      "a '%' is not followed by two hex digits",
    # %00 is the one a later check cannot see: every check downstream is a C
    # string operation, and they all stop AT it.
    "gopher://a.com/1/a%00b":  "the selector holds a control character",
    "gopher://a.com/1/a%1bb":  "the selector holds a control character",
}
for url, reason in REFUSED.items():
    verdict, got = run("url", url)
    check(f"refuse {url}", verdict, "refused")
    check(f"reason {url}", got.get("reason"), reason)
print(f"addresses: {len(REFUSED)} refusals, each by name")

# ── Item lines ──────────────────────────────────────────────────────────
def item(line):
    return run("item", line.encode("latin-1").hex())


verdict, got = item("1A menu\t/sel\texample.com\t70")
check("item ok", verdict, "ok")
check("item type", got["type"], "1")
check("item display", got["display"], "A menu")
check("item host", got["host"], "example.com")
check("item port", got["port"], "70")
check("item followable", got["followable"], "1")
check("item framing", got["framing"], "menu")

# The type character is GLUED to the display name. A parser that expects a
# separator loses the first letter of every title in gopherspace.
verdict, got = item("0Read me\t/f.txt\texample.com\t70")
check("glued type", got["type"], "0")
check("glued display", got["display"], "Read me")
check("text framing", got["framing"], "text")

verdict, got = item("9archive.zip\t/a.zip\texample.com\t70")
check("binary framing", got["framing"], "binary")

# An `i` line's address fields are conventionally junk, and are not read.
verdict, got = item("iJust text\tfake\t(NULL)\t0")
check("info verdict", verdict, "info")
check("info display", got["display"], "Just text")
check("info followable", got["followable"], "0")

# Types nothing follows, each shown rather than hidden.
for ch, name in (("3", "error"), ("2", "phone"), ("8", "telnet"),
                 ("T", "tn3270"), ("+", "mirror"), ("X", "unknown")):
    verdict, got = item(f"{ch}Something\t/s\texample.com\t70")
    check(f"type {ch} not followable", got["followable"], "0")
    check(f"type {ch} named", got["typename"], name)

# EVERY TYPE NAME FITS THE LABEL COLUMN /bin/gopher draws it in. A longer one
# is clipped and reads as a typo rather than as a name — "phone book" came
# back from a real Floodgap menu and was drawn "phone b".
for ch in "0123456789+TdghisI":
    _, got = item(f"{ch}Something\t/s\texample.com\t70")
    if len(got["typename"]) > 7:
        fail(f"type name {ch}", f"{got['typename']!r} is longer than the column")

# ...and the ones it does.
for ch in "014567 9dgIs".replace(" ", ""):
    verdict, got = item(f"{ch}Something\t/s\texample.com\t70")
    check(f"type {ch} followable", got["followable"], "1")

# `h` is followed, but not over gopher: it is handed to os64get.
verdict, got = item("hA web page\tURL:http://example.com/\texample.com\t70")
check("h followable", got["followable"], "1")
check("h selector", got["selector"], "URL:http://example.com/")

# Lines that are not item lines. A parser that indexes blindly into the
# tab-split fields reads off the end of most of these.
MALFORMED = [
    "1no tabs at all",
    "1one tab\t/sel",
    "1two tabs\t/sel\texample.com",
    "1empty host\t/sel\t\t70",
    "1empty port\t/sel\texample.com\t",
    "1word port\t/sel\texample.com\tseventy",
    "1huge port\t/sel\texample.com\t999999",
    "1negative\t/sel\texample.com\t-1",
    "1bad host\t/sel\tnot a host\t70",
    "",
]
for line in MALFORMED:
    verdict, _ = item(line)
    check(f"malformed {line[:24]!r}", verdict, "malformed")

# A fifth field is Gopher+ marking its items. Ignored, not refused — refusing
# it would make a Gopher+ server's ordinary items unreadable.
verdict, got = item("1Plus item\t/sel\texample.com\t70\t+")
check("gopher+ trailing field", verdict, "ok")
check("gopher+ port", got["port"], "70")

# A MENU IS A STRANGER'S BYTES ON A TERMINAL THAT OBEYS ESCAPES.
HOSTILE = [
    "1clear the screen: \x1b[2J\t/sel\texample.com\t70",
    "1home the cursor: \x1b[H\t/sel\texample.com\t70",
    "1paint it red: \x1b[41;97m\t/sel\texample.com\t70",
    "1carriage return: over\rwritten\t/sel\texample.com\t70",
    "1a bell: \x07\t/sel\texample.com\t70",
    "1escape in selector\t/s\x1b[2Jel\texample.com\t70",
    "1escape in host\t/sel\t\x1b[2Jhost\t70",
    "1a DEL: \x7f\t/sel\texample.com\t70",
]
for line in HOSTILE:
    verdict, _ = item(line)
    check(f"hostile {line[1:24]!r}", verdict, "refused")
print(f"item lines: {len(MALFORMED)} malformed and {len(HOSTILE)} hostile, all refused")

# A HOST FIELD THAT REWRITES THE URL IT IS SPELLED INTO. The host and port are
# judged by handing `gopher://<host>:<port>/` to the shared parser, so a field
# carrying a URL delimiter changes the shape of that spelling BEFORE the parser
# reads it: `/`, `?` and `#` each end an authority, and `#` also starts a
# fragment the port never sees. Each of these parses cleanly into an address
# that is not the one the menu line named, which is the whole reason the parse
# is checked back against the fields it came from.
SMUGGLED = [
    ("1slash in host\t/sel\tevil.com/path\t70",     "evil.com"),
    ("1query in host\t/sel\tevil.com?x\t70",        "evil.com"),
    ("1fragment in host\t/sel\tevil.com#x\t70",     "evil.com"),
    ("1slash in port\t/sel\texample.com\t70/x",     "example.com"),
    ("1fragment in port\t/sel\texample.com\t70#junk", "example.com"),
    ("1empty then junk\t/sel\texample.com\t70?q",   "example.com"),
]
for line, would_have_been in SMUGGLED:
    verdict, got = item(line)
    if verdict == "ok":
        fail(f"smuggled {line[1:24]!r}",
             f"followed host {got.get('host')!r}:{got.get('port')} instead of refusing"
             f" (the field said something else; {would_have_been!r} is what it collapses to)")
    else:
        check(f"smuggled {line[1:24]!r}", verdict, "malformed")
print(f"item lines: {len(SMUGGLED)} delimiter-smuggling hosts and ports refused")

# A high byte is NOT a control byte. Somebody's Latin-1 menu from 1994 is not
# an attack, and what a byte over 0x7F looks like is the font's business.
verdict, got = item("1caf\xe9\t/sel\texample.com\t70")
check("high byte kept", verdict, "ok")
check("high byte display", got["display"], "caf\xe9")

# ── Framing ─────────────────────────────────────────────────────────────
CHUNKS = (1, 2, 3, 7, 17, 64, 0)

MENU = ("iHello\tfake\t(NULL)\t0\r\n"
        "1A menu\t/sel\texample.com\t70\r\n"
        "0A file\t/f.txt\texample.com\t70\r\n"
        ".\r\n")


def lines(data, chunk, unstuff=False):
    return run("lines", chunk, 1 if unstuff else 0, data.encode("latin-1").hex())


for chunk in CHUNKS:
    _, got = lines(MENU, chunk)
    check(f"menu count @{chunk}", got["count"], "3")
    check(f"menu terminated @{chunk}", got["terminated"], "1")
    check(f"menu line1 @{chunk}", got["line1"],
          visible("1A menu\t/sel\texample.com\t70"))

# A server that sends bare LF instead of CRLF. Plenty do.
for chunk in CHUNKS:
    _, got = lines(MENU.replace("\r\n", "\n"), chunk)
    check(f"bare LF count @{chunk}", got["count"], "3")
    check(f"bare LF terminated @{chunk}", got["terminated"], "1")

# NO TERMINATOR: the close is the end. Common in the wild, and not an error —
# `terminated` says which happened, so a caller can tell a whole answer from
# a cut one without either being a failure.
for chunk in CHUNKS:
    _, got = lines(MENU[:-3], chunk)
    check(f"nodot count @{chunk}", got["count"], "3")
    check(f"nodot terminated @{chunk}", got["terminated"], "0")
    check(f"nodot failed @{chunk}", got["failed"], "0")

# RFC 1436's transparency rule, undone — for TEXT.
TEXT = "ordinary\r\n..\r\n...\r\n.. dotted words\r\nplain\r\n.\r\n"
for chunk in CHUNKS:
    _, got = lines(TEXT, chunk, unstuff=True)
    check(f"text count @{chunk}", got["count"], "5")
    check(f"unstuff '..' @{chunk}", got["line1"], ".")
    check(f"unstuff '...' @{chunk}", got["line2"], "..")
    check(f"unstuff '.. w' @{chunk}", got["line3"], ". dotted words")
    check(f"text terminated @{chunk}", got["terminated"], "1")

# A MENU IS NOT UNSTUFFED: a leading period there is a type character, and
# eating one would change what the line says it is.
_, got = lines("..odd\r\n.\r\n", 1)
check("menu not unstuffed", got["line0"], "..odd")

# A lone leading period on a server that never stuffed anything survives.
_, got = lines(".hidden\r\nplain\r\n.\r\n", 1, unstuff=True)
check("unstuffing needs two dots", got["line0"], ".hidden")

# An over-long line is truncated and the rest skipped to the newline, so the
# line AFTER it still parses. Losing a tail beats losing the menu.
LONG = ("1" + "x" * 40000 + "\t/s\texample.com\t70\r\n"
        "1after\t/s\texample.com\t70\r\n.\r\n")
for chunk in (1024, 0):
    _, got = lines(LONG, chunk)
    check(f"overlong truncated @{chunk}", got["truncated"], "1")
    check(f"overlong next line @{chunk}", got["line1"],
          visible("1after\t/s\texample.com\t70"))
    check(f"overlong terminated @{chunk}", got["terminated"], "1")

# BINARY: bytes to the close. No terminator exists, so a '.' line is DATA —
# which is why the type that led here is the only thing that can say so.
BLOB = "".join(chr(b) for b in range(256)) * 4 + "\r\n.\r\n"
seen = set()
for chunk in (1, 2, 13, 4096, 0):
    _, got = run("raw", chunk, BLOB.encode("latin-1").hex())
    check(f"raw bytes @{chunk}", got["bytes"], str(len(BLOB)))
    seen.add(got["hash"])
check("raw identical at every chunk size", len(seen), 1)
print(f"framing: menus, text, binaries at chunk sizes {CHUNKS}")

# ── The response ceiling ends the answer, rather than marking one line ──
#
# A peer that never stops talking must be cut off by GOPHER_RESPONSE_MAX and
# not by patience — and that has to hold for a peer sending ONE endless line
# as much as for one sending an endless supply of short ones. Marking only
# the line the ceiling landed in left `bytes` over the cap and every later
# call tripping the same check, handing back one byte at a time for as long
# as the caller kept asking. `hit_cap` is the shape of that failure: the
# harness stopped it, nothing in the parser did.
#
# The bytes are manufactured inside the harness because 8MB cannot travel in
# argv, and the limits are read back from it so they are not spelled twice.
for linelen, what in ((0, "one line that never ends"),
                      (4000, "an endless supply of lines")):
    _, got = run("flood", linelen, 20000)
    cap = int(got["response_max"])
    check(f"ceiling ends {what}", got["ceiling"], "1")
    check(f"ceiling truncates {what}", got["truncated"], "1")
    check(f"ceiling is not an error: {what}", got["failed"], "0")
    check(f"ceiling is not a terminator: {what}", got["terminated"], "0")
    if got["hit_cap"] == "1":
        fail(f"ceiling {what}",
             f"produced {got['count']} lines and was still going — the cap"
             f" stopped it, not the {cap}-byte ceiling")
    # One buffer of overshoot is the honest amount: the check runs per byte,
    # so the read that crosses the cap has already been taken whole.
    served, slack = int(got["served"]), 2 * int(got["buf_size"])
    if served > cap + slack:
        fail(f"ceiling overshoot {what}",
             f"read {served} bytes against a {cap}-byte cap"
             f" ({served - cap} past it)")
print("ceiling: an endless peer is cut off at the response cap, not by patience")

if failures:
    print(f"test_gopher_host: {failures} FAILED")
    sys.exit(1)
print("test_gopher_host: all checks passed")
PY
