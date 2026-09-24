#!/usr/bin/env python3
"""Drive os64's vncd the way a VNC viewer would (REMOTE.md section 5).

A pure-Python RFB 3.8 client, pointed at vncd through the tunnel a person
would use: host `ssh -N -L <port>:localhost:5900` into the guest. It checks
the handshake, a full Raw frame against QEMU's own screendump, an
incremental update after a pointer move, a 16-bit pixel format, a text
terminal taking the screen (the dimmed, bannered frame), and typing a
command there that a separate `ssh` exec then confirms ran.

    tools/vncd_probe.py --port 45900 --monitor 55572 \\
        --ssh 'ssh -p 20572 -i KEY -o UserKnownHostsFile=KH os64@127.0.0.1'

--monitor is the QEMU monitor port, for the screendump comparison.
"""
import argparse
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def recv_exact(sock, n):
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise EOFError('vncd closed the connection')
        data.extend(chunk)
    return bytes(data)


class Viewer:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=30)
        version = recv_exact(self.sock, 12)
        assert version == b'RFB 003.008\n', version
        self.sock.sendall(b'RFB 003.008\n')
        count = recv_exact(self.sock, 1)[0]
        if count == 0:
            reason = recv_exact(self.sock, struct.unpack('>I', recv_exact(self.sock, 4))[0])
            raise RuntimeError('vncd refused: ' + reason.decode())
        types = recv_exact(self.sock, count)
        assert types == b'\x01', types           # None, and only None
        self.sock.sendall(b'\x01')
        assert recv_exact(self.sock, 4) == b'\0\0\0\0'
        self.sock.sendall(b'\x01')               # shared
        w, h = struct.unpack('>HH', recv_exact(self.sock, 4))
        pf = recv_exact(self.sock, 16)
        name = recv_exact(self.sock, struct.unpack('>I', recv_exact(self.sock, 4))[0]).decode()
        self.w, self.h, self.name = w, h, name
        assert pf[:4] == bytes([32, 24, 0, 1]), pf
        self.bpp = 32
        self.fmt = (255, 255, 255, 16, 8, 0)
        self.fb = bytearray(w * h * 4)           # as 32bpp little-endian XRGB
        self.zin = zlib.decompressobj()          # ZRLE: one zlib stream for the connection
        self.wire = 0                            # encoded pixel bytes received
        self.encodings([0])

    def encodings(self, codes):
        self.sock.sendall(struct.pack('>BBH', 2, 0, len(codes)) + b''.join(struct.pack('>i', c) for c in codes))

    def set_format(self, bpp, rmax, gmax, bmax, rshift, gshift, bshift):
        pf = struct.pack('>BBBBHHHBBB3x', bpp, 16 if bpp == 16 else 24, 0, 1, rmax, gmax, bmax,
                         rshift, gshift, bshift)
        self.sock.sendall(b'\0\0\0\0' + pf)
        self.bpp, self.fmt = bpp, (rmax, gmax, bmax, rshift, gshift, bshift)

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.sock.sendall(struct.pack('>BBHHHH', 3, 1 if incremental else 0, x, y,
                                      self.w if w is None else w, self.h if h is None else h))

    def update(self):
        """Read one FramebufferUpdate; returns its rectangles."""
        msg = recv_exact(self.sock, 4)
        assert msg[0] == 0, msg
        rects = []
        for _ in range(struct.unpack('>H', msg[2:])[0]):
            x, y, w, h, enc = struct.unpack('>HHHHi', recv_exact(self.sock, 12))
            assert enc in (0, 16), enc
            if enc == 16:
                zlen = struct.unpack('>I', recv_exact(self.sock, 4))[0]
                self.wire += zlen
                data = self.zrle(w, h, self.zin.decompress(recv_exact(self.sock, zlen)))
            else:
                data = recv_exact(self.sock, w * h * self.bpp // 8)
                self.wire += len(data)
            if self.bpp == 32:
                for row in range(h):
                    start = ((y + row) * self.w + x) * 4
                    self.fb[start:start + w * 4] = data[row * w * 4:(row + 1) * w * 4]
            rects.append((x, y, w, h, data))
        return rects

    def zrle(self, w, h, tiles):
        """ZRLE tiles (RFC 6143 section 7.7.6) back to this format's PIXELs."""
        pb = self.bpp // 8
        cb = 3 if self.bpp == 32 else pb        # vncd's 32 bpp formats keep colour in 3 bytes
        p = 0
        def cpixel():
            nonlocal p
            v = tiles[p:p + cb] + (b'\0' if cb == 3 else b'')
            p += cb
            return bytes(v)
        def run():
            nonlocal p
            total = 1
            while True:
                b = tiles[p]; p += 1; total += b
                if b != 255:
                    return total
        out = bytearray(w * h * pb)
        for ty in range(0, h, 64):
            for tx in range(0, w, 64):
                tw, th = min(64, w - tx), min(64, h - ty)
                sub = tiles[p]; p += 1
                vals = []
                if sub == 0:
                    vals = [cpixel() for _ in range(tw * th)]
                elif sub == 1:
                    vals = [cpixel()] * (tw * th)
                elif 2 <= sub <= 16:
                    pal = [cpixel() for _ in range(sub)]
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    for _ in range(th):
                        n = (tw * bits + 7) // 8
                        row = tiles[p:p + n]; p += n
                        for x in range(tw):
                            b = x * bits
                            vals.append(pal[(row[b // 8] >> (8 - bits - b % 8)) & ((1 << bits) - 1)])
                elif sub == 128:
                    while len(vals) < tw * th:
                        c = cpixel(); vals += [c] * run()
                elif sub >= 130:
                    pal = [cpixel() for _ in range(sub - 128)]
                    while len(vals) < tw * th:
                        b = tiles[p]; p += 1
                        vals += [pal[b & 127]] * (run() if b & 128 else 1)
                else:
                    raise AssertionError(f'subencoding {sub}')
                for y in range(th):
                    for x in range(tw):
                        at = ((ty + y) * w + tx + x) * pb
                        out[at:at + pb] = vals[y * tw + x]
        assert p == len(tiles), (p, len(tiles))
        return bytes(out)

    def key(self, keysym, down):
        self.sock.sendall(struct.pack('>BBxxI', 4, 1 if down else 0, keysym))

    def tap(self, *keysyms):
        for k in keysyms:
            self.key(k, True)
        for k in reversed(keysyms):
            self.key(k, False)

    def type(self, text):
        for c in text:
            self.tap(0xFF0D if c == '\n' else ord(c))
            time.sleep(0.02)

    def pointer(self, x, y, mask=0):
        self.sock.sendall(struct.pack('>BBHH', 5, mask, x, y))


def screendump(monitor):
    with tempfile.NamedTemporaryFile(suffix='.ppm', delete=False) as f:
        path = f.name
    s = socket.create_connection(('127.0.0.1', monitor))
    s.sendall(f'screendump {path}\n'.encode())
    time.sleep(1.5)
    s.close()
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    assert parts[0] == b'P6', parts[0]
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--monitor', type=int, required=True)
    ap.add_argument('--ssh', required=True, help='the ssh command line, without a remote command')
    args = ap.parse_args()

    v = Viewer(args.port)
    print(f'vncd: RFB 3.8, security None, "{v.name}" {v.w}x{v.h} PASS', flush=True)

    # 1. A full frame, against the screen QEMU itself shows.
    v.request(False)
    rects = v.update()
    covered = sum(w * h for _, _, w, h, _ in rects)
    assert covered >= v.w * v.h, (covered, v.w * v.h)
    dw, dh, rgb = screendump(args.monitor)
    assert (dw, dh) == (v.w, v.h), (dw, dh)
    mismatched = sum(1 for i in range(v.w * v.h)
                     if (v.fb[i * 4 + 2], v.fb[i * 4 + 1], v.fb[i * 4]) != tuple(rgb[i * 3:i * 3 + 3]))
    assert mismatched == 0, f'{mismatched} pixels differ from the screendump'
    print(f'vncd: a full Raw frame equals QEMU\'s screendump, all {v.w * v.h} pixels PASS', flush=True)

    # 2. An incremental update after the pointer moves: small, near it.
    v.pointer(100, 100)
    v.request(True)
    rects = v.update()
    assert rects and all(w * h < v.w * v.h // 4 for _, _, w, h, _ in rects), [(x, y, w, h) for x, y, w, h, _ in rects]
    print(f'vncd: an incremental update after a pointer move: {[(x, y, w, h) for x, y, w, h, _ in rects]} PASS', flush=True)

    # 3. RGB565: every pixel is the 32-bit frame quantized.
    v.set_format(16, 31, 63, 31, 11, 5, 0)
    v.request(False, 0, 0, v.w, 64)
    rects = v.update()
    bad = 0
    for x, y, w, h, data in rects:
        for i in range(w * h):
            px = struct.unpack_from('<H', data, i * 2)[0]
            j = ((y + i // w) * v.w + (x + i % w)) * 4
            r, g, b = v.fb[j + 2], v.fb[j + 1], v.fb[j]
            if px != (((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)):
                bad += 1
    assert rects and bad == 0, bad
    v.set_format(32, 255, 255, 255, 16, 8, 0)
    print('vncd: SetPixelFormat to RGB565 matches the 32-bit frame PASS', flush=True)

    # 3b. The same, under ZRLE: the viewer prefers it, and every frame from
    # here on (typing and terminals included) arrives as ZRLE.
    v.encodings([16, 0])
    raw_frame = bytes(v.fb)
    v.fb = bytearray(len(v.fb))
    v.wire = 0
    v.request(False)
    v.update()
    assert bytes(v.fb) == raw_frame, 'the ZRLE frame differs from the Raw one'
    print(f'vncd: a full ZRLE frame equals the Raw frame, in {v.wire} bytes against {len(raw_frame)} PASS', flush=True)
    v.set_format(16, 31, 63, 31, 11, 5, 0)
    v.request(False, 0, 0, v.w, 64)
    rects = v.update()
    bad = 0
    for x, y, w, h, data in rects:
        for i in range(w * h):
            px = struct.unpack_from('<H', data, i * 2)[0]
            j = ((y + i // w) * v.w + (x + i % w)) * 4
            r, g, b = v.fb[j + 2], v.fb[j + 1], v.fb[j]
            if px != (((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)):
                bad += 1
    assert rects and bad == 0, bad
    v.set_format(32, 255, 255, 255, 16, 8, 0)
    print('vncd: ZRLE in RGB565 matches the 32-bit frame PASS', flush=True)

    # 4. Ctrl+Alt+F1 on the glass keyboard: a text terminal takes the
    # screen, and the frame comes back dimmed under the banner.
    v.tap(0xFFE3, 0xFFE9, 0xFFBE)
    time.sleep(0.8)
    v.request(False)
    v.update()
    banner = v.fb[(5 * v.w + v.w - 5) * 4:(5 * v.w + v.w - 5) * 4 + 3]
    assert tuple(banner) == (0x60, 0x28, 0x20), tuple(banner)     # the notice's blue, B G R
    print('vncd: Ctrl+Alt+F1 put a text terminal on the screen; the frame is bannered PASS', flush=True)

    # 5. Type a command into the shell there; ssh confirms it ran.
    marker = f'vncd_typed_{int(time.time())}'
    v.type(f'echo typed > /home/{marker}\n')
    time.sleep(1.5)
    out = subprocess.run(shlex.split(args.ssh) + [f'cat /home/{marker}'], capture_output=True, timeout=60)
    assert out.stdout == b'typed\n', out
    print('vncd: a command typed through the viewer ran in the shell (ssh read its file) PASS', flush=True)

    # 6. Alt+F8 brings the desktop back.
    v.tap(0xFFE9, 0xFFC5)
    time.sleep(0.8)
    v.request(False)
    v.update()
    dw, dh, rgb = screendump(args.monitor)
    corner = tuple(rgb[(5 * v.w + v.w - 5) * 3:(5 * v.w + v.w - 5) * 3 + 3])
    ours = (v.fb[(5 * v.w + v.w - 5) * 4 + 2], v.fb[(5 * v.w + v.w - 5) * 4 + 1], v.fb[(5 * v.w + v.w - 5) * 4])
    assert corner == ours and ours != (0x20, 0x28, 0x60), (corner, ours)
    print('vncd: Alt+F8 brought the desktop back to the screen PASS', flush=True)
    v.sock.close()


if __name__ == '__main__':
    main()
