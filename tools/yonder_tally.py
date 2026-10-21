#!/usr/bin/env python3
"""The census tally over yonder's page files (MODERN_WEB.md rule 3).

A page file is one page load's record (YONDER_DIAGNOSTICS.md): line one
`address: <url>`, line two the verdict, then record lines

    MISSING <kind> <name> (<count>)
    FAILED <what>: <message> (<count>)

and plain `key: value` lines. This reads the files it is handed as data and
answers three questions:

    yonder_tally.py DIR...                  what pages asked for that yonder
                                            lacks, ranked by how many files
                                            name each, split by kind
    yonder_tally.py DIR... --surface NAME...
                                            how many files name ANY of these
                                            names: the number a slice states
    yonder_tally.py DIR... --failed         what went wrong, ranked, with the
                                            pages it went wrong on

(--surface takes every word after it, so the directories come first, or
after a `--`.)

A directory is read for its `*.txt` files, not below it: a kept page's
`.html` and `.d/` beside its record are not records. A file that does not
begin `address: ` is passed over and counted on stderr. Run it with
`python3 -I`; it imports nothing but the standard library.
"""
import argparse
import collections
import pathlib
import re
import sys

COUNT = re.compile(rb' \((\d+)\)$')
ESCAPE = re.compile(rb'\\(\\|x[0-9A-Fa-f]{2})')


def unescape(raw):
    """A field as the page wrote it: diag.c spells a backslash `\\\\`, a
    byte below 0x20 or 0x7F `\\xHH`, and a token in data with its second
    letter percent-encoded, which this reads back."""
    def one(m):
        e = m.group(1)
        return b'\\' if e == b'\\' else bytes([int(e[1:], 16)])
    raw = ESCAPE.sub(one, raw)
    raw = raw.replace(b'M%49SSING', b'MISSING').replace(b'F%41ILED', b'FAILED')
    return raw.decode('utf-8', 'replace')


def site_of(address):
    """The host an address names, lower case, without userinfo or port;
    the scheme for an address with none (`file`, `about`)."""
    m = re.match(r'^([A-Za-z][A-Za-z0-9+.-]*):(//([^/?#]*))?', address)
    if not m:
        return address
    host = m.group(3)
    if not host:
        return m.group(1).lower()
    host = host.rsplit('@', 1)[-1]
    if host.startswith('['):
        return host.split(']', 1)[0].lower() + ']'
    return host.split(':', 1)[0].lower()


class Record:
    """One page file: its address, its MISSING lines as {(kind, name):
    count} and its FAILED lines as {(what, message): count}."""

    def __init__(self, path, address):
        self.path = path
        self.address = address
        self.site = site_of(address)
        self.missing = {}
        self.failed = {}


def read_record(path):
    """The record in `path`, or None when it is not a page file."""
    data = path.read_bytes()
    lines = data.split(b'\n')
    if not lines or not lines[0].startswith(b'address: '):
        return None
    rec = Record(path, unescape(lines[0][len(b'address: '):]))
    for line in lines[2:]:
        m = COUNT.search(line)
        if line.startswith(b'MISSING ') and m:
            body = line[len(b'MISSING '):m.start()]
            kind, _, name = body.partition(b' ')
            key = (unescape(kind), unescape(name))
            rec.missing[key] = rec.missing.get(key, 0) + int(m.group(1))
        elif line.startswith(b'FAILED ') and m:
            body = line[len(b'FAILED '):m.start()]
            what, _, message = body.partition(b': ')
            key = (unescape(what), unescape(message))
            rec.failed[key] = rec.failed.get(key, 0) + int(m.group(1))
    return rec


def read_records(paths):
    """Every record under the paths given: a file is read as one, a
    directory for its `*.txt` files, in name order."""
    records, passed = [], 0
    for p in map(pathlib.Path, paths):
        files = sorted(p.glob('*.txt')) if p.is_dir() else [p]
        for f in files:
            if not f.is_file():
                continue
            rec = read_record(f)
            if rec is None:
                passed += 1
            else:
                records.append(rec)
    if passed:
        print(f'yonder_tally: {passed} file(s) passed over: not page files', file=sys.stderr)
    return records


def tally_missing(records):
    """{kind: {name: [files, asks, sites]}} over the records."""
    out = collections.defaultdict(dict)
    for rec in records:
        for (kind, name), count in rec.missing.items():
            row = out[kind].setdefault(name, [0, 0, set()])
            row[0] += 1
            row[1] += count
            row[2].add(rec.site)
    return out


def ranked(rows):
    """Rows by files, then asks, then name."""
    return sorted(rows.items(), key=lambda kv: (-kv[1][0], -kv[1][1], kv[0]))


def print_missing(records, kinds, top):
    table = tally_missing(records)
    sites = len({r.site for r in records})
    print(f'{len(records)} page files, {sites} sites')
    order = sorted(table, key=lambda k: (-sum(r[0] for r in table[k].values()), k))
    for kind in order:
        if kinds and kind not in kinds:
            continue
        rows = ranked(table[kind])
        print()
        print(f'MISSING {kind}: {len(rows)} names')
        print(f'{"files":>7} {"sites":>6} {"asks":>8}  name')
        for name, (files, asks, where) in rows[:top or None]:
            print(f'{files:>7} {len(where):>6} {asks:>8}  {name}')
        if top and len(rows) > top:
            print(f'{"":>7} {"":>6} {"":>8}  and {len(rows) - top} more')


def surface(records, names, kinds):
    """The files, sites and asks naming any of `names` (of `kinds`, when
    given), and each name's own: the before-and-after a slice states."""
    wanted = set(names)
    files, sites, asks = 0, set(), 0
    each = {n: [0, set(), 0] for n in names}
    for rec in records:
        hit = False
        for (kind, name), count in rec.missing.items():
            if name not in wanted or (kinds and kind not in kinds):
                continue
            hit = True
            asks += count
            row = each[name]
            row[2] += count
            row[1].add(rec.site)
        for name in wanted:
            if any(n == name and (not kinds or k in kinds) for k, n in rec.missing):
                each[name][0] += 1
        if hit:
            files += 1
            sites.add(rec.site)
    return files, sites, asks, each


def print_surface(records, names, kinds):
    files, sites, asks, each = surface(records, names, kinds)
    total_sites = len({r.site for r in records})
    print(f'surface {", ".join(names)}: {files} of {len(records)} files, '
          f'{len(sites)} of {total_sites} sites, {asks} asks')
    for name in names:
        f, s, a = each[name]
        print(f'  {name}: {f} files, {len(s)} sites, {a} asks')


def failure_key(what, message):
    """What a FAILED line is ranked by: its kind (the first word of what
    failed) and its message. A picture's and a font's message begins with
    the file's own address, which would make every one unique, so for
    those the reason after the last `: ` is the message."""
    kind = what.split(' ', 1)[0]
    if kind in ('picture', 'font') and ': ' in message:
        message = message.rsplit(': ', 1)[1]
    return kind, message


def print_failed(records, top, shown):
    rows = {}
    for rec in records:
        # Several lines of one file can share a key (two scripts dying the
        # same way), and the file is counted once for it.
        mine = collections.Counter()
        for (what, message), count in rec.failed.items():
            mine[failure_key(what, message)] += count
        for key, count in mine.items():
            row = rows.setdefault(key, [0, 0, set(), []])
            row[0] += 1
            row[1] += count
            row[2].add(rec.site)
            if rec.address not in row[3]:
                row[3].append(rec.address)
    order = sorted(rows.items(), key=lambda kv: (-kv[1][0], -kv[1][1], kv[0]))
    print(f'{len(records)} page files, {len(rows)} distinct failures')
    print(f'{"files":>7} {"sites":>6} {"count":>7}  kind: message')
    for (kind, message), (files, count, sites, where) in order[:top or None]:
        print(f'{files:>7} {len(sites):>6} {count:>7}  {kind}: {message}')
        for address in where[:shown]:
            print(f'{"":>24}{address}')
        if len(where) > shown:
            print(f'{"":>24}and {len(where) - shown} more pages')
    if top and len(order) > top:
        print(f'and {len(order) - top} more failures')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Rank what yonder\'s page files say pages asked for.')
    ap.add_argument('paths', nargs='+', help='page files, or directories of them')
    ap.add_argument('--surface', nargs='+', metavar='NAME',
                    help='how many files name any of these MISSING names')
    ap.add_argument('--failed', action='store_true', help='rank the FAILED lines instead')
    ap.add_argument('--kind', action='append', default=[],
                    help='only this kind (global, css-property, ...); repeatable')
    ap.add_argument('--top', type=int, default=25, help='rows per table; 0 for all (default 25)')
    ap.add_argument('--pages', type=int, default=3,
                    help='--failed: pages named under each failure (default 3)')
    args = ap.parse_args(argv)
    # A name is whatever bytes a page wrote; one the terminal cannot show
    # is printed escaped rather than ending the run.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')
    records = read_records(args.paths)
    if args.surface:
        print_surface(records, args.surface, set(args.kind))
    elif args.failed:
        print_failed(records, args.top, args.pages)
    else:
        print_missing(records, set(args.kind), args.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
