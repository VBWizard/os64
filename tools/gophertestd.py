#!/usr/bin/env python3
r"""gophertestd — a deterministic Gopher server for driving /bin/gopher.

httptestd.py's sibling, and built for the same reason: the answers worth
testing are the ones a well-behaved server will not produce on request. A
menu whose display name carries an ESC sequence. A text file the server ends
by hanging up instead of sending the period. An item line with three tabs
where the protocol wants three fields. Floodgap will not serve you any of
those, and those are exactly where a client breaks.

    python3 gophertestd.py [--port N] [--dump DIR]

Default port is 7070, not 70: 70 is privileged, and nothing here needs root.
`--port` is spelled the way httptestd.py and os64serve.py spell it — these
sit in one directory and are started from one prompt, and one of them wanting
a bare number while the others took a flag is the kind of difference you
rediscover at 1am. A bare number still works.

WHERE TO RUN IT depends on which machine is fetching, exactly as httptestd.py
explains at length: from WSL2 the QEMU guest reaches this at 10.0.2.2; the P5
needs it started on the WINDOWS side, because WSL2 sits behind a NAT of its
own and a listener inside it is not reachable from the room.

THE PROTOCOL, in the four lines it takes (RFC 1436, 1991): the client sends a
selector and CRLF; the server sends the answer and hangs up. A menu is lines
of `<type><display>\t<selector>\t<host>\t<port>` ended by a line holding one
period. A text file is its lines, ended the same way, with any line that
begins with a period doubled so the terminator cannot be forged. A binary is
its bytes and nothing else — no terminator at all, which is why the type
character on the item that LED here is the only thing that told the client
what framing to expect. Getting that wrong is how a client hangs.

SELECTORS, and what each one is FOR:

  (empty) or /       the root menu: one item of every type worth having
  /menu/plain        an ordinary menu                    the happy path
  /menu/deep/N       a menu linking to /menu/deep/N+1    the back stack
  /menu/types        every RFC 1436 type plus the strays the world added
  (the root's three `h` items)  a URL, an https URL, and an `h` that is a
                     gopher-served HTML FILE — type h's original meaning,
                     which predates the URL: convention and is still served
  /menu/hostile      ESC, CR and NUL inside display names and selectors
  /menu/malformed    too few tabs, empty host, a port that is not a number
  /menu/prose        120 `i` lines and NO links: the arrows must scroll it,
                     because there is no selection to walk
  /menu/longline     one item line far past any sane cap
  /menu/nodot        a menu the server ends by hanging up
  /text/hello        a small text file                   the happy path
  /text/big          200 lines, seeded                   the scroller
  /text/dotted       lines that begin with a period      dot-unstuffing
  /text/nodot        a text file ended by the close      the missing terminator
  /text/tabs         tabs and a very long line           rendering, not parsing
  /text/hostile      ESC, CR, NUL and OSC inside a TEXT FILE, which nothing
                     parses — the escaping a menu gets at the parse has to
                     happen at the PRINT for this one, or a page owns the screen
  /search            type 7: expects `selector<TAB>query`
  /bin/blob          64 KiB, seeded, no terminator       binary framing
  /bin/image.png     a real PNG                          the save path
  /error             what a server says about a bad selector
  /slow              a menu dribbled out a byte at a time
  /stall             half a menu, then 45 seconds of silence
  /cut               a menu cut in the middle of an item line
  (a root item)      a link to a dead port: the client must survive it

Anything else answers the way a real server does when it does not know a
selector: a type-3 error item, then the terminator.
"""

import random
import socketserver
import sys
import time
import zlib

USAGE = "python3 gophertestd.py [--port N] [--dump DIR]"

# How much of a selector line this server will read before refusing. A
# selector is a path, not a payload; anything past this is somebody probing.
MAX_SELECTOR = 4096

# Filled in from the command line: the host and port this server tells
# clients to come back to. A menu item carries its OWN host and port (that is
# gopher's whole routing model — there are no relative links), so a server
# that names the wrong one serves menus nobody can follow.
ADVERTISE_HOST = "10.0.2.2"
ADVERTISE_PORT = 7070

DUMP_DIR = None


def visible(text):
    r"""`text` with every byte a terminal would OBEY spelled as an escape.

    The same rule httptestd.py and tlsproxy.py keep, for the same reason: this
    port is open to the room, and a selector off the LAN is logged. A client
    that sends `\033[2J` as its selector must not clear the operator's screen
    by being logged."""
    out = []
    for ch in text:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif code < 0x20 or 0x7F <= code < 0xA0:
            out.append(f"\\x{code:02x}")
        else:
            out.append(ch)
    return "".join(out)


def item(kind, display, selector, host=None, port=None):
    """One menu line. The type character is GLUED to the display name — no
    separator, no space — which is the single most misparsed thing in this
    protocol and the reason a fixture server should emit it by hand once."""
    host = ADVERTISE_HOST if host is None else host
    port = ADVERTISE_PORT if port is None else port
    return f"{kind}{display}\t{selector}\t{host}\t{port}\r\n".encode("latin-1")


def info(display):
    """An `i` line: text in a menu that is not a link. Not in RFC 1436 at all
    — the world invented it because a menu was the only place a server could
    say anything — and universal enough by now that a client which cannot
    show one cannot show most of gopherspace. The selector and host of an `i`
    line are conventionally junk; this server sends the conventional junk
    (`fake`/`(NULL)`/`0`) precisely so a client is tested against it."""
    return f"i{display}\tfake\t(NULL)\t0\r\n".encode("latin-1")


TERMINATOR = b".\r\n"


def text_body(lines):
    """Lines as a gopher text file: CRLF endings, a leading period doubled on
    any line that has one, and the period-only terminator. The doubling is
    RFC 1436's transparency rule and it is the server's job — a client that
    does not undo it shows a stray period; a client that stops at a stuffed
    line truncates the file."""
    out = b""
    for line in lines:
        encoded = line.encode("latin-1")
        if encoded.startswith(b"."):
            encoded = b"." + encoded
        out += encoded + b"\r\n"
    return out + TERMINATOR


def seeded_lines(count, seed):
    rng = random.Random(seed)
    words = ["gopher", "burrow", "menu", "selector", "minnesota", "1991",
             "gophers", "tunnel", "gopherspace", "veronica", "jughead"]
    return [f"{n:04d} " + " ".join(rng.choice(words) for _ in range(8))
            for n in range(count)]


def blob_bytes(size=64 * 1024, seed=0x60F4E12):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


BLOB = blob_bytes()


def tiny_png():
    """A 4x4 greyscale PNG, built here rather than shipped as a file, so the
    fixture cannot drift from the checker that compares against it. libpng
    reads this; the point of the route is the SAVE path, not the decode."""
    def chunk(kind, payload):
        body = kind + payload
        return (len(payload).to_bytes(4, "big") + body
                + zlib.crc32(body).to_bytes(4, "big"))

    ihdr = (4).to_bytes(4, "big") + (4).to_bytes(4, "big") + bytes([8, 0, 0, 0, 0])
    raw = b"".join(bytes([0]) + bytes([16 * y + 4 * x for x in range(4)])
                   for y in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG = tiny_png()


def root_menu():
    return b"".join([
        info("gophertestd - the deterministic burrow"),
        info(""),
        info("Everything below is a fixture. Nothing here is on the internet."),
        info(""),
        item("1", "An ordinary menu", "/menu/plain"),
        item("1", "Menus that nest (the back stack)", "/menu/deep/1"),
        item("1", "One item of every type", "/menu/types"),
        item("0", "A short text file", "/text/hello"),
        item("0", "A long text file (the scroller)", "/text/big"),
        item("7", "Search this burrow", "/search"),
        item("9", "A binary blob (64 KiB)", "/bin/blob"),
        item("I", "A tiny PNG", "/bin/image.png"),
        item("h", "An HTTP link (handed off)", "URL:http://example.com/"),
        item("h", "An HTTPS link (no TLS here)", "URL:https://example.com/"),
        item("h", "An h item that is a FILE, not a URL", "/text/hello"),
        info(""),
        info("-- the awkward half --"),
        item("1", "Display names with escapes in them", "/menu/hostile"),
        item("0", "A TEXT FILE with escapes in it", "/text/hostile"),
        # A SELECTOR IS A NAME THE SERVER CHOSE, and the save prompt offers
        # its last component as the default — one keystroke from being
        # accepted. FatFs separates path components on BOTH '/' and '\\', so a
        # backslash here is a directory traversal on any FAT mount, and the
        # lifeboat partition is FAT. The prompt must offer `limine.conf` (a
        # name in this directory), never `..\limine.conf` (the boot config of
        # the partition that exists for when root is broken).
        item("9", "A binary whose name climbs out of the directory",
             "/bin/..\\limine.conf"),
        # And a last component that is not a name at all: the prompt must
        # offer NOTHING and make the person type one.
        item("9", "A binary whose name is a directory", "/bin/.."),
        # A final component longer than any sane name buffer. Copying it into
        # one TRUNCATES it, and a truncated name is a name the server never
        # suggested — two different long selectors can even agree once cut.
        # The prompt must offer nothing rather than something shortened.
        item("9", "A binary whose suggested name is enormous",
             "/bin/" + "n" * 300),
        item("1", "Item lines that are not item lines", "/menu/malformed"),
        item("1", "A menu of prose, taller than the screen", "/menu/prose"),
        item("1", "A menu with no terminator", "/menu/nodot"),
        item("0", "A text file with no terminator", "/text/nodot"),
        item("0", "Lines that begin with a period", "/text/dotted"),
        item("1", "A menu cut mid-line", "/cut"),
        item("1", "A menu that stalls forever", "/stall"),
        # A link to a port nothing is listening on. Following it must leave
        # the session and its history exactly where they were — a dead burrow
        # is a page that did not load, not a reason to lose your place.
        item("1", "A link to a burrow that is not there", "/menu/plain",
             port=ADVERTISE_PORT + 29),
        TERMINATOR,
    ])


def types_menu():
    """One item of every type this client will ever meet. The point is the
    CLIENT'S table, not the server's: a type it follows blindly is a type it
    can be lied to about, so what it does with `2` and `T` and `X` here is
    the whole test."""
    return b"".join([
        info("Every type character, and what a client owes each one"),
        info(""),
        item("0", "0  text file", "/text/hello"),
        item("1", "1  menu", "/menu/plain"),
        item("2", "2  CSO phone book (dead since the 90s)", "/cso"),
        item("3", "3  an error", "/error"),
        item("4", "4  BinHex (Macintosh)", "/bin/blob"),
        item("5", "5  DOS binary archive", "/bin/blob"),
        item("6", "6  uuencoded file", "/bin/blob"),
        item("7", "7  search server", "/search"),
        item("8", "8  telnet session", "/telnet", host="example.com", port=23),
        item("9", "9  binary file", "/bin/blob"),
        item("+", "+  redundant server", "/menu/plain"),
        item("T", "T  tn3270 session", "/tn3270", host="example.com", port=23),
        item("g", "g  GIF image", "/bin/blob"),
        item("I", "I  some other image", "/bin/image.png"),
        item("s", "s  sound", "/bin/blob"),
        item("d", "d  document (never standardised)", "/bin/blob"),
        item("h", "h  an HTML link", "URL:http://example.com/"),
        item("X", "X  a type nobody has ever defined", "/text/hello"),
        info("i  an informational line - this very one"),
        TERMINATOR,
    ])


def hostile_menu():
    r"""Display names and selectors carrying bytes a terminal would obey.

    os64's terminal obeys escape sequences, which makes a menu a stranger
    with a paintbrush: `\033[2J` in a display name clears the screen,
    `\033[H` moves the cursor somewhere the client did not put it, and a CR
    drags the rest of the line back over the start of it.

    A menu line is refused WHOLE at the parse, which is what keeps a doctored
    item from being followed and not merely from being drawn — so what this
    route proves is that none of the lines below paint anything AND that none
    of them can be entered. Its sibling is /text/hostile, where nothing
    parses and the escaping falls to the print; between them they cover both
    roads a stranger's bytes take to that screen."""
    lines = [
        b"1" + b"clear the screen: \x1b[2J" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"home the cursor: \x1b[H" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"paint it red: \x1b[41;97m" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"a carriage return: over\rwritten" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"a NUL: before\x00after" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"a bell: \x07" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"an escape in the SELECTOR" + b"\t/menu/\x1b[2Jplain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"an escape in the HOST" + b"\t/menu/plain\t" + b"\x1b[2Jhost\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
        b"1" + b"a high byte: caf\xe9" + b"\t/menu/plain\t" + ADVERTISE_HOST.encode() + b"\t" + str(ADVERTISE_PORT).encode() + b"\r\n",
    ]
    return b"".join([info("Nine lines, none of which may paint anything"), info("")]
                    + lines + [TERMINATOR])


def prose_menu(rows=120):
    """A MENU WITH NO LINKS AT ALL, and taller than any screen.

    Gopherspace is full of these — a phlog index, a header of `i` lines, an
    ASCII-art page — and they are documents that happen to be spelled as
    menus. A client that walks LINKS with the arrow keys has nothing to walk
    here, so the arrows have to scroll instead; one that pins its viewport to
    a selection that cannot move shows the first screenful and then refuses
    to move at all, which is what this route is for."""
    lines = [info(f"{n:03d}  " + "a line of prose with no link in it")
             for n in range(rows)]
    return b"".join(lines) + TERMINATOR


def malformed_menu():
    """Item lines that are not item lines. A client that indexes blindly into
    the tab-split fields reads off the end of at least four of these."""
    host = ADVERTISE_HOST.encode()
    port = str(ADVERTISE_PORT).encode()
    lines = [
        b"1no tabs at all\r\n",
        b"1one tab\t/menu/plain\r\n",
        b"1two tabs\t/menu/plain\t" + host + b"\r\n",
        b"1empty host\t/menu/plain\t\t" + port + b"\r\n",
        b"1empty port\t/menu/plain\t" + host + b"\t\r\n",
        b"1port is a word\t/menu/plain\t" + host + b"\tseventy\r\n",
        b"1port is enormous\t/menu/plain\t" + host + b"\t999999\r\n",
        b"1port is negative\t/menu/plain\t" + host + b"\t-1\r\n",
        b"1four tabs\t/menu/plain\t" + host + b"\t" + port + b"\textra\r\n",
        b"\r\n",                       # an empty line in the middle of a menu
        b"1\t/menu/plain\t" + host + b"\t" + port + b"\r\n",   # empty display
        b"\t/menu/plain\t" + host + b"\t" + port + b"\r\n",    # no type at all
        b"1trailing bare LF\t/menu/plain\t" + host + b"\t" + port + b"\n",
        TERMINATOR,
    ]
    return b"".join([info("Twelve lines a naive split would trip on"), info("")] + lines)


def deep_menu(depth):
    lines = [info(f"You are {depth} menus down."), info("")]
    if depth < 12:
        lines.append(item("1", f"Deeper still ({depth + 1})", f"/menu/deep/{depth + 1}"))
    else:
        lines.append(info("This is the bottom. Left arrow all the way home."))
    lines.append(item("0", "A text file at this depth", "/text/hello"))
    lines.append(item("1", "Back to the root", "/"))
    lines.append(TERMINATOR)
    return b"".join(lines)


def search_menu(query):
    if not query:
        return b"".join([
            info("This is a type-7 item. A client sends `selector<TAB>query`."),
            info("Nothing was sent after the tab, so there is nothing to find."),
            TERMINATOR,
        ])
    return b"".join([
        info(f"Results for: {query[:200]}"),
        info(""),
        item("0", f"The first thing matching '{query[:40]}'", "/text/hello"),
        item("0", f"The second thing matching '{query[:40]}'", "/text/big"),
        item("1", "A menu of matches", "/menu/plain"),
        TERMINATOR,
    ])


def plain_menu():
    return b"".join([
        info("An ordinary menu, of the kind most of gopherspace is."),
        info(""),
        item("0", "The first file", "/text/hello"),
        item("0", "The second file", "/text/big"),
        item("1", "A submenu", "/menu/deep/1"),
        item("1", "Back to the root", "/"),
        TERMINATOR,
    ])


HELLO_LINES = [
    "hello from the host, over gopher.",
    "",
    "Gopher is from the University of Minnesota, 1991 - Mark McCahill's team,",
    "named for the Golden Gophers and for 'go fer', which is what it does.",
    "It lost to the web in 1993, the year Minnesota announced a licence fee.",
    "",
    "This file arrived as lines ending CRLF, terminated by a lone period.",
]

DOTTED_LINES = [
    "The next lines begin with a period. The server doubles them; a client",
    "that does not undo the doubling shows the extra one, and a client that",
    "stops at the first of them truncates this file to three lines.",
    ".",
    "..",
    ". a period, a space, and some words",
    "...and an ellipsis, which is the one that looks like prose",
    "Ordinary again.",
]

TAB_LINES = [
    "name\tvalue\tnotes",
    "port\t70\tthe well-known one",
    "a very long line follows, of the kind that has to wrap or be cut: "
    + "burrow " * 40,
    "done.",
]

# LATIN-1, NOT UTF-8. `text_body` encodes latin-1, so a character outside it
# is an encode error and the route serves nothing at all — which is what an
# em-dash in this list did on its first run. High bytes are fine and wanted
# (the `café` in /menu/hostile is deliberate); it is the punctuation this
# project writes in prose that has no latin-1 spelling, and no os64 glyph
# either (DEBTS: the terminal does not speak UTF-8).
HOSTILE_TEXT_LINES = [
    "A TEXT FILE IS ALSO A STRANGER'S BYTES, and it reaches a screen by a",
    "different road than a menu does: nothing PARSES it, because it is",
    "somebody's document and refusing it for a stray byte would be refusing",
    "the page. So the escaping has to happen where it is DRAWN.",
    "",
    "Each line below carries what a page would use to take the screen. Every",
    "one of them must appear as visible text (`^[[2J` and not a cleared",
    "screen), and the browser's own title and status bars must survive:",
    "",
    "erase display:   \x1b[2J and the rest of this line",
    "home the cursor: \x1b[H and the rest of this line",
    "paint it red:    \x1b[41;97m and the rest of this line",
    "set the paper:   \x1b]11;#ff0000\x07 and the rest of this line",
    "erase to end:    \x1b[K and the rest of this line",
    "put it far away: \x1b[99;1H and the rest of this line",
    "a carriage return: before\rafter  (the second half must not overwrite)",
    "a bell:          \x07 and the rest of this line",
    "a DEL:           \x7f and the rest of this line",
    "a NUL ends the line early:  \x00 nothing after this should be shown",
    "a tab\tis layout, not an attack, and stays a tab stop's worth of space",
    "",
    "If the screen is intact and the chrome is where it was, the guard holds.",
]


class Handler(socketserver.StreamRequestHandler):
    # A per-connection read timeout and a bounded selector read, for the
    # reason httptestd.py has both: this binds every interface, so anything
    # on the LAN can connect and then say nothing forever. `/stall` is the
    # one route that deliberately outlasts it, from the server's side.
    timeout = 30

    def handle(self):
        try:
            line = self.read_selector()
        except OSError as error:
            print(f"  !! selector never arrived: {error}", flush=True)
            return
        if line is None:
            return

        selector, _, query = line.partition("\t")
        shown = visible(selector) + (f"  [query: {visible(query)}]" if query else "")
        print(f"  -> {shown or '(root)'}", flush=True)

        body, framing = self.answer(selector, query)
        if body is None:
            return                                  # the route did its own writing
        if DUMP_DIR is not None:
            self.dump(selector, body)
        try:
            self.wfile.write(body)
        except OSError:
            print("  !! client hung up mid-answer", flush=True)
        # Fetch-and-close, which is the whole of gopher's framing: the client
        # knows the answer is complete because the connection ended.
        _ = framing

    def read_selector(self):
        """One line, CRLF or bare LF, bounded. An empty line is the root
        selector and is perfectly legal — it is how you ask for a server's
        front page."""
        data = b""
        while b"\n" not in data:
            if len(data) > MAX_SELECTOR:
                print("  !! selector too long, refused", flush=True)
                return None
            chunk = self.rfile.read(1)
            if not chunk:
                # A client that connects and closes without asking. Real
                # ones do it (a port scan); it is not an error.
                return "" if not data else data.decode("latin-1")
            data += chunk
        return data.split(b"\n", 1)[0].rstrip(b"\r").decode("latin-1")

    def dump(self, selector, body):
        import os
        name = "".join(c if c.isalnum() else "_" for c in selector) or "root"
        path = os.path.join(DUMP_DIR, name + ".bin")
        with open(path, "wb") as handle:
            handle.write(body)

    def answer(self, selector, query):
        route = selector.rstrip("/") or "/"

        if route == "/":
            return root_menu(), "menu"
        if route == "/menu/plain":
            return plain_menu(), "menu"
        if route == "/menu/types":
            return types_menu(), "menu"
        if route == "/menu/hostile":
            return hostile_menu(), "menu"
        if route == "/menu/malformed":
            return malformed_menu(), "menu"
        if route == "/menu/prose":
            return prose_menu(), "menu"
        if route.startswith("/menu/deep/"):
            try:
                depth = int(route.rsplit("/", 1)[1])
            except ValueError:
                depth = 1
            return deep_menu(max(1, min(depth, 12))), "menu"
        if route == "/menu/longline":
            # One display name of 40000 characters. A client with a line cap
            # must refuse or truncate it; a client without one grows a buffer
            # for as long as a stranger keeps typing.
            return (item("1", "x" * 40000, "/menu/plain") + TERMINATOR), "menu"
        if route == "/menu/nodot":
            return plain_menu()[: -len(TERMINATOR)], "menu"

        if route == "/text/hello":
            return text_body(HELLO_LINES), "text"
        if route == "/text/big":
            return text_body(seeded_lines(200, 0x1991)), "text"
        if route == "/text/dotted":
            return text_body(DOTTED_LINES), "text"
        if route == "/text/tabs":
            return text_body(TAB_LINES), "text"
        if route == "/text/hostile":
            return text_body(HOSTILE_TEXT_LINES), "text"
        if route == "/text/nodot":
            return text_body(HELLO_LINES)[: -len(TERMINATOR)], "text"

        if route == "/search":
            return search_menu(query), "menu"

        if route == "/bin/blob":
            return BLOB, "binary"
        if route == "/bin/image.png":
            return PNG, "binary"

        if route == "/error":
            return (item("3", "That selector means nothing here", "") + TERMINATOR), "menu"

        if route == "/slow":
            self.dribble(plain_menu())
            return None, "menu"
        if route == "/stall":
            self.stall(plain_menu())
            return None, "menu"
        if route == "/cut":
            body = plain_menu()
            self.wfile.write(body[: len(body) // 2])
            return None, "menu"

        return (item("3", f"No such selector: {visible(selector)[:120]}", "")
                + TERMINATOR), "menu"

    def dribble(self, body, pause=0.01):
        try:
            for index in range(len(body)):
                self.wfile.write(body[index:index + 1])
                self.wfile.flush()
                time.sleep(pause)
        except OSError:
            print("  !! client hung up mid-dribble", flush=True)

    def stall(self, body, seconds=45):
        try:
            self.wfile.write(body[: len(body) // 2])
            self.wfile.flush()
        except OSError:
            return
        time.sleep(seconds)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main(argv):
    global ADVERTISE_HOST, ADVERTISE_PORT, DUMP_DIR
    port = 7070
    index = 1
    while index < len(argv):
        arg = argv[index]
        if arg == "--port" and index + 1 < len(argv):
            port = int(argv[index + 1]); index += 2
        elif arg == "--host" and index + 1 < len(argv):
            ADVERTISE_HOST = argv[index + 1]; index += 2
        elif arg == "--dump" and index + 1 < len(argv):
            DUMP_DIR = argv[index + 1]; index += 2
        elif arg.isdigit():
            port = int(arg); index += 1
        else:
            print(USAGE, file=sys.stderr)
            return 2
    ADVERTISE_PORT = port

    if DUMP_DIR is not None:
        import os
        os.makedirs(DUMP_DIR, exist_ok=True)

    with Server(("", port), Handler) as server:
        print(f"gophertestd on port {port}, "
              f"advertising {ADVERTISE_HOST}:{ADVERTISE_PORT}", flush=True)
        print(f"  in the guest:  gopher gopher://{ADVERTISE_HOST}:{port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
