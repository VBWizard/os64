#!/usr/bin/env python3
"""Which kept pages got worse between two replays of the corpus
(MODERN_WEB.md rule 3).

    yonder_corpus_diff.py BEFORE-DIR AFTER-DIR

Each directory holds page files by the same names, as
tools/test_yonder_replay_host.sh writes them. ONLY A PAGE REPLAYED TO A
VERDICT IS JUDGED: one whose record carries no `REPLAY` line. Such a page
is the set of its record lines, `MISSING <kind> <name>` and `FAILED <what>:
<message>`, with their counts left out: a count moves with timing, and a
line appearing or going is what a slice changed. Judged on both sides, a
page whose set GAINED a line is a regression, printed with the lines it
gained, and one that lost lines is progress, printed with those.

The rest each have a heading of their own, and none is progress:

- BROKEN: the after-record says the replay reached no verdict (`REPLAY`
  and a reason other than `incomplete`: the page never arrived, or asked
  for the next turn for ever), so what it shows is not the page. A
  regression when the before-record reached one; still broken otherwise.
- TO RECORD AGAIN: the after-record says `REPLAY incomplete`, the page
  asked for something the corpus never kept. Listed with what it asked
  for, as newly incomplete when the before-record was not; neither a
  regression nor progress, and never silent.
- RECOVERED: the after-record reached a verdict where the before-record
  did not; its lines are not compared with a record that was not the page.
- A page the after directory does not have is a regression, since whatever
  it gained cannot be seen.

Exit 1 when there is any regression, 2 when a directory or a page file in
it cannot be read, 0 otherwise. Run it with `python3 -I`; the records are
data, read by tools/yonder_tally.py's reader, loaded from beside this file.
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


def state_of(rec):
    """'incomplete', 'broken', or 'verdict'."""
    if rec.incomplete:
        return 'incomplete'
    return 'broken' if rec.unfinished else 'verdict'


def read_dir(path):
    """{file name: record} for the page files in a directory. Unreadable
    (tally's) when a `*.txt` there is not a file or cannot be read: a page
    left out of the comparison would read as a page that did not change."""
    pages = {}
    for f in sorted(pathlib.Path(path).glob('*.txt')):
        if not f.is_file():
            raise tally.Unreadable(f'{f}: not a file')
        try:
            rec = tally.read_record(f)
        except OSError as e:
            raise tally.Unreadable(f'{f}: {e.strerror or e}') from e
        if rec is not None:
            pages[f.name] = rec
    return pages


def diff(before, after):
    """{heading: {name: (address, lines)}} for the headings the module's
    comment names, and the names only one side has (gone, new)."""
    out = {h: {} for h in ('regressions', 'progress', 'broken', 'still broken', 'newly incomplete',
                           'to record again', 'recovered')}
    for name in sorted(before.keys() & after.keys()):
        was, now = before[name], after[name]
        b, a = state_of(was), state_of(now)
        if a == 'incomplete':
            out['to record again' if b == 'incomplete' else 'newly incomplete'][name] = (now.address, now.incomplete)
        elif a == 'broken':
            out['still broken' if b == 'broken' else 'broken'][name] = (now.address, now.unfinished)
        elif b != 'verdict':
            out['recovered'][name] = (now.address, [])
        else:
            gained, lost = lines_of(now) - lines_of(was), lines_of(was) - lines_of(now)
            if gained:
                out['regressions'][name] = (now.address, sorted(gained))
            if lost:
                out['progress'][name] = (now.address, sorted(lost))
    gone = sorted(before.keys() - after.keys())
    new = sorted(after.keys() - before.keys())
    return out, gone, new


def clean(pages):
    return sum(1 for r in pages.values() if state_of(r) == 'verdict' and not r.missing and not r.failed)


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
    try:
        before, after = read_dir(args.before), read_dir(args.after)
    except tally.Unreadable as e:
        print(f'yonder_corpus_diff: {e}', file=sys.stderr)
        return 2
    table, gone, new = diff(before, after)
    # Regressions and progress are always said, the rest when they have a
    # page; each line under a page says what it gained, lost, or why the
    # replay is not the page.
    signs = {'regressions': '+', 'progress': '-', 'broken': '!', 'still broken': '!',
             'newly incomplete': '?', 'to record again': '?', 'recovered': ''}
    for title, sign in signs.items():
        rows = table[title]
        if not rows and title not in ('regressions', 'progress'):
            continue
        print(f'{title}: {len(rows)} pages')
        for name, (address, lines) in rows.items():
            print(f'  {name}  {address}')
            for line in lines:
                print(f'    {sign} {line}')
    if gone:
        print(f'missing from {args.after}: {len(gone)} pages')
        for name in gone:
            print(f'  {name}  {before[name].address}')
    if new:
        print(f'only in {args.after}: {len(new)} pages')
    gained = sum(len(lines) for _, lines in table['regressions'].values())
    lost = sum(len(lines) for _, lines in table['progress'].values())
    incomplete = len(table['to record again']) + len(table['newly incomplete'])
    broken = len(table['broken']) + len(table['still broken'])
    print(f'before: {len(before)} pages, {clean(before)} clean; after: {len(after)} pages, '
          f'{clean(after)} clean, {incomplete} incomplete, {broken} broken; '
          f'{gained} lines gained, {lost} lines lost')
    return 1 if table['regressions'] or table['broken'] or gone else 0


if __name__ == '__main__':
    sys.exit(main())
