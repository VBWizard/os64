#!/usr/bin/env python3
"""Which kept pages got worse between two replays of the corpus
(MODERN_WEB.md rule 3).

    yonder_corpus_diff.py BEFORE-DIR AFTER-DIR

Each directory holds the records two replays of ONE kept corpus wrote, by
the same names, as tools/test_yonder_replay_host.sh writes them; a record
no replay wrote (yonder's own, with no state line) is not compared. Each
is read through
tools/yonder_records.py, the one reader of page files, which says what
state its page came to: a replay's record says it on its second line. ONLY
A PAGE REPLAYED TO A VERDICT IS JUDGED. Such a page is the set of its
record lines, `MISSING <kind> <name>` and `FAILED <what>: <message>`, with
their counts left out: a count moves with timing, and a line appearing or
going is what a slice changed. A REPLAY DOES NOT REPRODUCE WHAT A FETCH IT
NEVER PERFORMS DECIDED (a picture's or a web font's fetch, and what its
bytes came to), and its record names those kinds on a `REPLAY not
reproduced:` line: the lines of every kind any record on either side names
there are compared on neither side, and the summary says which, so a kept
page's picture that failed to fetch is not read as progress when its
replay has none. A picture or a font decided without a fetch (a scheme
nothing fetches, an address refused, a family no face names) is compared.
Judged on both sides, a page whose set GAINED a line is a regression,
printed with the lines it gained, and one that lost lines is progress,
printed with those.

The rest each have a heading of their own, by the after-record's state
(the reader's State, whose order is the precedence: a record that says it
is both is the later), and none is progress:

- BROKEN: the after-record says `REPLAY unfinished`, the replay reached no
  verdict (the page never arrived, or asked for the next turn for ever),
  so what it shows is not the page. A regression when the before-record
  reached one; still broken otherwise.
- NEWLY INCOMPLETE: the after-record is incomplete (the page asked for
  something the corpus never kept, or never asked for something it kept)
  and the before-record was not. It is A GATE: the corpus must be
  recorded again before the change can be judged on that page, so the
  answer is 1, and the heading says so.
- TO RECORD AGAIN: incomplete on both sides. Listed with what it met,
  neither a regression nor progress, and never silent.
- RECOVERED: the after-record reached a verdict where the before-record
  did not; its lines are not compared with a record that was not the page.
- UNJUDGED: a `*.txt` on either side the reader cannot judge (one that
  cannot be read, is not a page file, or is not whole: a file cut short
  reads as a page that stopped asking for everything it lost), a record
  no replay wrote, or one whose replay says its page is unjudged (its kept
  record lost lines or was never written, a harness check failed, its
  record could not be written), and a pair whose records name two
  addresses, took different inputs from the harness, were run with
  different `--window` statements, or were given different budgets.
  Listed with why, and judged on neither side.
- A page the after directory does not have is a regression, since whatever
  it gained cannot be seen; a page only the after directory has is
  unjudged, since the before replay did not replay it (it stopped, or was
  partial).

Exit 2 when a directory cannot be listed or any page is unjudged; else 1
when there is any regression, broken page, missing page or newly
incomplete page; else 0. Run it with `python3 -I`; the
records are data, and the reader is loaded from beside this file.
"""
import argparse
import importlib.util
import pathlib
import sys


def load_records():
    path = pathlib.Path(__file__).resolve().parent / 'yonder_records.py'
    spec = importlib.util.spec_from_file_location('yonder_records', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


records = load_records()
State = records.State
display = records.display


def lines_of(rec, skip):
    """The page's record lines, counts left out, but those of the kinds in
    `skip`, a MISSING line's kind or a FAILED line's what."""
    out = {f'MISSING {kind} {name}' for kind, name in rec.missing if kind not in skip}
    out |= {f'FAILED {what}: {message}' for what, message in rec.failed if what not in skip}
    return out


def read_dir(path, unjudged):
    """{file name: record} for the replays' records in a directory: each
    read whole by the reader, and one it cannot judge, that a replay did
    not write, or whose record says its page is UNJUDGED, put in
    `unjudged` as {name: reason} instead.
    Unreadable (the reader's) when the directory cannot be listed: read as
    empty, it would read as a corpus that did not change."""
    pages = {}
    for f in records.page_files(path):
        rec = records.read(f)
        if isinstance(rec, records.Unjudged):
            unjudged[f.name] = str(rec)
        elif not rec.replay:
            unjudged[f.name] = (f"{f}: not a replay's record (a replay's says its state on its second line); "
                                'the diff compares replays of one kept corpus')
        elif rec.state == State.UNJUDGED:
            unjudged[f.name] = f'{f}: unjudged: {rec.reason}'
        else:
            pages[f.name] = rec
    return pages


def not_reproduced(*sides):
    """The kinds any record on these sides says its replay does not
    reproduce."""
    return set().union(*(r.not_reproduced for pages in sides for r in pages.values()))


def diff(before, after, skip):
    """{heading: {name: (address, lines)}} for the headings the module's
    comment names, comparing no line of a kind in `skip`, and the names
    only one side has (gone, new)."""
    out = {h: {} for h in ('regressions', 'progress', 'broken', 'still broken', 'newly incomplete',
                           'to record again', 'recovered')}
    for name in sorted(before.keys() & after.keys()):
        was, now = before[name], after[name]
        b, a = was.state, now.state
        if a == State.UNFINISHED:
            # Why it reached no verdict, then what it asked for that was
            # never kept, which is detail here.
            said = [f'unfinished {w}' for w in now.unfinished] + [f'incomplete {i}' for i in now.incomplete]
            out['still broken' if b == State.UNFINISHED else 'broken'][name] = (now.address, said)
        elif a == State.INCOMPLETE:
            out['to record again' if b == State.INCOMPLETE else 'newly incomplete'][name] = (now.address, now.incomplete)
        elif b != State.VERDICT:
            out['recovered'][name] = (now.address, [])
        else:
            gained, lost = lines_of(now, skip) - lines_of(was, skip), lines_of(was, skip) - lines_of(now, skip)
            if gained:
                out['regressions'][name] = (now.address, sorted(gained))
            if lost:
                out['progress'][name] = (now.address, sorted(lost))
    gone = sorted(before.keys() - after.keys())
    new = sorted(after.keys() - before.keys())
    return out, gone, new


def clean(pages, skip):
    return sum(1 for r in pages.values() if r.state == State.VERDICT and not lines_of(r, skip))


def main(argv=None):
    ap = argparse.ArgumentParser(description='Name every kept page whose record got worse.')
    ap.add_argument('before')
    ap.add_argument('after')
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(errors='backslashreplace')
    for d in (args.before, args.after):
        if not pathlib.Path(d).is_dir():
            print(f'yonder_corpus_diff: {display(d)} is not a directory', file=sys.stderr)
            return 2
    unjudged = {}
    try:
        before, after = read_dir(args.before, unjudged), read_dir(args.after, unjudged)
    except records.Unreadable as e:
        print(f'yonder_corpus_diff: {display(e)}', file=sys.stderr)
        return 2
    # Paired by name, a page must be one page: records whose addresses
    # differ are two pages, and comparing them would be no comparison.
    # And a page is compared only with itself read the same way: records
    # that took different inputs from the harness (`REPLAY inputs
    # defaulted:`), were run with different `--window` statements
    # (`REPLAY inputs stated:`) or given different budgets (`REPLAY
    # budget:`, `--seconds`), were read in two different windows.
    for name in sorted(before.keys() & after.keys()):
        if before[name].address != after[name].address:
            unjudged[name] = (f'{name}: its records name two addresses, {before[name].address} and '
                              f'{after[name].address}')
        elif before[name].defaulted != after[name].defaulted:
            unjudged[name] = (f'{name}: its replays took different inputs from the harness, '
                              f'{" ".join(sorted(before[name].defaulted)) or "none"} and '
                              f'{" ".join(sorted(after[name].defaulted)) or "none"}')
        elif before[name].budget != after[name].budget:
            unjudged[name] = (f'{name}: its replays were given different budgets, {before[name].budget} s and '
                              f'{after[name].budget} s')
        elif before[name].stated != after[name].stated:
            unjudged[name] = (f'{name}: its replays were run with different --window statements, '
                              f'{" ".join(f"{k}={v}" for k, v in sorted(before[name].stated.items())) or "none"} and '
                              f'{" ".join(f"{k}={v}" for k, v in sorted(after[name].stated.items())) or "none"}')
    # A page not judged on one side is judged on neither.
    for name in unjudged:
        before.pop(name, None)
        after.pop(name, None)
    skip = not_reproduced(before, after)
    table, gone, new = diff(before, after, skip)
    # Regressions and progress are always said, the rest when they have a
    # page; each line under a page says what it gained, lost, or why the
    # replay is not the page.
    signs = {'regressions': '+', 'progress': '-', 'broken': '!', 'still broken': '!',
             'newly incomplete': '?', 'to record again': '?', 'recovered': ''}
    for title, sign in signs.items():
        rows = table[title]
        if not rows and title not in ('regressions', 'progress'):
            continue
        gate = (' (the corpus must be re-recorded before this change can be judged on them)'
                if title == 'newly incomplete' else '')
        print(f'{title}: {len(rows)} pages{gate}')
        for name, (address, lines) in rows.items():
            print(f'  {display(name)}  {display(address)}')
            for line in lines:
                print(f'    {sign} {display(line)}')
    if gone:
        print(f'missing from {display(args.after)}: {len(gone)} pages')
        for name in gone:
            print(f'  {display(name)}  {display(before[name].address)}')
    # Both replays read one kept corpus, so a page only the after one has
    # is a page the before one never replayed (it stopped, or was
    # partial): nothing to compare it with, and never silent.
    for name in new:
        unjudged[name] = f'{name}: {args.before} did not replay this page'
        after.pop(name, None)
    if unjudged:
        print(f'unjudged: {len(unjudged)} pages')
        for name, why in sorted(unjudged.items()):
            print(f'  {display(name)}  {display(why)}')
    gained = sum(len(lines) for _, lines in table['regressions'].values())
    lost = sum(len(lines) for _, lines in table['progress'].values())
    incomplete = len(table['to record again']) + len(table['newly incomplete'])
    broken = len(table['broken']) + len(table['still broken'])
    print(f'before: {len(before)} pages, {clean(before, skip)} clean; after: {len(after)} pages, '
          f'{clean(after, skip)} clean, {incomplete} incomplete, {broken} broken; '
          f'{gained} lines gained, {lost} lines lost'
          + (f'; not compared, as the replay does not reproduce them: {" ".join(sorted(skip))}' if skip else ''))
    if unjudged:
        return 2
    return 1 if table['regressions'] or table['broken'] or table['newly incomplete'] or gone else 0


if __name__ == '__main__':
    sys.exit(main())
