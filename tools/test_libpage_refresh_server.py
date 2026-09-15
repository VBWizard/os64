#!/usr/bin/env python3
"""Manual QEMU/wend refresh fixtures. See LIBPAGE_REVIEW.md for assertions."""
import argparse
import http.server
import json
import pathlib


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        with self.server.access_log.open('a') as log:
            log.write(json.dumps(self.path) + '\n')
        if self.path == '/fragment':
            body = '<meta http-equiv=refresh content="0;url=#target"><h1>FRAGMENT TOP</h1>'
            body += ''.join(f'<p>Filler line {i}</p>' for i in range(80))
            body += '<h1 id=target>FRAGMENT TARGET REACHED</h1><p>End of fragment fixture</p>'
        elif self.path == '/long-refresh':
            body = '<meta http-equiv=refresh content="0;url=/long-target#' + 'x'*1100 + '">'
        elif self.path == '/long-target':
            body = ('<h1>BEFORE LONG TARGET</h1>' + '<p>filler</p>'*80 +
                    '<h2 id="' + 'x'*1100 + '">LONG FRAGMENT TARGET</h2>' + '<p>after</p>'*40)
        elif self.path == '/precedence':
            body = ('<meta http-equiv=refresh content="0;url=#x">'
                    '<a name=x href=/wrong>WRONG LEGACY TARGET</a>' + '<p>filler</p>'*80 +
                    '<div>Before nested target<h2 id=x>ID PRECEDENCE TARGET</h2></div><a href=#y>named target</a>' +
                    '<p>middle</p>'*40 + '<a name=y href=/else>NAMED LINK TARGET</a>' +
                    '<p>after</p>'*40)
        elif self.path == '/long-link':
            body = '<a href="/long-target#' + 'x'*1100 + '">LONG LINK</a>'
        elif self.path == '/reference-edges':
            body = ('<meta http-equiv=refresh content="0;url=#reading">'
                    '<p id=foo>WRONG PREFIX TARGET</p><area id=area>' + '<p>before</p>'*80 +
                    '<h2 id=reading>READING POSITION</h2>'
                    '<p><a href=#area>Invisible target</a></p>'
                    '<p><a href=#%00>NUL top</a></p>'
                    '<p><a href=#foo%00bar>NUL prefix</a></p>' + '<p>after</p>'*40)
        elif self.path == '/fail-refresh':
            body = ('<meta http-equiv=refresh content="0;url=/failure">'
                    '<h1>FAILED REFRESH SOURCE</h1><p>Keys must not repeat the failed fetch.</p>')
        elif self.path == '/failure':
            # An HTTP error page is still a successful navigation. Closing
            # before the status line exercises load failure and retained state.
            self.connection.close()
            return
        elif self.path in ('/loop-a', '/loop-b'):
            target = '/loop-b' if self.path == '/loop-a' else '/loop-a'
            body = (f'<meta http-equiv=refresh content="0;url={target}">'
                    '<h1>BOUNDED REFRESH CYCLE</h1>')
        else:
            self.send_error(404)
            return
        data = body.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8769)
    parser.add_argument('--log', type=pathlib.Path, default=pathlib.Path('/tmp/libpage-refresh.log'))
    args = parser.parse_args()
    with http.server.HTTPServer(('127.0.0.1', args.port), Handler) as server:
        server.access_log = args.log
        print(f'wend http://10.0.2.2:{args.port}/fragment (QEMU user networking)', flush=True)
        server.serve_forever()


if __name__ == '__main__':
    main()
