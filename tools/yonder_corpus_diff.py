#!/usr/bin/env python3
"""Which kept pages got worse between two replays of the corpus
(MODERN_WEB.md rule 3).

    yonder_corpus_diff.py BEFORE-DIR AFTER-DIR

Each directory holds page files by the same names, as
tools/test_yonder_replay_host.sh writes them. A page is the set of its
record lines, `MISSING <kind> <name>` and `FAILED <what>: <message>`, with
their counts left out: a count moves with timing, and a line appearing or
going is what a slice changed. A page whose set GAINED a line is a
regression and is printed with the lines it gained; a page that lost lines
is progress and is printed with those. A page the after directory does
not have is counted with the regressions, since whatever it gained cannot
be seen. Exit 1 when there is any regression, 0 otherwise.

Run it with `python3 -I`; the records are data, read by
tools/yonder_tally.py's reader, loaded from beside this file.
"""
import argparse
import importlib.util
import pathlib
import sys


def load_tally():
    path = pathlib.Path(__file__).resolve().parent / 'yonder_tally.py'
    spec = importlib.util.spec_from_file_location('yonder_tally', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tally = load_tally()


def lines_of(rec):
    """The page's record lines, counts left out."""
    out = {f'MISSING {kind} {name}' for kind, name in rec.missing}
    out |= {f'FAILED {what}: {message}' for what, message in rec.failed}
    return out


def read_dir(path):
    """{file name: record} for the page files in a directory."""
    pages = {}
    for f in sorted(pathlib.Path(path).glob('*.txt')):
        rec = tally.read_record(f)
        if rec is not None:
            pages[f.name] = rec
    return pages


def diff(before, after):
    """(regressions, progress, gone, new): the first two {name: (address,
    lines gained or lost)}, then the names only one side has."""
    regressions, progress = {}, {}
    for name in sorted(before.keys() & after.keys()):
        was, now = lines_of(before[name]), lines_of(after[name])
        if now - was:
            regressions[name] = (after[name].address, sorted(now - was))
        if was - now:
            progress[name] = (after[name].address, sorted(was - now))
    gone = sorted(before.keys() - after.keys())
    new = sorted(after.keys() - before.keys())
    return regressions, progress, gone, new


def clean(pages):
    return sum(1 for r in pages.values() if not r.missing and not r.failed)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Name every kept page whose record got worse.')
    ap.add_argument('before')
    ap.add_argument('after')
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')
    for d in (args.before, args.after):
        if not pathlib.Path(d).is_dir():
            print(f'yonder_corpus_diff: {d} is not a directory', file=sys.stderr)
            return 2
    before, after = read_dir(args.before), read_dir(args.after)
    regressions, progress, gone, new = diff(before, after)
    for title, table, sign in (('regressions', regressions, '+'), ('progress', progress, '-')):
        print(f'{title}: {len(table)} pages')
        for name, (address, lines) in table.items():
            print(f'  {name}  {address}')
            for line in lines:
                print(f'    {sign} {line}')
    if gone:
        print(f'missing from {args.after}: {len(gone)} pages')
        for name in gone:
            print(f'  {name}  {before[name].address}')
    if new:
        print(f'only in {args.after}: {len(new)} pages')
    gained = sum(len(lines) for _, lines in regressions.values())
    lost = sum(len(lines) for _, lines in progress.values())
    print(f'before: {len(before)} pages, {clean(before)} clean; after: {len(after)} pages, '
          f'{clean(after)} clean; {gained} lines gained, {lost} lines lost')
    return 1 if regressions or gone else 0


if __name__ == '__main__':
    sys.exit(main())
