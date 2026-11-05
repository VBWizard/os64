#!/usr/bin/env python3
"""Serve tools/pages/intl-capture.html and save what it sends (INTL.md § 8).

Usage: tools/intl_capture_server.py <capture file> [--port 8794] [--bind 127.0.0.1]

Open http://localhost:<port>/intl-capture.html in the browser to capture;
the page POSTs its capture to /capture, written to <capture file>, and the
server says where and keeps serving. tools/intl_capture_check.py holds
libjs to the file. Under WSL2 a browser on Windows reaches localhost here;
--bind 0.0.0.0 when it does not (that serves the LAN too, while it runs).
"""
import argparse
import functools
import http.server
import os
import sys

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages')


class Handler(http.server.SimpleHTTPRequestHandler):
    target = None

    def do_POST(self):
        if self.path != '/capture':
            self.send_error(404)
            return
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length)
        with open(self.target, 'wb') as f:
            f.write(body)
        reply = ('%s (%d bytes)' % (self.target, len(body))).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)
        sys.stderr.write('intl capture: saved %s\n' % reply.decode())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('target')
    parser.add_argument('--port', type=int, default=8794)
    parser.add_argument('--bind', default='127.0.0.1')
    args = parser.parse_args()
    Handler.target = os.path.abspath(args.target)
    handler = functools.partial(Handler, directory=PAGES)
    server = http.server.ThreadingHTTPServer((args.bind, args.port), handler)
    sys.stderr.write('intl capture: http://localhost:%d/intl-capture.html -> %s\n' % (args.port, Handler.target))
    server.serve_forever()


if __name__ == '__main__':
    main()
