#!/usr/bin/env python3
r"""ftptestd — a deterministic FTP server for driving /bin/ftp.

httptestd.py and gophertestd.py's sibling, and built for the same reason: the
answers worth testing are the ones a well-behaved server will not produce on
request. A welcome banner containing a line that looks like the end of the
banner. A passive-mode reply with prose either side of the tuple. A transfer
that closes the data connection and THEN reports failure on the control
channel. vsftpd will not do any of those for you, and those are exactly where
a client breaks.

    python3 ftptestd.py [--port N] [--root DIR]

Default port is 2121, not 21: 21 is privileged and nothing here needs root.

WHERE TO RUN IT depends on which machine is fetching, exactly as httptestd.py
explains at length: from WSL2 the QEMU guest reaches this at 10.0.2.2; the P5
needs it started on the WINDOWS side, because WSL2 sits behind a NAT of its
own and a listener inside it is not reachable from the room.

PASSIVE MODE ONLY, which is also all /bin/ftp speaks. The server opens a
listening socket per transfer, names it in a 227, and accepts one connection
on it. The address it names is the one the control connection arrived on, so
this works from the guest without being told where it is.

THE FILES, and what each one is FOR:

  /hello.txt        21 bytes of text                    the happy path
  /big.bin          1 MiB, seeded and checkable         the streamer
  /huge.bin         16 MiB of the same                  the rate, with resolution
  /empty            zero bytes                          a transfer with no data
  /slow.txt         dribbled a few bytes at a time      a client in no hurry
  /cut.bin          claims to be large, closes early    EOF is not success
  /noperm.txt       every RETR answers 550              the refusal path
  /odd name.txt     a space in the name                 the tokenizer
  /deep/            a subdirectory                      cd and relative names

THE AWKWARD ANSWERS, chosen because each one has broken a real client:

  * the greeting is MULTILINE and carries a complete-looking `500` line in
    the middle of it. Only the opening `220` repeated with a space closes a
    reply, so a client scanning for "three digits and a space" ends the
    greeting early and reads the rest of the banner as answers to commands
    it has not sent — every reply from then on one behind. See BANNER for
    why the line is a `500` and not a `220`.
  * `--pasv-style` picks how the 227 is written: `parens` (the usual),
    `bare` (no parentheses at all), `prose` (a decoy six-number run BEFORE
    the real one, in parentheses), or `broken` (no tuple at all).
  * RETR of /cut.bin sends half of what it promised, closes the data
    connection, and answers `451 Transfer aborted` on the control channel —
    the case where the bytes ended cleanly and the transfer still failed.
  * RETR of /slow.txt dribbles for half a minute. A client that walks away
    mid-transfer is owed one more reply, and this server may take a long
    while to send it — os64 absorbs data on a closed connection rather than
    resetting it, so the dribble runs to its end before the server notices.
    A client that forgets it is owed that reply reads it as the answer to
    whatever it types next.
  * STOR always succeeds and the bytes are counted, not kept, unless --root
    is writable and the name is ordinary.
"""

import argparse
import os
import socket
import threading
import time

# THE BANNER IS THE TRAP, and it is a LEGAL one. RFC 959 §4.2 lets any line
# inside a multiline reply say anything at all; only the opening code repeated
# with a SPACE closes it. So the middle line here is a complete-looking `500`
# that a client scanning for "three digits and a space" will take as the end
# of the greeting — after which every reply it reads is one behind, and the
# `230` meant for PASS is read as the answer to TYPE.
#
# What is deliberately NOT here is a middle line reading `220 something`. That
# would be the server breaking the rule rather than the client, RFC 959 tells
# servers not to write one, and no client can survive it — Python's own ftplib
# desyncs on it too, which is how this banner started and why it changed.
BANNER = [
    "220-ftptestd, the awkward server",
    "500 This line is not the end of anything",
    "220-still the banner",
    "220 Ready.",
]

# Long enough to interrupt. A transfer that finishes before a person can reach
# for Ctrl+C cannot test what happens when they do, and what happens then is
# the one part of this client with a way to go quietly wrong: replies from the
# abandoned transfer left unread, answering every later command one behind.
SLOW_TEXT = b"".join(b"line %04d arrives a few bytes at a time\r\n" % i
                     for i in range(100))
SLOW_CHUNK = 7
SLOW_PAUSE = 0.05


def seeded(n):
    """A megabyte a client can check without holding a copy of it."""
    out = bytearray(n)
    x = 0x12345678
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


FILES = {
    "/hello.txt": b"hello from ftptestd\r\n",
    "/empty": b"",
    "/odd name.txt": b"a name with a space in it\r\n",
    "/deep/inner.txt": b"one level down\r\n",
}

LISTINGS = {
    "/": [
        "-rw-r--r-- 1 ftp ftp        21 Sep  9 20:00 hello.txt",
        "-rw-r--r-- 1 ftp ftp   1048576 Sep  9 20:00 big.bin",
        "-rw-r--r-- 1 ftp ftp  16777216 Sep  9 20:00 huge.bin",
        "-rw-r--r-- 1 ftp ftp         0 Sep  9 20:00 empty",
        "-rw-r--r-- 1 ftp ftp      4100 Sep  9 20:00 slow.txt",
        "-rw-r--r-- 1 ftp ftp    524288 Sep  9 20:00 cut.bin",
        "-rw-r--r-- 1 ftp ftp        11 Sep  9 20:00 noperm.txt",
        "-rw-r--r-- 1 ftp ftp        26 Sep  9 20:00 odd name.txt",
        "drwxr-xr-x 2 ftp ftp      4096 Sep  9 20:00 deep",
    ],
    "/deep": [
        "-rw-r--r-- 1 ftp ftp        16 Sep  9 20:00 inner.txt",
    ],
}


class Session(threading.Thread):
    def __init__(self, conn, addr, args):
        super().__init__(daemon=True)
        self.conn = conn
        self.addr = addr
        self.args = args
        self.cwd = "/"
        self.rest = b""
        self.pasv = None            # the listening socket, waiting for its dial

    # ── the control channel ──────────────────────────────────────────────

    def send(self, text):
        self.conn.sendall((text + "\r\n").encode("latin-1"))
        print(f"  -> {text}")

    def readline(self):
        while b"\r\n" not in self.rest:
            chunk = self.conn.recv(4096)
            if not chunk:
                return None
            self.rest += chunk
        line, _, self.rest = self.rest.partition(b"\r\n")
        text = line.decode("latin-1")
        print(f"  <- {text}")
        return text

    # ── the data channel ─────────────────────────────────────────────────

    def do_pasv(self):
        if self.pasv is not None:
            self.pasv.close()
        self.pasv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.pasv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Bind on the interface the CONTROL connection arrived on, so a guest
        # reaching this through slirp is told an address it can reach.
        host = self.conn.getsockname()[0]
        self.pasv.bind((host, 0))
        self.pasv.listen(1)
        port = self.pasv.getsockname()[1]

        quad = ",".join(host.split("."))
        tuple_text = f"{quad},{port >> 8},{port & 0xFF}"
        style = self.args.pasv_style

        if style == "bare":
            self.send(f"227 Entering Passive Mode {tuple_text}")
        elif style == "prose":
            # A decoy run FIRST, the real one in parentheses. A client that
            # takes the first six numbers it sees dials nowhere.
            self.send(f"227 Mode 1,2,3,4,5,6 retired; use ({tuple_text})")
        elif style == "broken":
            self.send("227 Entering Passive Mode, somewhere")
        else:
            self.send(f"227 Entering Passive Mode ({tuple_text})")

    def data_accept(self):
        if self.pasv is None:
            self.send("425 Use PASV first")
            return None
        self.pasv.settimeout(20)
        try:
            data, _ = self.pasv.accept()
        except socket.timeout:
            self.send("425 No data connection arrived")
            return None
        finally:
            self.pasv.close()
            self.pasv = None
        return data

    # ── verbs ────────────────────────────────────────────────────────────

    def resolve(self, arg):
        if not arg:
            return self.cwd
        path = arg if arg.startswith("/") else self.cwd.rstrip("/") + "/" + arg
        parts = []
        for piece in path.split("/"):
            if piece in ("", "."):
                continue
            if piece == "..":
                if parts:
                    parts.pop()
                continue
            parts.append(piece)
        return "/" + "/".join(parts)

    def do_list(self, arg):
        data = self.data_accept()
        if data is None:
            return
        self.send("150 Here comes the directory listing")
        path = self.resolve(arg)
        rows = LISTINGS.get(path if path != "" else "/")
        if rows is None:
            data.close()
            self.send("550 No such directory")
            return
        for row in rows:
            data.sendall((row + "\r\n").encode("latin-1"))
        data.close()
        self.send("226 Directory send OK")

    def content_for(self, path):
        if path == "/big.bin":
            return seeded(1024 * 1024)
        if path == "/huge.bin":
            # Big enough that the transfer rate has resolution. The client's
            # own report is in tenths of a second, so a file that finishes in
            # two of them measures nothing.
            return seeded(1024 * 1024) * 16
        if path == "/slow.txt":
            return SLOW_TEXT
        if path == "/cut.bin":
            return seeded(512 * 1024)
        return FILES.get(path)

    def do_retr(self, arg):
        path = self.resolve(arg)

        # THE REFUSAL ARRIVES ON THE CONTROL CHANNEL WITH NO DATA AT ALL.
        # A client that waits on the data handle first waits forever.
        if path == "/noperm.txt":
            self.send("550 Permission denied")
            return
        body = self.content_for(path)
        if body is None:
            self.send("550 No such file")
            return

        data = self.data_accept()
        if data is None:
            return
        self.send(f"150 Opening BINARY mode data connection ({len(body)} bytes)")

        if path == "/slow.txt":
            try:
                for i in range(0, len(body), SLOW_CHUNK):
                    data.sendall(body[i:i + SLOW_CHUNK])
                    time.sleep(SLOW_PAUSE)
            except (ConnectionResetError, BrokenPipeError):
                # The client walked away mid-transfer, so the transfer's final
                # reply says so. ONE reply and not two: the `226 Abort
                # successful` that usually follows a `426` is the answer to an
                # ABOR command, and no ABOR was sent. That exactness is what
                # lets a client know precisely how many replies it is owed.
                data.close()
                self.send("426 Transfer aborted, data connection closed")
                return
            data.close()
            self.send("226 Transfer complete")
            return

        if path == "/cut.bin":
            # HALF THE FILE, THEN A CLEAN CLOSE, THEN A FAILURE. The data
            # connection ends exactly as it would on success; only the control
            # channel knows the difference.
            data.sendall(body[:len(body) // 2])
            data.close()
            self.send("451 Requested action aborted: local error in processing")
            return

        data.sendall(body)
        data.close()
        self.send("226 Transfer complete")

    def do_stor(self, arg):
        path = self.resolve(arg)
        data = self.data_accept()
        if data is None:
            return
        self.send("150 Ok to send data")

        total = 0
        blob = bytearray()
        while True:
            chunk = data.recv(65536)
            if not chunk:
                break
            total += len(chunk)
            if self.args.root:
                blob += chunk
        data.close()

        if self.args.root:
            target = os.path.join(self.args.root, os.path.basename(path))
            with open(target, "wb") as fh:
                fh.write(blob)
            print(f"  .. stored {total} bytes in {target}")
        else:
            print(f"  .. counted {total} bytes (no --root, nothing kept)")
        self.send(f"226 Transfer complete ({total} bytes)")

    # ── the loop ─────────────────────────────────────────────────────────

    def run(self):
        print(f"session from {self.addr}")
        for line in BANNER:
            self.conn.sendall((line + "\r\n").encode("latin-1"))
            print(f"  -> {line}")

        try:
            while True:
                line = self.readline()
                if line is None:
                    break
                verb, _, arg = line.partition(" ")
                verb = verb.upper()

                if verb == "USER":
                    self.send("331 Please specify the password")
                elif verb == "PASS":
                    self.send("230-Logged in")
                    self.send("230 Have fun")
                elif verb == "SYST":
                    self.send("215 UNIX Type: L8")
                elif verb == "TYPE":
                    self.send(f"200 Type set to {arg}")
                elif verb == "PWD":
                    # The quoting rule, exercised: a directory whose name
                    # contains a quote writes it twice.
                    self.send('257 "%s" is the current directory'
                              % self.cwd.replace('"', '""'))
                elif verb == "CWD":
                    path = self.resolve(arg)
                    if path in LISTINGS or path == "/":
                        self.cwd = path
                        self.send(f'250 Directory changed to {path}')
                    else:
                        self.send("550 No such directory")
                elif verb == "CDUP":
                    self.cwd = self.resolve("..")
                    self.send("250 Directory changed")
                elif verb == "PASV":
                    self.do_pasv()
                elif verb == "LIST":
                    self.do_list(arg)
                elif verb == "RETR":
                    self.do_retr(arg)
                elif verb == "STOR":
                    self.do_stor(arg)
                elif verb == "DELE":
                    self.send("250 Delete operation successful")
                elif verb == "MKD":
                    self.send('257 "%s" created' % self.resolve(arg))
                elif verb == "RMD":
                    self.send("250 Remove directory operation successful")
                elif verb == "NOOP":
                    self.send("200 NOOP ok")
                elif verb == "QUIT":
                    self.send("221 Goodbye")
                    break
                else:
                    self.send(f"500 Unknown command {verb}")
        except (ConnectionResetError, BrokenPipeError):
            print("  .. the client went away")
        finally:
            if self.pasv is not None:
                self.pasv.close()
            self.conn.close()
            print(f"session from {self.addr} ended")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=2121)
    ap.add_argument("--root", default=None,
                    help="directory uploads are written into (default: counted "
                         "and discarded)")
    ap.add_argument("--pasv-style", default="parens",
                    choices=["parens", "bare", "prose", "broken"],
                    help="how the 227 reply is written")
    args = ap.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.port))
    server.listen(8)
    print(f"ftptestd on port {args.port}, 227 style {args.pasv_style}")

    try:
        while True:
            conn, addr = server.accept()
            Session(conn, addr, args).start()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        server.close()


if __name__ == "__main__":
    main()
