#!/usr/bin/env python3
"""telnetd_probe — drive os64's telnetd from the host, the way the pin's
verification did (handle.c § The pin; SERVERS.md § Verification).

    python3 tools/telnetd_probe.py <port> session <n>
    python3 tools/telnetd_probe.py <port> dontecho

`session n` runs n logins in a row: connect, negotiate, type `ls /`, read the
answer, then DROP the socket with husk still seated. That is the teardown
Codex #101 rd4 described — the session child returns from main while its
outbound thread is parked in read(master) — and before the pin it was a
ring-0 use-after-free. Watch the guest for a panic, then `ps` (no zombies, no
leftover sessions) and `cat /sys/net/tcp` (accepted == reaped).

`dontecho` answers the server's WILL ECHO with DONT ECHO, the way a line-mode
client does, and prints what comes back: the server's one-line explanation
and then EOF (Codex #101 rd5 — a session that cannot honour the refusal ends
instead of showing every keystroke twice).

Port 2323 is the usual QEMU forward (`hostfwd=tcp::2323-:23`); on the P5 it
is 23 on its own address.
"""
import socket
import sys
import time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
O_ECHO, O_SGA, O_NAWS = 1, 3, 31


def negotiate(sock, data, refuse_echo=False):
    """Answer the server's options the way a plain client would (SGA yes,
    ECHO yes unless refuse_echo, everything else no); return the data bytes."""
    reply, text, i = bytearray(), bytearray(), 0
    while i < len(data):
        c = data[i]
        if c == IAC and i + 1 < len(data):
            n = data[i + 1]
            if n in (WILL, WONT, DO, DONT) and i + 2 < len(data):
                opt = data[i + 2]
                if n == WILL:
                    if opt == O_ECHO and refuse_echo:
                        reply += bytes([IAC, DONT, opt])
                    else:
                        reply += bytes([IAC, DO if opt in (O_ECHO, O_SGA) else DONT, opt])
                elif n == DO:
                    reply += bytes([IAC, WILL if opt == O_SGA else WONT, opt])
                i += 3
                continue
            if n == SB:
                end = data.find(bytes([IAC, SE]), i)
                i = len(data) if end < 0 else end + 2
                continue
            if n == IAC:
                text.append(255)
            i += 2
            continue
        text.append(c)
        i += 1
    if reply:
        sock.sendall(bytes(reply))
    return bytes(text)


def read_for(sock, secs, refuse_echo=False):
    got = bytearray()
    end = time.time() + secs
    sock.settimeout(0.3)
    while time.time() < end:
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            got += b"<EOF>"
            break
        got += negotiate(sock, chunk, refuse_echo)
    return bytes(got)


def show(label, data):
    print(f"--- {label} ({len(data)} bytes) ---")
    print(data.decode("latin-1").replace("\r", "\\r"))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    port = int(sys.argv[1])
    mode = sys.argv[2]
    if mode == "session":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        for k in range(n):
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            banner = read_for(s, 2.5)
            s.sendall(b"ls /\r\n")
            answer = read_for(s, 2.5)
            show(f"session {k+1} banner", banner)
            show(f"session {k+1} ls", answer)
            s.close()   # no goodbye: husk is still seated, the bridge is parked
            time.sleep(1.0)
        print("sessions: done")
    elif mode == "dontecho":
        s = socket.create_connection(("127.0.0.1", port), timeout=10)
        show("dontecho", read_for(s, 4.0, refuse_echo=True))
        s.close()
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
