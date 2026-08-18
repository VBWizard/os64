#!/usr/bin/env python3
r"""os64serve.py - the other end of os64get. Run this on the build PC.

It is a VALET, not a daemon: you start it when you want to refresh the P5
and you stop it with Ctrl-C when you are done. It has no config file, no
install step, no service registration, and it serves exactly one directory
that you name on the command line.

    python3 os64serve.py [directory] [--port 6464]

Run it on the WINDOWS side of the build PC, not inside WSL2. That is not a
style preference: WSL2 lives behind a NAT of its own, so a listener there
is not reachable from the P5 without port-proxy incantations, and the whole
point of the P5 dialing OUT was to keep NAT out of the story entirely.

BUT THE FILES LIVE IN WSL2, which looks like a contradiction and is not.
Windows can read WSL2's filesystem directly over \\wsl$, so the listener
runs as a Windows process (reachable on the LAN) while serving the build
tree in place (no copy, no mirror, no staging directory). From a WSL2
shell, invoking the Windows interpreter does both at once:

    cd /mnt/c/temp     # a Windows-visible cwd; python.exe cannot use a Linux one
    python3.exe '\\wsl$\<distro>\home\<you>\src\os64-8125\tools\os64serve.py' \
                '\\wsl$\<distro>\home\<you>\src\os64-8125\userland\bin'

`wsl -l` names the distro. Verified 2026-08-16 serving userland/bin to the
P5 with zero files copied anywhere.

Windows will likely prompt about the firewall the first time Python binds —
say yes, or the P5's connections are dropped before this program ever sees
them. (The symptom is a client that says "timed out", not "refused".)

THE PROTOCOL (RTL8125.md), deliberately 1971-shaped:

    client -> server:   GET <name>\n
    server -> client:   OK <length-decimal> <crc32-hex8>\n  then <length> bytes
                   or:  NO <reason>\n

One connection per file. ASCII where a human might read it, binary only
where a machine must. You can drive it by hand with telnet, which matters
more than it sounds: a protocol you can type is a protocol you can debug
at 1am on a machine with no tooling.

The CRC is zlib.crc32 - CRC-32/ISO-HDLC, the same one in Ethernet frames
and PNG chunks and gzip. os64's os64_crc32 computes the identical value;
tools/test_crc32_host.c checks both against the published vectors so that
neither end has to be told which flavour the other meant.

SECURITY, stated honestly: there is none. No authentication, no encryption,
and the only access control is the one rule below - the requested name must
be a single path component with no separators and no "..", so the serving
directory is the entire namespace. That is adequate for an isolated
two-node build segment with no gateway, which is what this was designed
for, and it is NOT adequate for anything else. Do not run it on your home
LAN and then forget it is running.
"""

import argparse
import os
import socket
import stat
import sys
import zlib

CHUNK = 65536


def is_safe_name(name):
    """One path component, nothing clever.

    Rejecting traversal by BUILDING a safe name (rather than by hunting for
    bad patterns) is the only version of this that stays correct: blacklists
    of "..", "%2e%2e", backslashes and drive letters are a game you lose
    eventually. A name with no separator in it at all cannot leave the
    directory, whatever it contains.
    """
    if not name or name in (".", "..") or "\x00" in name:
        return False
    if "/" in name or "\\" in name:
        return False
    if os.path.basename(name) != name:      # catches drive letters, oddities
        return False
    return True


def read_served_file(directory, name):
    """Read one regular, non-symlink file from the serving directory.

    Open before inspecting so that the file we validate is also the file we
    read.  O_NOFOLLOW rejects a final-component symlink atomically where the
    host provides it.  The descriptor/directory-entry identity check supplies
    the same protection on hosts without O_NOFOLLOW (notably Windows) and
    catches a replacement racing the open.
    """
    path = os.path.join(directory, name)
    flags = (os.O_RDONLY
             | getattr(os, "O_BINARY", 0)
             | getattr(os, "O_CLOEXEC", 0)
             | getattr(os, "O_NOFOLLOW", 0)
             | getattr(os, "O_NONBLOCK", 0))

    try:
        fd = os.open(path, flags)
    except OSError:
        return None

    try:
        opened = os.fstat(fd)
        entry = os.stat(path, follow_symlinks=False)
        if (not stat.S_ISREG(opened.st_mode)
                or not stat.S_ISREG(entry.st_mode)
                or not os.path.samestat(opened, entry)):
            return None

        file_obj = os.fdopen(fd, "rb")
        fd = None                 # file_obj owns the descriptor from here
        with file_obj:
            return file_obj.read()
    except OSError:
        return None
    finally:
        if fd is not None:
            os.close(fd)


def serve_one(conn, addr, directory):
    conn.settimeout(30)
    try:
        # Read the request line one byte at a time. A stream has no message
        # boundaries, so reading in blocks would swallow whatever came after
        # the newline - and on a request-then-respond protocol there is
        # nothing after it yet, but a client that pipelines would break us.
        request = b""
        while not request.endswith(b"\n"):
            b = conn.recv(1)
            if not b:
                print(f"  {addr[0]}: hung up before finishing the request")
                return
            request += b
            if len(request) > 1024:
                conn.sendall(b"NO request too long\n")
                return

        line = request.decode("utf-8", "replace").strip()
        if not line.startswith("GET "):
            print(f"  {addr[0]}: not a GET: {line!r}")
            conn.sendall(b"NO expected GET <name>\n")
            return

        name = line[4:].strip()
        if not is_safe_name(name):
            print(f"  {addr[0]}: refused unsafe name {name!r}")
            conn.sendall(b"NO name must be a single path component\n")
            return

        data = read_served_file(directory, name)
        if data is None:
            print(f"  {addr[0]}: no such file or refused file: {name}")
            conn.sendall(b"NO no such file\n")
            return

        # Read it once, whole: checksum and length describe the SAME
        # bytes we are about to send. Computing the CRC in one pass and then
        # streaming the file from disk in another would leave a window where
        # an edit between the two makes the header a lie - and the client
        # would then reject a transfer that was perfectly intact, which is
        # the most confusing failure available.
        crc = zlib.crc32(data) & 0xFFFFFFFF
        header = f"OK {len(data)} {crc:08x}\n".encode("ascii")
        conn.sendall(header)
        conn.sendall(data)
        print(f"  {addr[0]}: sent {name} ({len(data)} bytes, crc {crc:08x})")

    except socket.timeout:
        print(f"  {addr[0]}: timed out")
    except (ConnectionResetError, BrokenPipeError):
        print(f"  {addr[0]}: connection reset")
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser(description="Serve files to os64get.")
    ap.add_argument("directory", nargs="?", default=".",
                    help="directory to serve (default: the current one)")
    ap.add_argument("--port", type=int, default=6464)
    ap.add_argument("--bind", default="0.0.0.0",
                    help="address to listen on (default: everything)")
    args = ap.parse_args()

    directory = os.path.realpath(args.directory)
    if not os.path.isdir(directory):
        print(f"os64serve: {directory} is not a directory", file=sys.stderr)
        return 2

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # So a Ctrl-C and an immediate restart does not hit TIME_WAIT and refuse
    # to bind - which during a debugging session is every single restart.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.bind, args.port))
    s.listen(4)

    print(f"os64serve: serving {directory} on {args.bind}:{args.port}")
    print("           Ctrl-C to stop.  (Windows may ask about the firewall - say yes.)")
    try:
        while True:
            conn, addr = s.accept()
            serve_one(conn, addr, directory)
    except KeyboardInterrupt:
        print("\nos64serve: stopped.")
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
