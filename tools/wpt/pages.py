#!/usr/bin/env python3
"""The WPT runner's pages, from WPT's own MANIFEST.json.

    tools/wpt/pages.py <MANIFEST.json> <dir>

WPT's manifest is what knows which files are tests, of which type, and
which window, worker and variant pages each .any.js and .window.js makes
(the suite's maintainers say to enumerate tests through it, not by file
name). This turns its testharness entries under <dir> into the lines
tools/test_yonder_wpt_host.c reads with --pages, one page per line:

    run <tab> page <tab> source <tab> kind <tab> timeout

`page` is the test's address relative to <dir> (its variant's query
included), `source` the file it comes from relative to the checkout,
`kind` html, any or window, and `timeout` long or normal. A page the runner
cannot load in a window is a `skip <tab> page <tab> reason` line instead: a
worker or shadow-realm variant (there are no workers), and an XML document
(.xhtml, .xht, .svg, .xml: yonder parses HTML). An entry that cannot be
written as a line, or that names a file outside <dir> (a tab, a newline or
a carriage return in it; absolute; a `..` in its path), stops it with
nothing written and a nonzero exit, since the manifest is input it did not
make. Standard library only, and safe under python3 -I: it reads the files
it is handed and writes stdout.
"""
import json
import sys

XML = ('.xhtml', '.xht', '.svg', '.xml')


def entries(node, path):
    for name, value in sorted(node.items()):
        at = f'{path}/{name}' if path else name
        if isinstance(value, dict):
            yield from entries(value, at)
        else:
            yield at, value


def refuse(what, name):
    """Why `name` cannot be a line's field, or a path inside <dir>; None."""
    if any(c in name for c in '\t\n\r'):
        return f'{what} {name!r} holds a tab, a newline or a carriage return'
    if name.startswith('/') or '..' in name.split('?', 1)[0].split('/'):
        return f'{what} {name!r} is outside the directory'
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: pages.py <MANIFEST.json> <dir>')
    with open(sys.argv[1], encoding='utf-8') as f:
        manifest = json.load(f)
    if manifest.get('version') != 9:
        sys.exit(f"pages.py: MANIFEST.json is version {manifest.get('version')}, this reads 9")
    directory = sys.argv[2].strip('/')
    why = refuse('the directory', directory) if directory else 'the directory is empty'
    if why:
        sys.exit(f'pages.py: {why}')
    node = manifest['items'].get('testharness', {})
    for part in directory.split('/'):
        node = node.get(part, {})
    prefix = directory + '/'
    lines = []
    for source, value in entries(node, directory):
        why = refuse('the source', source)
        if why is None and not source.startswith(prefix):
            why = f'the source {source!r} is outside the directory'
        if why:
            sys.exit(f'pages.py: {why}')
        for url, extras in value[1:]:
            url = (url or '/' + source).lstrip('/')
            if not url.startswith(prefix):
                continue
            page = url[len(prefix):]
            why = refuse('the page', page)
            if why:
                sys.exit(f'pages.py: {why}')
            path = page.split('?', 1)[0]
            if path.endswith(XML):
                lines.append(f'skip\t{page}\tan XML document')
            elif source.endswith('.js') and not path.endswith(('.any.html', '.window.html')):
                lines.append(f'skip\t{page}\ta worker or shadow-realm variant')
            else:
                kind = 'any' if source.endswith('.any.js') else 'window' if source.endswith('.js') else 'html'
                timeout = 'long' if extras.get('timeout') == 'long' else 'normal'
                lines.append(f'run\t{page}\t{source}\t{kind}\t{timeout}')
    for line in lines:
        print(line)


main()
