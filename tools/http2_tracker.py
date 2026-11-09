#!/usr/bin/env python3
"""The HTTP/2 arc's tracker: check it, print the board, render the page.

    tools/http2_tracker.py                 check docs/design/pending/HTTP2_TRACKER.csv, print the board
    tools/http2_tracker.py --html out.html ...and write the page Chris reads

HTTP2.md § 0 is the contract: one row per task, six columns, three kinds of
row told apart by their id (a slice `H2.3`, a sub-task `H2.3.4` or a
capture `C1`, a question `Q1`), each kind with its own status vocabulary.
The CSV is the truth and the page is generated from it, never edited. A
check that fails names the row and exits nonzero, so a push that would
break the board stops here. Nothing in this file knows what the tasks ARE;
it knows only the shape every row must have.
"""
import argparse
import csv
import html
import re
import sys
from pathlib import Path

TRACKER = Path(__file__).resolve().parent.parent / 'docs' / 'design' / 'pending' / 'HTTP2_TRACKER.csv'
COLUMNS = ['id', 'task', 'depends_on', 'status', 'done_in', 'notes']

# A slice row walks the house's lifecycle in this order; a sub-task has the
# short vocabulary; a question is open until Chris rules.
SLICE_STATUSES = ['todo', 'design', 'design-review', 'code', 'review', 'p5', 'done', 'dropped']
TASK_STATUSES = ['todo', 'doing', 'done', 'dropped']
QUESTION_STATUSES = ['open', 'ruled']

SLICE_ID = re.compile(r'^H2\.\d+$')
TASK_ID = re.compile(r'^(H2\.\d+\.\d+|C\d+)$')
QUESTION_ID = re.compile(r'^Q\d+$')


def kind_of(row_id):
    if SLICE_ID.match(row_id):
        return 'slice'
    if TASK_ID.match(row_id):
        return 'task'
    if QUESTION_ID.match(row_id):
        return 'question'
    return None


def slice_of(row_id):
    """The slice a sub-task belongs to by its id; captures and questions
    have none and are grouped on their own."""
    m = re.match(r'^(H2\.\d+)\.\d+$', row_id)
    return m.group(1) if m else None


def read(path):
    with open(path, newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != COLUMNS:
            raise SystemExit(f'{path}: columns are {reader.fieldnames}, not {COLUMNS}')
        # A blank line between slices is the file's own convention (§ 0 rule 4);
        # csv hands it back as a row of empty fields.
        return [r for r in reader if any((v or '').strip() for v in r.values())]


def check(rows):
    problems = []
    ids = [r['id'] for r in rows]
    seen = set()
    for i in ids:
        if i in seen:
            problems.append(f'{i}: listed twice')
        seen.add(i)
    for r in rows:
        kind = kind_of(r['id'])
        if kind is None:
            problems.append(f"{r['id']}: not a slice (H2.n), sub-task (H2.n.m or Cn) or question (Qn) id")
            continue
        allowed = {'slice': SLICE_STATUSES, 'task': TASK_STATUSES, 'question': QUESTION_STATUSES}[kind]
        if r['status'] not in allowed:
            problems.append(f"{r['id']}: status '{r['status']}' is not one of {allowed} for a {kind}")
        if kind == 'task' and slice_of(r['id']) and slice_of(r['id']) not in seen:
            problems.append(f"{r['id']}: its slice {slice_of(r['id'])} has no row")
        for dep in r['depends_on'].split():
            if dep not in seen:
                problems.append(f"{r['id']}: depends on {dep}, which has no row")
        if r['status'] == 'done' and not r['done_in'].strip():
            problems.append(f"{r['id']}: done without a done_in (the PR number)")
        if r['status'] == 'ruled' and not r['notes'].strip():
            problems.append(f"{r['id']}: ruled with no ruling in notes")
        if not r['task'].strip():
            problems.append(f"{r['id']}: no task text")
    return problems


def grouped(rows):
    """Slices in file order, each with its sub-tasks; then captures; then
    questions."""
    slices = []
    by_slice = {}
    captures, questions = [], []
    for r in rows:
        kind = kind_of(r['id'])
        if kind == 'slice':
            slices.append(r)
            by_slice[r['id']] = []
        elif kind == 'question':
            questions.append(r)
        elif slice_of(r['id']):
            by_slice.setdefault(slice_of(r['id']), []).append(r)
        else:
            captures.append(r)
    return slices, by_slice, captures, questions


def progress(tasks):
    counted = [t for t in tasks if t['status'] != 'dropped']
    done = sum(1 for t in counted if t['status'] == 'done')
    return done, len(counted)


def board(rows):
    slices, by_slice, captures, questions = grouped(rows)
    out = []
    for s in slices:
        done, total = progress(by_slice.get(s['id'], []))
        out.append(f"{s['id']:<8} {s['status']:<14} {done:>2}/{total:<2}  {s['task']}")
        for t in by_slice.get(s['id'], []):
            mark = {'done': 'x', 'doing': '>', 'dropped': '-'}.get(t['status'], ' ')
            out.append(f"  [{mark}] {t['id']:<9} {t['task']}")
    if captures:
        out.append('captures')
        for t in captures:
            mark = {'done': 'x', 'doing': '>', 'dropped': '-'}.get(t['status'], ' ')
            out.append(f"  [{mark}] {t['id']:<9} {t['task']}")
    if questions:
        out.append('questions')
        for q in questions:
            out.append(f"  {q['id']:<4} {q['status']:<6} {q['task']}" + (f"  -- {q['notes']}" if q['status'] == 'ruled' else ''))
    return '\n'.join(out)


PAGE_HEAD = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HTTP/2 Tracker</title>
<style>
:root{--bg:#fbfaf7;--fg:#1d1b17;--mute:#6b665c;--line:#e2ddd2;--done:#3b7d4f;--doing:#b7791f;--open:#a33a2e;--bar:#e8e3d8;--card:#fff}
@media (prefers-color-scheme: dark){:root:not([data-theme=light]){--bg:#17161a;--fg:#ece8e1;--mute:#a39d92;--line:#2d2b30;--done:#6fbf85;--doing:#e0a84a;--open:#e07a6d;--bar:#2a282d;--card:#1f1e22}}
:root[data-theme=dark]{--bg:#17161a;--fg:#ece8e1;--mute:#a39d92;--line:#2d2b30;--done:#6fbf85;--doing:#e0a84a;--open:#e07a6d;--bar:#2a282d;--card:#1f1e22}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
main{max-width:980px;margin:0 auto;padding:24px 16px 48px}
h1{font-size:22px;margin:0 0 4px}
.sub{color:var(--mute);margin:0 0 20px}
.slice{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:0 0 14px}
.slice h2{font-size:16px;margin:0 0 6px;display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
.id{font-family:ui-monospace,monospace;color:var(--mute)}
.status{font-size:12px;padding:1px 8px;border-radius:999px;border:1px solid var(--line);color:var(--mute)}
.status.done{color:var(--done);border-color:var(--done)}.status.doing,.status.code,.status.review,.status.p5,.status.design,.status.design-review{color:var(--doing);border-color:var(--doing)}
.status.open{color:var(--open);border-color:var(--open)}
.bar{height:6px;background:var(--bar);border-radius:3px;overflow:hidden;margin:6px 0 10px}
.bar i{display:block;height:100%;background:var(--done)}
ul{list-style:none;margin:0;padding:0}
li{display:grid;grid-template-columns:22px 86px 1fr;gap:8px;padding:4px 0;border-top:1px solid var(--line)}
li.done .task{text-decoration:line-through;color:var(--mute)}
li.dropped .task{color:var(--mute);font-style:italic}
.box{font-family:ui-monospace,monospace;color:var(--mute)}
.meta{color:var(--mute);font-size:13px}
.q .task{font-weight:600}
@media (max-width:520px){li{grid-template-columns:22px 1fr}li .id{display:none}}
</style></head><body><main>
"""


def page(rows, source):
    slices, by_slice, captures, questions = grouped(rows)
    total_done = total_all = 0
    out = [PAGE_HEAD, '<h1>HTTP/2 Tracker</h1>']
    out.append(f'<p class="sub">Generated from <code>{html.escape(source)}</code> by tools/http2_tracker.py. '
               'HTTP2.md &sect; 0 says how the file is kept.</p>')

    def item(t, extra_class=''):
        box = {'done': '&#x2713;', 'doing': '&#x25B6;', 'dropped': '&#x2014;'}.get(t['status'], '&#x25A1;')
        meta = []
        if t['done_in']:
            meta.append('PR ' + html.escape(t['done_in']))
        if t['depends_on']:
            meta.append('after ' + html.escape(t['depends_on']))
        if t['notes']:
            meta.append(html.escape(t['notes']))
        m = f'<div class="meta">{" &middot; ".join(meta)}</div>' if meta else ''
        return (f'<li class="{t["status"]} {extra_class}"><span class="box">{box}</span>'
                f'<span class="id">{html.escape(t["id"])}</span>'
                f'<span><span class="task">{html.escape(t["task"])}</span>{m}</span></li>')

    for s in slices:
        tasks = by_slice.get(s['id'], [])
        done, total = progress(tasks)
        total_done += done
        total_all += total
        pct = int(100 * done / total) if total else 0
        out.append('<section class="slice">')
        out.append(f'<h2><span class="id">{html.escape(s["id"])}</span>{html.escape(s["task"])}'
                   f'<span class="status {s["status"]}">{html.escape(s["status"])}</span>'
                   f'<span class="meta">{done}/{total}</span></h2>')
        if s['depends_on'] or s['notes'] or s['done_in']:
            bits = []
            if s['done_in']:
                bits.append('PR ' + html.escape(s['done_in']))
            if s['depends_on']:
                bits.append('after ' + html.escape(s['depends_on']))
            if s['notes']:
                bits.append(html.escape(s['notes']))
            out.append(f'<div class="meta">{" &middot; ".join(bits)}</div>')
        out.append(f'<div class="bar"><i style="width:{pct}%"></i></div>')
        if tasks:
            out.append('<ul>' + ''.join(item(t) for t in tasks) + '</ul>')
        out.append('</section>')
    if captures:
        out.append('<section class="slice"><h2>Captures</h2><ul>' + ''.join(item(t) for t in captures) + '</ul></section>')
    if questions:
        out.append('<section class="slice"><h2>Questions for Chris</h2><ul>')
        for q in questions:
            box = '&#x2713;' if q['status'] == 'ruled' else '?'
            notes = f'<div class="meta">{html.escape(q["notes"])}</div>' if q['notes'] else ''
            out.append(f'<li class="q"><span class="box">{box}</span><span class="id">{html.escape(q["id"])}</span>'
                       f'<span><span class="task">{html.escape(q["task"])}</span>'
                       f' <span class="status {q["status"]}">{q["status"]}</span>{notes}</span></li>')
        out.append('</ul></section>')
    out.insert(2, f'<p class="sub">{total_done} of {total_all} sub-tasks done across {len(slices)} slices.</p>')
    out.append('</main></body></html>\n')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--csv', default=str(TRACKER), help='the tracker to read (default: the repo\'s)')
    ap.add_argument('--html', metavar='PATH', help='write the rendered page here')
    ap.add_argument('--quiet', action='store_true', help='check only; print nothing on success')
    args = ap.parse_args()
    rows = read(args.csv)
    problems = check(rows)
    if problems:
        for p in problems:
            print(f'{args.csv}: {p}', file=sys.stderr)
        return 1
    if args.html:
        Path(args.html).write_text(page(rows, Path(args.csv).name), encoding='utf-8')
    if not args.quiet:
        print(board(rows))
        if args.html:
            print(f'\nwrote {args.html}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
