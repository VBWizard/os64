#!/usr/bin/env python3
"""Serve a directory (tools/pages by default) over HTTP for QEMU page tests,
answering Range requests as a web server does, which python3 -m http.server
does not.

    tools/pages_serve.py [--port 8731] [--root DIR] [--rate BYTES_PER_S] [--no-ranges] [--part BYTES]
                         [--no-length]

--rate sends every body at most that fast, so a seek in a sound can land past
what has arrived; --no-ranges answers every request whole and says nothing of
ranges, as a server that has none; --part answers a Range request with at most
that many bytes, as servers that cut ranges into pieces do; --no-length
answers whole with no Content-Length and no ranges, the body ended by closing
the connection (HTTP/1.0's way), so the size is not known until the end, as a
stream's is. Each request is
logged on stdout with its
Range header and what was answered, which is what a test reads back.
The guest reaches it at 10.0.2.2:<port>.
"""

import argparse
import http.server
import os
import re
import sys
import time

ARGS = None


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().do_GET()
        size = os.path.getsize(path)
        asked = self.headers.get('Range')
        first, last, status = 0, size - 1, 200
        if asked and not ARGS.no_ranges and not ARGS.no_length:
            m = re.fullmatch(r'bytes=(\d+)-(\d*)', asked.strip())
            if m is None or int(m.group(1)) >= size:
                self.send_response(416)
                self.send_header('Content-Range', f'bytes */{size}')
                self.send_header('Content-Length', '0')
                self.end_headers()
                print(f'GET {self.path} range={asked} -> 416', flush=True)
                return
            first = int(m.group(1))
            if m.group(2):
                last = min(int(m.group(2)), size - 1)
            if ARGS.part:
                last = min(last, first + ARGS.part - 1)
            status = 206
        self.send_response(status)
        self.send_header('Content-Type', self.guess_type(path))
        if ARGS.no_length:
            self.close_connection = True
        else:
            self.send_header('Content-Length', str(last - first + 1))
        if not ARGS.no_ranges and not ARGS.no_length:
            self.send_header('Accept-Ranges', 'bytes')
        if status == 206:
            self.send_header('Content-Range', f'bytes {first}-{last}/{size}')
        self.end_headers()
        print(f'GET {self.path} range={asked or "-"} -> {status} {first}-{last}/{size}', flush=True)
        sent, began = 0, time.monotonic()
        try:
            with open(path, 'rb') as f:
                f.seek(first)
                left = last - first + 1
                while left > 0:
                    piece = f.read(min(left, 16384))
                    if not piece:
                        break
                    self.wfile.write(piece)
                    left -= len(piece)
                    sent += len(piece)
                    if ARGS.rate:
                        ahead = sent / ARGS.rate - (time.monotonic() - began)
                        if ahead > 0:
                            time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError):
            print(f'GET {self.path} closed by the client after {sent} bytes', flush=True)
            return
        print(f'GET {self.path} sent {sent} bytes', flush=True)


def main():
    global ARGS
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int, default=8731)
    p.add_argument('--root', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages'))
    p.add_argument('--rate', type=int, default=0)
    p.add_argument('--no-ranges', action='store_true')
    p.add_argument('--part', type=int, default=0)
    p.add_argument('--no-length', action='store_true')
    ARGS = p.parse_args()
    os.chdir(ARGS.root)
    http.server.ThreadingHTTPServer.daemon_threads = True
    with http.server.ThreadingHTTPServer(('', ARGS.port), Handler) as server:
        print(f'serving {ARGS.root} on {ARGS.port}', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            return 0


if __name__ == '__main__':
    sys.exit(main())
