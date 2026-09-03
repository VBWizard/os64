#!/bin/bash
# Cross-check os64get's HTTP parser against Python's, across chunk boundaries.
#
# Two references, both external to this tree: http.client for what a reply
# means, urllib.parse for what a URL means. Where os64get is deliberately
# STRICTER than either (a space before a colon, two disagreeing lengths, a
# scheme with no TLS behind it), the expectation is spelled out here by hand
# and the reason is in http.c beside the refusal.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/apps/os64get -I userland/libos64/include -I abi/include \
   userland/apps/os64get/http.c userland/libos64/str.c userland/libos64/fmt.c \
   tools/test_http_host.c \
   -o "$work/test_http"

python3 - "$work" <<'PY'
import http.client
import io
import pathlib
import random
import subprocess
import sys
import urllib.parse

work = pathlib.Path(sys.argv[1])
exe = str(work / "test_http")
failures = 0


def fail(what, detail):
    global failures
    failures += 1
    print(f"FAIL {what}: {detail}")


def run(args):
    # NOT text=True: universal newlines would fold every CRLF the parser
    # emits into a bare LF, and CRLF is exactly what is under test here.
    done = subprocess.run([exe] + [str(a) for a in args], capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"harness failed ({done.returncode}): {done.stderr!r}")
    text = done.stdout.decode("latin-1")
    lines = text.split("\n")
    verdict = lines[0]
    fields = {}
    for line in lines[1:]:
        if "=" in line:
            key, _, value = line.partition("=")
            fields.setdefault(key, value)
    return verdict, fields, text


# ── URLs ────────────────────────────────────────────────────────────────
# The well-formed ones are checked against urllib.parse: same host, same
# port, same path-with-query, fragment gone.
good_urls = [
    "http://example.com",
    "http://example.com/",
    "http://example.com/index.html",
    "http://example.com:8080/a/b/c.txt",
    "http://example.com/a/b/c.txt?x=1&y=2",
    "http://example.com/a?x=1#frag",
    "http://example.com?x=1",
    "http://example.com#top",
    "http://10.0.2.2:6464/thing",
    "HTTP://Example.COM/Case/Kept",
    "http://a-b_c.example.com:65535/",
    "http://textfiles.com/computers/",
    "http://" + "a" * 60 + "." + "b" * 60 + ".example.com/deep/" + "p" * 900,
    # https PARSES — whether it can be FETCHED is a question about the
    # machine (is there a proxy?) and is answered by os64get, not here. The
    # default port comes from the scheme.
    "https://example.com/",
    "https://example.com",
    "https://example.com:8443/secure/page.html?q=1",
    "https://example.com:443/explicit-default",
    "HTTPS://Example.COM/Case",
]
for text in good_urls:
    verdict, got, out = run(["url", text])
    if verdict != "ok":
        fail(text, f"refused as {verdict}")
        continue
    want = urllib.parse.urlsplit(text)
    wantScheme = want.scheme.lower()
    wantDefault = 443 if wantScheme == "https" else 80
    wantPath = want.path or "/"
    if want.query:
        wantPath += "?" + want.query
    if got["host"] != want.hostname:
        fail(text, f"host {got['host']!r} != {want.hostname!r}")
    if int(got["port"]) != (want.port or wantDefault):
        fail(text, f"port {got['port']} != {want.port or wantDefault}")
    if got["path"] != wantPath:
        fail(text, f"path {got['path']!r} != {wantPath!r}")
    if got["scheme"] != wantScheme:
        fail(text, f"scheme {got['scheme']!r} != {wantScheme!r}")

    # Rendering back out must round-trip everything the parse kept — and must
    # have dropped the fragment and a redundant default port, by construction.
    wantRender = f"{wantScheme}://{want.hostname.lower()}"
    if want.port and want.port != wantDefault:
        wantRender += f":{want.port}"
    wantRender += wantPath
    if got["render"] != wantRender:
        fail(text, f"render {got['render']!r} != {wantRender!r}")
    if "#" in got["render"]:
        fail(text, f"render kept a fragment: {got['render']!r}")

    # The request line carries the path verbatim; Host carries the port only
    # when it is not the scheme's default.
    head = out.split("request<<\n", 1)[1].split("\n>>\nproxyrequest", 1)[0] + "\n"
    first = head.split("\r\n")[0]
    if first != f"GET {wantPath} HTTP/1.0":
        fail(text, f"request line {first!r}")
    hostLine = [l for l in head.split("\r\n") if l.lower().startswith("host:")][0]
    wantHost = want.hostname + (f":{want.port}" if want.port and want.port != wantDefault else "")
    if hostLine != f"Host: {wantHost}":
        fail(text, f"host header {hostLine!r} != {wantHost!r}")
    if "\r\n\r\n" not in head:
        fail(text, "request has no blank line")

    # Addressed to a PROXY: the whole URL in the request line (absolute-form,
    # RFC 7230 §5.3.2), Host still naming the origin. Getting these two
    # backwards is the classic proxy bug — the path alone would name a file on
    # the proxy, and a Host naming the proxy would lose the virtual host.
    proxyHead = out.split("proxyrequest<<\n", 1)[1].rsplit(">>\n", 1)[0]
    proxyFirst = proxyHead.split("\r\n")[0]
    if proxyFirst != f"GET {wantRender} HTTP/1.0":
        fail(text, f"proxy request line {proxyFirst!r} != 'GET {wantRender} HTTP/1.0'")
    proxyHostLine = [l for l in proxyHead.split("\r\n") if l.lower().startswith("host:")][0]
    if proxyHostLine != f"Host: {wantHost}":
        fail(text, f"proxy host header {proxyHostLine!r} != {wantHost!r}")

# Refusals, each named. These are the cases where os64get is stricter than
# urllib on purpose.
bad_urls = {
    "os64_kernel": "not_a_url",              # a valet name, not a URL
    "10.0.2.2": "not_a_url",
    "example.com/thing": "not_a_url",        # no scheme: wget guesses, os64get does not
    # https is NOT here any more: it parses, and whether it can be fetched is
    # os64get's question, not the parser's.
    "ftp://example.com/": "scheme",
    "gopher://gopher.floodgap.com/": "scheme",
    "http://": "no_host",
    "http:///path": "no_host",
    "http://user@example.com/": "host_chars",
    "http://[::1]/": "host_chars",
    "http://exa mple.com/": "host_chars",
    "http://:80/": "no_host",
    "http://example.com:/": "port",
    "http://example.com:0/": "port",
    "http://example.com:65536/": "port",
    "http://example.com:80x/": "port",
    "http://example.com/a b": "path_chars",
    "http://example.com/a\tb": "path_chars",
    "http://" + "h" * 300 + "/": "too_long",
    "http://example.com/" + "p" * 2000: "too_long",
}
for text, want in bad_urls.items():
    verdict, _, _ = run(["url", text])
    if verdict != want:
        fail(text, f"{verdict} != {want}")



# ── Location -> a whole address ─────────────────────────────────────────
# What the redirect advice offers when it says "to follow it by hand". The
# forms os64get claims to resolve are checked against urllib's urljoin; the
# ones it deliberately declines are checked by name, because declining is the
# answer, not a shortfall to be papered over.
join_cases = [
    ("http://example.com/a/b.html", "http://other.example/x"),
    ("http://example.com/a/b.html", "https://other.example/x"),
    ("http://example.com/a/b.html", "/top.html"),
    ("http://example.com/a/b.html", "/"),
    ("http://example.com:8080/a/b.html", "/top.html"),
    ("http://example.com:80/a/b.html", "/top.html"),
    ("http://example.com/a/b.html", "/q?x=1&y=2"),
    ("http://10.0.2.2:8080/redirect-https", "https://example.com/"),
    ("http://10.0.2.2:8080/redirect", "/hello.txt"),
# The redirect helper must keep the BASE's scheme. Hard-coding http sent an
# https page's `/login` back as `http://host:443/login` — plaintext at a TLS
# port, printed as a command to copy. (Codex, PR #52.)
    ("https://example.com/a/b.html", "/login"),
    ("https://example.com/", "/"),
    ("https://example.com:8443/a/b.html", "/top.html"),
    ("https://example.com:443/a/b.html", "/top.html"),
    ("https://example.com/a/b.html", "http://plain.example/x"),
    # Scheme-relative: the AUTHORITY comes from the reference, only the scheme
    # is inherited (RFC 3986 §4.2). Treating it as a path pointed the command
    # back at the server that had just redirected away from itself.
    ("https://example.com/a/b.html", "//cdn.example.com/file"),
    ("http://example.com/a/b.html", "//cdn.example.com/file?x=1"),
    ("http://example.com:8080/a/b.html", "//cdn.example.com/"),
    # A URL inside the QUERY of a root-relative redirect. The "://" is there
    # but it does not START the reference, so this must be joined to the base
    # — printing it unchanged produced a command the CLI read as the valet
    # dialect and refused. (Codex round 3.)
    ("https://example.com/a/b.html", "/login?next=https://id.example/"),
    ("http://example.com/a/b.html", "/go?u=http://other.example/x&v=1"),
]
for base, location in join_cases:
    verdict, got, _ = run(["absolute", base, location])
    if verdict != "absolute":
        fail(f"absolute {base} + {location}", f"declined it ({verdict})")
        continue
    want = urllib.parse.urljoin(base, location)
    # urljoin keeps an explicit default port; os64get drops it for the same
    # reason the Host header does, so compare the parsed pieces rather than
    # the spelling — and default the port PER SCHEME, or https://host and
    # https://host:443 read as different places.
    a, b = urllib.parse.urlsplit(got["url"]), urllib.parse.urlsplit(want)
    default = lambda s: 443 if s == "https" else 80
    if (a.scheme, a.hostname, a.port or default(a.scheme), a.path, a.query) != \
       (b.scheme, b.hostname, b.port or default(b.scheme), b.path, b.query):
        fail(f"absolute {base} + {location}", f"{got['url']!r} != {want!r}")

# Declined on purpose: a genuinely relative reference wants RFC 3986's full
# algorithm, which belongs to the increment that follows redirects for real.
for base, location in [("http://example.com/a/b.html", "page.html"),
                       ("http://example.com/a/b.html", "../up.html"),
                       ("http://example.com/a/b.html", "")]:
    verdict, _, _ = run(["absolute", base, location])
    if verdict != "relative":
        fail(f"absolute {base} + {location}", f"resolved it ({verdict}) — it should decline")

# ── Replies ─────────────────────────────────────────────────────────────
rng = random.Random(0x05064A17)


def reply_bytes(status, reason, headers, body, version="HTTP/1.1"):
    head = f"{version} {status} {reason}\r\n"
    for name, value in headers:
        head += f"{name}: {value}\r\n"
    return head.encode("latin-1") + b"\r\n" + body


# http.client stops at 100 header fields; os64get's cap is 128, and the
# boundary case has to be checked against a reference that will read it.
http.client._MAXHEADERS = 256


def reference(raw):
    """What http.client makes of the same bytes."""
    class FakeSocket:
        def __init__(self, data):
            self.data = io.BytesIO(data)

        def makefile(self, *args, **kwargs):
            return self.data

    reply = http.client.HTTPResponse(FakeSocket(raw))
    reply.begin()
    return reply


cases = []
bodies = {
    "empty": b"",
    "short": b"<html>hi</html>\n",
    "text": b"the quick brown fox jumps over the lazy dog\n" * 977,
    "binary": bytes(rng.randrange(256) for _ in range(40000)),
}

for name, body in bodies.items():
    cases.append((f"{name}-length", reply_bytes(
        200, "OK",
        [("Server", "os64test/1"), ("Content-Type", "text/plain"),
         ("Content-Length", str(len(body))), ("Connection", "close")],
        body), body, True))
    # No Content-Length: HTTP/1.0's original framing — the close is the end.
    cases.append((f"{name}-close", reply_bytes(
        200, "OK", [("Content-Type", "text/plain")], body, "HTTP/1.0"),
        body, True))

# Header shapes a real server produces: odd casing, generous whitespace, a
# reason phrase with spaces in it, and a header line long enough to be
# dropped (the drop must not cost the fetch).
body = b"payload\n" * 64
cases.append(("odd-case", reply_bytes(
    200, "OK Then",
    [("content-LENGTH", f"   {len(body)}   "), ("X-Empty", ""),
     ("Set-Cookie", "c=" + "x" * 3000)],
    body), body, True))
cases.append(("interim", b"HTTP/1.1 100 Continue\r\n\r\n" + reply_bytes(
    200, "OK", [("Content-Length", str(len(body)))], body), body, True))
cases.append(("bare-lf", b"HTTP/1.1 200 OK\nContent-Length: %d\n\n" % len(body) + body,
              body, True))
cases.append(("no-reason", reply_bytes(
    200, "", [("Content-Length", str(len(body)))], body), body, True))
# A CONTENT coding of identity beside a length is fine — the coding says the
# bytes are untouched, and the length frames them. A TRANSFER coding beside a
# length is not (see "te-identity-and-length" below): the coding is then the
# framing and the length is noise. So identity gets two cases, not one.
cases.append(("identity-coding", reply_bytes(
    200, "OK", [("Content-Length", str(len(body))),
                ("Content-Encoding", "identity")], body), body, True))
cases.append(("identity-transfer-close-delimited", reply_bytes(
    200, "OK", [("Transfer-Encoding", "identity")], body), body, True))
cases.append(("repeated-length", reply_bytes(
    200, "OK", [("Content-Length", str(len(body))),
                ("Content-Length", str(len(body)))], body), body, True))
cases.append(("headerless", reply_bytes(404, "Not Found", [], b""), b"", True))
# Exactly HTTP_HEADERS_MAX fields (127 padding plus the length) is LEGAL: the
# cap used to charge for the blank line, so the advertised 128 was really
# 127 and a reply sitting on the boundary was refused as too much. (Codex
# review round 4, 2026-09-03.)
cases.append(("max-headers", reply_bytes(
    200, "OK", [("X-Pad", str(i)) for i in range(127)] + [("Content-Length", str(len(body)))],
    body), body, True))
cases.append(("redirect", reply_bytes(
    301, "Moved Permanently",
    [("Location", "http://example.com/elsewhere"), ("Content-Length", "0")],
    b""), b"", True))
# A tab inside a value and non-ASCII (obs-text) in a reason are LEGAL field
# bytes; the control-byte refusals below must not reach them.
cases.append(("tab-in-value", reply_bytes(
    200, "OK déjà", [("X-Tab", "a\tb"), ("Content-Length", str(len(body)))],
    body), body, True))

for label, raw, body, checkReference in cases:
    path = work / f"reply-{label}"
    path.write_bytes(raw)

    if checkReference:
        try:
            ref = reference(raw)
            refBody = ref.read()
        except Exception as error:              # noqa: BLE001 - the reference's verdict IS the datum
            fail(label, f"http.client refused it: {error}")
            continue
        if refBody != body:
            fail(label, "the fixture disagrees with http.client about its own body")

    # Every chunk size from one byte up, then a few big ones, crossed with a
    # small and a large caller buffer (HTTP_BUF_SIZE is 8192, so 16384 takes
    # the read-straight-into-the-caller path and 64 takes the buffered one).
    for chunk in [1, 2, 3, 7, 64, 1000, 8191, 8192, 8193, 65536, 0]:
        for sip in [1, 64, 4096, 16384]:
            out = work / "body.out"
            verdict, got, _ = run(["head", path, chunk, sip, 0, out])
            if verdict != "ok":
                fail(f"{label} chunk={chunk} sip={sip}", f"head refused: {verdict}")
                break
            if int(got["status"]) != ref.status:
                fail(f"{label} chunk={chunk}", f"status {got['status']} != {ref.status}")
            # http.client joins repeated headers with ", "; RFC 7230 lets a
            # recipient collapse identical Content-Lengths to the one value,
            # which is what os64get does.
            wantLength = ref.getheader("Content-Length")
            if wantLength is not None and "," in wantLength:
                wantLength = wantLength.split(",")[0].strip()
            if (got["haslength"] == "1") != (wantLength is not None):
                fail(f"{label} chunk={chunk}", f"haslength {got['haslength']}")
            if wantLength is not None and int(got["length"]) != int(wantLength):
                fail(f"{label} chunk={chunk}", f"length {got['length']} != {wantLength}")
            if out.read_bytes() != body:
                fail(f"{label} chunk={chunk} sip={sip}",
                     f"body {len(out.read_bytes())} bytes != {len(body)}")
            if got["short"] != "0" or got["broke"] != "0":
                fail(f"{label} chunk={chunk}", f"short={got['short']} broke={got['broke']}")
            loc = ref.getheader("Location") or ""
            if got["location"] != loc:
                fail(f"{label} chunk={chunk}", f"location {got['location']!r} != {loc!r}")

# ── Replies that must be REFUSED ────────────────────────────────────────
body = b"x" * 32
refusals = {
    "not-http": (b"220 smtpd ready\r\n\r\n", "status"),
    "empty": (b"", "source"),
    "truncated-head": (b"HTTP/1.1 200 OK\r\nContent-Length: 32\r\n", "source"),
    "no-blank-line": (b"HTTP/1.1 200 OK\r\nContent-Length: 32\r\n" + body, "source"),
    "short-status": (b"HTTP/1.1 20 OK\r\n\r\n", "status"),
    "long-status": (b"HTTP/1.1 2000 OK\r\n\r\n", "status"),
    "no-status": (b"HTTP/1.1\r\n\r\n", "status"),
    "space-before-colon": (b"HTTP/1.1 200 OK\r\nContent-Length : 32\r\n\r\n", "syntax"),
    # obs-fold: a line beginning with whitespace is a continuation, deprecated
    # by RFC 7230 §3.2.4 and refused here because it can hide a framing header
    # under a name that matches nothing.
    "folded-line": (b"HTTP/1.1 200 OK\r\n  Transfer-Encoding: chunked\r\n\r\n", "syntax"),
    "folded-tab": (b"HTTP/1.1 200 OK\r\n\tContent-Length: 32\r\n\r\n", "syntax"),
    "no-colon": (b"HTTP/1.1 200 OK\r\nContent-Length 32\r\n\r\n", "syntax"),
    "empty-name": (b"HTTP/1.1 200 OK\r\n: 32\r\n\r\n", "syntax"),
    "bad-length": (b"HTTP/1.1 200 OK\r\nContent-Length: 32x\r\n\r\n", "syntax"),
    "negative-length": (b"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", "syntax"),
    # Two answers to one question is the request-smuggling shape, and there
    # is no correct guess between them.
    "two-lengths": (b"HTTP/1.1 200 OK\r\nContent-Length: 32\r\nContent-Length: 33\r\n\r\n",
                    "conflict"),
    "two-encodings": (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                      b"Transfer-Encoding: identity\r\n\r\n", "conflict"),
    # A transfer coding AND a length is the same two-answers shape: the coding
    # is the framing and the length is to be ignored (RFC 9112 §6.3), so a
    # body read to the length is a prefix published as the whole. `identity`
    # is the sly form — it looks like "no framing", and a longer body behind
    # it would be silently cut. (Codex review round 6, 2026-09-03.)
    "te-identity-and-length": (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n"
                               b"Content-Length: 32\r\n\r\n" + body + b"and then some more",
                               "conflict"),
    "te-chunked-and-length": (b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                              b"Transfer-Encoding: chunked\r\n\r\n", "conflict"),
    "too-many-headers": (b"HTTP/1.1 200 OK\r\n" + b"X-Pad: 1\r\n" * 200 + b"\r\n",
                         "too_much"),
    # One past the cap — and "max-headers" above proves the cap itself is
    # accepted, so between them the count is exact.
    "one-header-too-many": (b"HTTP/1.1 200 OK\r\n" + b"X-Pad: 1\r\n" * 129 + b"\r\n",
                            "too_much"),
    # A NUL byte anywhere in the head. The line is handed on as a C string,
    # so a NUL that LED a line made it read as the blank terminator, and the
    # framing header behind it became body: the raw chunk markers were the
    # file. No legal head holds one, so every position is refused. (Codex
    # review round 4, 2026-09-03.)
    "nul-leads-header": (b"HTTP/1.1 200 OK\r\n\x00Transfer-Encoding: chunked\r\n\r\n"
                         b"20\r\nthirty-two bytes of chunked body\r\n0\r\n\r\n", "syntax"),
    "nul-in-value": (b"HTTP/1.1 200 OK\r\nContent-Length: 32\x00\r\n\r\n" + body, "syntax"),
    "nul-in-name": (b"HTTP/1.1 200 OK\r\nX-\x00Pad: 1\r\n\r\n", "syntax"),
    "nul-in-status": (b"HTTP/1.1 200 OK\x00\r\n\r\n", "status"),
    "nul-leads-status": (b"\x00HTTP/1.1 200 OK\r\n\r\n", "status"),
    # Any other control byte in the origin's WORDS — the reason phrase, a
    # Location — is refused too: os64get prints both to the terminal, and
    # ESC [ 2 J in a reason clears the screen of whoever ran it. A tab is
    # the one control a value may hold (see "tab-in-value" above). (Codex
    # review round 5, 2026-09-03.)
    "escape-in-reason": (b"HTTP/1.1 200 OK\x1b[2J\r\n\r\n", "status"),
    "escape-in-value": (b"HTTP/1.1 302 Found\r\nLocation: /x\x1b[2J\r\nContent-Length: 0\r\n\r\n",
                        "syntax"),
    "del-in-value": (b"HTTP/1.1 200 OK\r\nX-A: a\x7fb\r\n\r\n", "syntax"),
    "bell-in-length": (b"HTTP/1.1 200 OK\r\nContent-Length: \x0732\r\n\r\n" + body, "syntax"),
    "endless-interim": (b"HTTP/1.1 100 Continue\r\n\r\n" * 20, "too_much"),
    # A peer that talks forever and never sends a newline. The head's cap is
    # checked BETWEEN lines, so without a budget inside the line reader this
    # one never returns to be capped at all: the test that would hang is the
    # test that matters.
    "flood-status": (b"H" * (1 << 20), "status"),
    "flood-header": (b"HTTP/1.1 200 OK\r\n" + b"X" * (1 << 20), "too_much"),
}
for label, (raw, want) in refusals.items():
    path = work / f"bad-{label}"
    path.write_bytes(raw)
    for chunk in [1, 3, 64, 0]:
        verdict, _, _ = run(["head", path, chunk, 64, 0, work / "body.out"])
        if verdict != want:
            fail(f"refusal {label} chunk={chunk}", f"{verdict} != {want}")

# ── The P1: an over-long header that DECIDES THE FRAMING ────────────────
# The drop rule for long header lines is safe only for headers nothing
# depends on. A server picks the length of its own lines, and RFC 7230 allows
# unlimited optional whitespace after the colon — so these hide a framing
# header behind padding and must be REFUSED, not dropped. Found by Codex on
# PR #52; the rule was right and its premise ("the headers this acts on are
# all short") was the server's to break.
pad = b" " * 3000
# Each entry is (bytes, the refusal it must earn). "framing" means the name
# was legible and names a header the body's reading depends on; "syntax"
# means the name was not a legal token at all, which the SHORT form of the
# same line is also refused for — the two paths judge a name by one rule now,
# and that is the property under test.
framing_hidden = {
    "long-transfer-encoding": (b"HTTP/1.1 200 OK\r\nTransfer-Encoding:" + pad + b"chunked\r\n\r\n", "framing"),
    "long-content-encoding": (b"HTTP/1.1 200 OK\r\nContent-Encoding:" + pad + b"gzip\r\n\r\n", "framing"),
    "long-content-length": (b"HTTP/1.1 200 OK\r\nContent-Length:" + pad + b"5\r\n\r\nhello", "framing"),
    # Cased differently, because the compare must not care.
    "long-te-cased": (b"HTTP/1.1 200 OK\r\ntRaNsFeR-eNcOdInG:" + pad + b"chunked\r\n\r\n", "framing"),
    # Whitespace before the name is obs-fold, and it hides a framing header
    # exactly as trailing padding does — the same hole through a second door,
    # found while writing this test rather than by Codex.
    "folded-transfer-encoding": (b"HTTP/1.1 200 OK\r\n  Transfer-Encoding:" + pad + b"chunked\r\n\r\n", "syntax"),
    # A THIRD door (Codex round 3): whitespace BEFORE the colon. The short
    # form is refused by header_take; the long form used to carry the blank
    # into the name, match nothing, and be dropped.
    "long-te-space-before-colon": (b"HTTP/1.1 200 OK\r\nTransfer-Encoding :" + pad + b"chunked\r\n\r\n", "syntax"),
    "long-cl-space-before-colon": (b"HTTP/1.1 200 OK\r\nContent-Length :" + pad + b"5\r\n\r\nhello", "syntax"),
    "long-te-tab-before-colon": (b"HTTP/1.1 200 OK\r\nTransfer-Encoding\t:" + pad + b"chunked\r\n\r\n", "syntax"),
    # Any other byte a field name may not hold, for the same reason.
    "long-te-inner-space": (b"HTTP/1.1 200 OK\r\nTransfer Encoding:" + pad + b"chunked\r\n\r\n", "syntax"),
}
for label, (raw, want) in framing_hidden.items():
    path = work / f"framing-{label}"
    path.write_bytes(raw)
    for chunk in [1, 7, 64, 4096, 0]:
        verdict, _, _ = run(["head", path, chunk, 64, 0, work / "body.out"])
        if verdict != want:
            fail(f"framing {label} chunk={chunk}",
                 f"{verdict} != {want} — an unreadable framing header was DROPPED")

# The SHORT form of each malformed name must earn the same answer, because a
# server must not be able to pick which parser it faces by padding a line.
short_malformed = {
    "short-te-space-before-colon": b"HTTP/1.1 200 OK\r\nTransfer-Encoding : chunked\r\n\r\n",
    "short-te-tab-before-colon": b"HTTP/1.1 200 OK\r\nTransfer-Encoding\t: chunked\r\n\r\n",
    "short-te-inner-space": b"HTTP/1.1 200 OK\r\nTransfer Encoding: chunked\r\n\r\n",
}
for label, raw in short_malformed.items():
    path = work / f"short-{label}"
    path.write_bytes(raw)
    for chunk in [1, 7, 0]:
        verdict, _, _ = run(["head", path, chunk, 64, 0, work / "body.out"])
        if verdict != "syntax":
            fail(f"short {label} chunk={chunk}", f"{verdict} != syntax")

# ...and the tolerance the drop rule exists for must survive. A giant
# Set-Cookie is real on the web, os64get does not read cookies, and refusing
# the whole fetch over one would be the wrong trade.
body = b"payload\n" * 64
tolerated = (b"HTTP/1.1 200 OK\r\nSet-Cookie: c=" + b"x" * 4000 +
             b"\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
path = work / "framing-tolerated"
path.write_bytes(tolerated)
for chunk in [1, 7, 64, 4096, 0]:
    verdict, got, _ = run(["head", path, chunk, 64, 0, work / "body.out"])
    if verdict != "ok":
        fail(f"tolerated chunk={chunk}", f"a long Set-Cookie was refused: {verdict}")
    elif int(got["length"]) != len(body) or (work / "body.out").read_bytes() != body:
        fail(f"tolerated chunk={chunk}", "the long header cost the body")

# ── A connection that breaks mid-body ───────────────────────────────────
# The head parses, the body does not arrive, and the caller must be able to
# tell: broke and short, never a quiet success on a fragment.
body = b"y" * 5000
raw = reply_bytes(200, "OK", [("Content-Length", str(len(body)))], body)
path = work / "reply-cut"
path.write_bytes(raw)
headLen = raw.index(b"\r\n\r\n") + 4
for chunk in [1, 64, 4096, 0]:
    verdict, got, _ = run(["head", path, chunk, 64, headLen + 1000, work / "body.out"])
    if verdict != "ok":
        fail(f"cut chunk={chunk}", f"head refused: {verdict}")
        continue
    if got["broke"] != "1" or got["short"] != "1":
        fail(f"cut chunk={chunk}", f"broke={got['broke']} short={got['short']}")
    if int(got["bodylen"]) != 1000:
        fail(f"cut chunk={chunk}", f"bodylen {got['bodylen']} != 1000")

# A peer that simply hangs up early: no error, just fewer bytes than promised.
truncated = work / "reply-truncated"
truncated.write_bytes(raw[:headLen + 2000])
for chunk in [1, 64, 0]:
    verdict, got, _ = run(["head", truncated, chunk, 64, 0, work / "body.out"])
    if verdict != "ok":
        fail(f"truncated chunk={chunk}", f"head refused: {verdict}")
        continue
    if got["broke"] != "0" or got["short"] != "1" or int(got["bodylen"]) != 2000:
        fail(f"truncated chunk={chunk}",
             f"broke={got['broke']} short={got['short']} bodylen={got['bodylen']}")

if failures:
    raise SystemExit(f"{failures} failure(s)")
print("test_http_host: all checks passed")
PY
