#!/usr/bin/env python3
"""libjs's Intl held to a browser's capture, byte for byte (INTL.md § 8).

Usage (tools/test_intl_capture_host.sh builds the runner and calls this):
  intl_capture_check.py --runner <bin> <capture.json[.gz]> [--show N]

The capture is what tools/pages/intl-capture.html recorded in a browser:
every case's options, and per case the construction's outcome, the
resolved options, and the strings and parts of its formats and ranges
(DateTimeFormat's, NumberFormat's) or its plural categories
(PluralRules').
Each case is run again through libjs (the runner's --script mode), and
every field that differs is reported with both answers; U+202F and U+2009
are shown by name, since they are what a folded comparison would hide.
The probes and the surface are printed beside what libjs answers, for a
person to read: they are the questions on which browsers differ, so they
inform rather than gate. Exit 1 when a case differs.
"""
import argparse
import gzip
import json
import os
import subprocess
import sys
import tempfile

OURS = r'''
const C = %s;
function attempt(f) { try { return f(); } catch (e) { return 'THROWS ' + e.name; } }
const out = [];
for (const c of C) {
    const dtf = attempt(() => new Intl.DateTimeFormat('en-US', c.options));
    const r = {};
    if (typeof dtf === 'string') { r.construct = dtf; out.push(r); continue; }
    r.resolved = dtf.resolvedOptions();
    if (c.format) r.format = c.format.map(([t]) => [t, dtf.format(t)]);
    if (c.parts) r.parts = c.parts.map(([t]) => [t, dtf.formatToParts(t)]);
    if (c.range) r.range = c.range.map(([x, y]) => [x, y, dtf.formatRange(x, y)]);
    if (c.rangeParts) r.rangeParts = c.rangeParts.map(([x, y]) => [x, y, dtf.formatRangeToParts(x, y)]);
    out.push(r);
}
// The number cases, each value written as the page writes it.
function numberValue(text) {
    const kind = text.slice(0, 2), body = text.slice(2);
    return kind === 'b:' ? BigInt(body) : kind === 's:' ? body : body === '-0' ? -0 : Number(body);
}
const numberOut = [];
for (const c of %s) {
    const C = c.kind === 'PluralRules' ? Intl.PluralRules : Intl.NumberFormat;
    const f = attempt(() => new C('en-US', c.options));
    const r = {};
    if (typeof f === 'string') { r.construct = f; numberOut.push(r); continue; }
    r.resolved = f.resolvedOptions();
    if (c.format) r.format = c.format.map(([v]) => [v, attempt(() => f.format(numberValue(v)))]);
    if (c.parts) r.parts = c.parts.map(([v]) => [v, attempt(() => f.formatToParts(numberValue(v)))]);
    if (c.range) r.range = c.range.map(([x, y]) => [x, y, attempt(() => f.formatRange(numberValue(x), numberValue(y)))]);
    if (c.rangeParts) r.rangeParts = c.rangeParts.map(([x, y]) => [x, y, attempt(() => f.formatRangeToParts(numberValue(x), numberValue(y)))]);
    if (c.select) r.select = c.select.map(([v]) => [v, attempt(() => f.select(numberValue(v)))]);
    if (c.selectRange) r.selectRange = c.selectRange.map(([x, y]) => [x, y, attempt(() => f.selectRange(numberValue(x), numberValue(y)))]);
    numberOut.push(r);
}
const zoneOf = z => attempt(() => new Intl.DateTimeFormat('en-US', {timeZone: z}).resolvedOptions().timeZone);
const probes = {};
for (const name of %s) if (name.startsWith('timeZone ')) probes[name] = zoneOf(name.slice(9));
probes['supportedLocalesOf de-DE en-GB en-US'] = Intl.DateTimeFormat.supportedLocalesOf(['de-DE', 'en-GB', 'en-US']);
probes['de-DE resolved locale'] = new Intl.DateTimeFormat('de-DE').resolvedOptions().locale;
probes['Locale und-Thai maximize'] = attempt(() => new Intl.Locale('und-Thai').maximize().toString());
probes['Locale en maximize'] = attempt(() => new Intl.Locale('en').maximize().toString());
probes['Locale cel-gaulish'] = attempt(() => new Intl.Locale('cel-gaulish').toString());
const surface = {Intl: Object.getOwnPropertyNames(Intl).sort()};
for (const name of surface.Intl) {
    const value = Intl[name];
    if (typeof value === 'function' && value.prototype) surface[name] = Object.getOwnPropertyNames(value.prototype).sort();
}
print(JSON.stringify({cases: out, numberCases: numberOut, probes, surface}));
'''


def shown(value):
    text = json.dumps(value, ensure_ascii=False)
    return text.replace(' ', '<NNBSP>').replace(' ', '<THIN>')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runner', required=True)
    parser.add_argument('--show', type=int, default=40)
    parser.add_argument('capture')
    args = parser.parse_args()
    opener = gzip.open if args.capture.endswith('.gz') else open
    with opener(args.capture, 'rt', encoding='utf-8') as f:
        capture = json.load(f)
    cases = capture['cases']
    number_cases = capture.get('numberCases', [])     # absent from a capture older than NumberFormat
    with tempfile.TemporaryDirectory(prefix='os64-intl-capture.') as work:
        script = os.path.join(work, 'check.js')
        with open(script, 'w', encoding='utf-8') as f:
            f.write(OURS % (json.dumps(cases), json.dumps(number_cases), json.dumps(sorted(capture['probes']))))
        run = subprocess.run([args.runner, '--script', script], capture_output=True, text=True,
                             env=dict(os.environ, TZ='EST5EDT'))
    if run.returncode != 0:
        sys.stderr.write(run.stderr)
        sys.exit(2)
    ours = json.loads(run.stdout)
    differences, compared = [], 0
    for theirs, mine in zip(cases, ours['cases']):
        for field in ('construct', 'resolved', 'format', 'parts', 'range', 'rangeParts'):
            if field not in theirs and field not in mine:
                continue
            compared += 1
            if theirs.get(field) != mine.get(field):
                differences.append((theirs['options'], field, theirs.get(field), mine.get(field)))
    for theirs, mine in zip(number_cases, ours['numberCases']):
        for field in ('construct', 'resolved', 'format', 'parts', 'range', 'rangeParts', 'select', 'selectRange'):
            if field not in theirs and field not in mine:
                continue
            compared += 1
            if theirs.get(field) != mine.get(field):
                differences.append(({theirs['kind']: theirs['options']}, field, theirs.get(field), mine.get(field)))
    print('intl capture (%s): %d of %d fields agree across %d option sets'
          % (capture.get('userAgent', '?')[:60], compared - len(differences), compared, len(cases) + len(number_cases)))
    for options, field, theirs, mine in differences[:args.show]:
        print('  %s %s\n    browser %s\n    libjs   %s' % (shown(options), field, shown(theirs), shown(mine)))
    if len(differences) > args.show:
        print('  ... %d more' % (len(differences) - args.show))
    print('probes (the browser, then libjs):')
    for name, value in sorted(capture['probes'].items()):
        mine = ours['probes'].get(name, '(not asked of libjs: depends on the capturing machine)')
        mark = '' if value == mine else '   <- differs'
        print('  %s: %s | %s%s' % (name, shown(value), shown(mine), mark))
    print('surface (names the browser has that libjs lacks):')
    for name, names in sorted(capture['surface'].items()):
        missing = sorted(set(names) - set(ours['surface'].get(name, [])))
        if missing:
            print('  %s: %s' % (name, ', '.join(missing)))
    sys.exit(1 if differences else 0)


if __name__ == '__main__':
    main()
