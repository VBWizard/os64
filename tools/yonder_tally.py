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
`.html` and `.d/` beside its record are not records. Every record is read
through tools/yonder_records.py, the one reader of page files, which
judges it whole and says its State. A CENSUS IS NOT A DIFF: it counts
every page yonder recorded, a record marked unjudged included (lines
lost, or settings changed during its load: what it kept is what the page
asked for, and partial is still asked; stderr says how many), and a replay's record only when its page
reached a verdict (stderr says how many of each other state, and which).
A path that is not there, a directory that cannot be listed, a claimed
name's placeholder that was never written (damage: a yonder stopped
between claim and write), and any `*.txt` the reader cannot judge (one
that cannot be read, is not a page file, or is not whole) are each named,
and the answer is 2, since a census of the files that could be read would
read as a census of all of them. Run it with `python3 -I`; it imports nothing but the standard
library and that reader, loaded from beside this file.
"""
import argparse
import collections
import importlib.util
import pathlib
import sys


def load_records():
    path = pathlib.Path(__file__).resolve().parent / 'yonder_records.py'
    spec = importlib.util.spec_from_file_location('yonder_records', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


records_module = load_records()
unescape = records_module.unescape
display = records_module.display
Unreadable = records_module.Unreadable


class NotJudged(Unreadable):
    """Files the reader could not judge, each named with why."""

    def __init__(self, unjudged):
        super().__init__('\n'.join(f'yonder_tally: {display(u)}' for u in unjudged))
        self.unjudged = unjudged


def read_records(paths):
    """Every record under the paths given that the census counts: a file is
    read as one, a directory for its `*.txt` files. A record marked
    unjudged (lines lost, settings changed) is counted, and their number
    said on stderr; a replay's record
    whose page did not reach a verdict is said on stderr, by state and by
    file, and not counted. Unreadable when a path is not there or a
    directory cannot be listed; NotJudged, naming every one, when any file
    could not be judged or is a placeholder never written."""
    records, unjudged, other, lost = [], [], collections.defaultdict(list), 0
    for p in map(pathlib.Path, paths):
        if not p.exists():
            raise Unreadable(f'{p}: no such file or directory')
        files = records_module.page_files(p) if p.is_dir() else [p]
        for f in files:
            rec = records_module.read(f)
            if isinstance(rec, records_module.Unjudged):
                unjudged.append(rec)
            elif rec.unwritten:
                unjudged.append(records_module.Unjudged(f, f'damage: {rec.reason}'))
            elif rec.marked:
                lost += 1
                records.append(rec)
            elif rec.state != records_module.State.VERDICT:
                other[rec.state].append(rec)
            else:
                records.append(rec)
    if unjudged:
        raise NotJudged(unjudged)
    if lost:
        print(f'yonder_tally: {lost} record(s) say they cannot be judged (lines lost, or settings changed during '
              f'the load); the lines they kept are counted', file=sys.stderr)
    if other:
        print('yonder_tally: not counted: ' + ', '.join(f'{len(other[s])} {s.name.lower()}' for s in sorted(other)),
              file=sys.stderr)
        for state in sorted(other):
            for rec in other[state]:
                print(f'  {display(rec.path)}  {state.name.lower()}{": " + display(rec.reason) if rec.reason else ""}',
                      file=sys.stderr)
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
        print(f'MISSING {display(kind)}: {len(rows)} names')
        print(f'{"files":>7} {"sites":>6} {"asks":>8}  name')
        for name, (files, asks, where) in rows[:top or None]:
            print(f'{files:>7} {len(where):>6} {asks:>8}  {display(name)}')
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
    print(f'surface {display(", ".join(names))}: {files} of {len(records)} files, '
          f'{len(sites)} of {total_sites} sites, {asks} asks')
    for name in names:
        f, s, a = each[name]
        print(f'  {display(name)}: {f} files, {len(s)} sites, {a} asks')


def failure_key(what, message):
    """What a FAILED line is ranked by: its kind (the first word of what
    failed) and its message. A picture's and a font's message begins with
    the file's own address, which would make every one unique, so for
    those (and their fetches') the reason after the last `: ` is the
    message."""
    kind = what.split(' ', 1)[0]
    if kind in ('picture', 'font', 'picture-fetch', 'font-fetch') and ': ' in message:
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
        print(f'{files:>7} {len(sites):>6} {count:>7}  {display(kind)}: {display(message)}')
        for address in where[:shown]:
            print(f'{"":>24}{display(address)}')
        if shown and len(where) > shown:
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
                    help='--failed: pages named under each failure (default 3; 0 for none)')
    args = ap.parse_args(argv)
    # A name is whatever bytes a page wrote; one the terminal cannot show
    # is printed escaped rather than ending the run.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')
    try:
        records = read_records(args.paths)
    except NotJudged as e:
        print(e, file=sys.stderr)
        return 2
    except Unreadable as e:
        print(f'yonder_tally: {display(e)}', file=sys.stderr)
        return 2
    if args.surface:
        print_surface(records, args.surface, set(args.kind))
    elif args.failed:
        print_failed(records, args.top, args.pages)
    else:
        print_missing(records, set(args.kind), args.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
