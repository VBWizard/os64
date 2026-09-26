#!/usr/bin/env python3
"""libgarb against css-parsing-tests (tools/css-parsing-tests, CC0).

Feeds every input of every parser file to the driver and compares its JSON
with the suite's, numbers to a relative 1e-12 (the suite's were written by
Python's correctly-rounded float(); the tokenizer computes its own).

The suite keeps two token kinds CSS Syntax Level 3 no longer has — the
attribute match operators (`~=` …) as single tokens, and `unicode-range` —
so its expectations are brought to the specification before comparing: a
match operator is the two delimiters the tokenizer now makes of it, and a
case with a unicode-range is skipped by name. It also predates the draft's
trimming of a declaration's value, so the white space at either end of an
expected value is taken off. A case in an encoding the
library does not read is skipped the same way (decode.c says which it reads).
"""
import json
import math
import subprocess
import sys
from pathlib import Path

SUITE = Path(__file__).resolve().parent / 'css-parsing-tests'
FILES = ['component_value_list', 'one_component_value', 'declaration_list',
         'blocks_contents', 'one_declaration', 'one_rule', 'rule_list',
         'stylesheet', 'stylesheet_bytes']
MATCH = {'~=', '|=', '^=', '$=', '*=', '||'}
READ = {'utf-8', 'utf-16le', 'utf-16be', 'windows-1252'}


def spec_form(x):
    """The suite's expectation, with match tokens as two delimiters."""
    if isinstance(x, list):
        out = []
        for y in x:
            if isinstance(y, str) and y in MATCH:
                out.extend([y[0], y[1]])
            else:
                out.append(spec_form(y))
        return out
    return x


def trim_declarations(x):
    """White space off both ends of every declaration's value."""
    if isinstance(x, list):
        if len(x) == 4 and x[0] == 'declaration' and isinstance(x[2], list):
            v = list(x[2])
            while v and v[0] == ' ':
                v.pop(0)
            while v and v[-1] == ' ':
                v.pop()
            return ['declaration', x[1], [trim_declarations(y) for y in v], x[3]]
        return [trim_declarations(y) for y in x]
    return x


def has_urange(x):
    if isinstance(x, list):
        return (len(x) > 0 and x[0] == 'unicode-range') or any(has_urange(y) for y in x)
    return False


def same(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if a == b:
            return True
        if math.isinf(a) or math.isinf(b):
            return False
        return abs(a - b) <= 1e-12 * max(abs(a), abs(b))
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def record(text):
    return f'{len(text)}\n'.encode() + text


def label(value):
    return b'-1\n' if value is None else record(value.encode())


def main():
    driver = sys.argv[1]
    total = failed = skipped = 0
    for name in FILES:
        cases = json.loads((SUITE / f'{name}.json').read_text(encoding='utf-8'))
        pairs = list(zip(cases[0::2], cases[1::2]))
        feed = bytearray()
        for given, _ in pairs:
            if name == 'stylesheet_bytes':
                raw = given['css_bytes'].encode('latin-1')
                feed += record(raw) + label(given.get('protocol_encoding')) \
                    + label(given.get('environment_encoding'))
            else:
                feed += record(given.encode('utf-8')) + b'-1\n-1\n'
        run = subprocess.run([driver, name], input=bytes(feed), capture_output=True)
        if run.returncode != 0:
            print(f'FAIL {name}: the driver exited {run.returncode}')
            print(run.stderr.decode(errors='replace')[:2000])
            sys.exit(1)
        lines = run.stdout.decode('utf-8').splitlines()
        if len(lines) != len(pairs):
            print(f'FAIL {name}: {len(lines)} answers for {len(pairs)} cases')
            sys.exit(1)
        for (given, want), line in zip(pairs, lines):
            total += 1
            if has_urange(want):
                skipped += 1
                continue
            if name == 'stylesheet_bytes' and want[1] not in READ:
                skipped += 1
                continue
            got = json.loads(line)
            want = trim_declarations(spec_form(want))
            if not same(got, want):
                failed += 1
                if failed <= 40:
                    print(f'FAIL {name}: {given!r}\n  want {json.dumps(want)[:400]}'
                          f'\n  got  {json.dumps(got)[:400]}')
    print(f'css-parsing-tests: {total} cases, {failed} failed, '
          f'{skipped} skipped (unicode-range tokens, encodings not read)')

    # An+B (Syntax §6), which Selectors reads nth-child() with.
    cases = json.loads((SUITE / 'An+B.json').read_text(encoding='utf-8'))
    pairs = list(zip(cases[0::2], cases[1::2]))
    feed = b''.join(record(given.encode('utf-8')) + b'-1\n-1\n' for given, _ in pairs)
    run = subprocess.run([driver, 'anplusb'], input=feed, capture_output=True)
    lines = run.stdout.decode().splitlines()
    anb_failed = 0
    for (given, want), line in zip(pairs, lines):
        if json.loads(line) != want:
            anb_failed += 1
            print(f'FAIL An+B: {given!r} want {want} got {line}')
    if run.returncode != 0 or len(lines) != len(pairs):
        print(f'FAIL An+B: the driver answered {len(lines)} of {len(pairs)}')
        anb_failed += 1
    print(f'An+B: {len(pairs)} cases, {anb_failed} failed')

    # Colours (Color 4 § 4-8) in the forms GARB.md takes: names, hex, rgb(),
    # hsl(), hwb(). lab(), lch(), oklab(), oklch() and color() are booked,
    # so their files are not run. Channels compare to 1e-4: the suite prints
    # six decimals of arithmetic the library does its own way.
    color_failed = color_total = 0
    for name in ['color_keywords_3', 'color_keywords_4', 'color_hexadecimal_3',
                 'color_hexadecimal_4', 'color_hsl_3', 'color_hsl_4', 'color_hwb_4']:
        cases = json.loads((SUITE / f'{name}.json').read_text(encoding='utf-8'))
        pairs = list(zip(cases[0::2], cases[1::2]))
        feed = b''.join(record(given.encode('utf-8')) + b'-1\n-1\n' for given, _ in pairs)
        run = subprocess.run([driver, 'color'], input=feed, capture_output=True)
        lines = run.stdout.decode().splitlines()
        if run.returncode != 0 or len(lines) != len(pairs):
            print(f'FAIL {name}: the driver answered {len(lines)} of {len(pairs)}')
            color_failed += 1
            continue
        for (given, want), line in zip(pairs, lines):
            color_total += 1
            if not same_color(json.loads(line), want):
                color_failed += 1
                if color_failed <= 20:
                    print(f'FAIL {name}: {given!r} want {want} got {line}')
    print(f'colours: {color_total} cases, {color_failed} failed')

    # Property grammars, worked by hand (tools/garb_corpus/declarations.txt).
    cases = []
    for line in (SUITE.parent / 'garb_corpus' / 'declarations.txt').read_text().splitlines():
        if line.startswith('IN: '):
            cases.append([line[4:], None])
        elif line.startswith('OUT: '):
            cases[-1][1] = line[5:]
    feed = b''.join(record(given.encode('utf-8')) + b'-1\n-1\n' for given, _ in cases)
    run = subprocess.run([driver, 'decl'], input=feed, capture_output=True)
    lines = run.stdout.decode().splitlines()
    decl_failed = 0
    for (given, want), line in zip(cases, lines):
        if line != want:
            decl_failed += 1
            print(f'FAIL declaration: {given}\n  want {want}\n  got  {line}')
    if run.returncode != 0 or len(lines) != len(cases):
        print(f'FAIL declarations: the driver answered {len(lines)} of {len(cases)}')
        decl_failed += 1
    print(f'declarations: {len(cases)} cases, {decl_failed} failed')
    sys.exit(1 if failed or anb_failed or color_failed or decl_failed else 0)


def channels(text):
    if not isinstance(text, str) or '(' not in text:
        return None
    head, body = text.split('(', 1)
    return head, [float(x) for x in body.rstrip(')').split(',')]


def same_color(got, want):
    if got is None or want is None or got == want:
        return got == want
    g, w = channels(got), channels(want)
    if g is None or w is None or g[0] != w[0] or len(g[1]) != len(w[1]):
        return False
    return all(abs(a - b) <= 1e-4 for a, b in zip(g[1], w[1]))


if __name__ == '__main__':
    main()
