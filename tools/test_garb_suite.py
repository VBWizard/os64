#!/usr/bin/env python3
"""libgarb against css-parsing-tests (tools/css-parsing-tests, CC0).

Feeds every input of every parser file to the driver and compares its JSON
with the suite's, numbers to a relative 1e-12 (the suite's were written by
Python's correctly-rounded float(); the tokenizer computes its own).

The suite keeps two token kinds CSS Syntax Level 3 no longer has — the
attribute match operators (`~=` …) as single tokens, and `unicode-range` —
so its expectations are brought to the specification before comparing: a
match operator standing as a token (never one spelled inside a string or
another token's payload) is the two delimiters the tokenizer now makes of
it, and a
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


# A node is a list whose first element names its kind. A scalar node's
# other elements are PAYLOAD — a string's text, an ident's name — and are
# never tokens, whatever they spell; the rest hold component values at the
# positions given here (None: every position past the kind).
SCALAR = {'ident', 'at-keyword', 'hash', 'string', 'url', 'number', 'percentage',
          'dimension', 'unicode-range', 'error'}
VALUE_LISTS = {'{}': None, '[]': None, '()': None, 'function': 2,
               'declaration': (2,), 'qualified rule': (1, 2), 'at-rule': (2, 3)}


def spec_values(xs):
    """A list of component values, its match tokens as two delimiters."""
    out = []
    for y in xs:
        if isinstance(y, str) and y in MATCH:
            out.extend([y[0], y[1]])
        else:
            out.append(spec_form(y))
    return out


def spec_form(x):
    """The suite's expectation, with match tokens as two delimiters."""
    if not isinstance(x, list):
        return x
    kind = x[0] if x and isinstance(x[0], str) else None
    if kind in SCALAR:
        return x
    if kind in VALUE_LISTS:
        where = VALUE_LISTS[kind]
        if where is None:
            return [kind] + spec_values(x[1:])
        if isinstance(where, int):
            return x[:where] + spec_values(x[where:])
        return [spec_values(y) if i in where and isinstance(y, list) else y
                for i, y in enumerate(x)]
    # A list of rules and declarations, or of component values.
    return spec_values(x)


def payloads(x):
    """Every scalar node's payload, in order: what spec_form must not touch."""
    if not isinstance(x, list):
        return []
    if x and isinstance(x[0], str) and x[0] in SCALAR:
        return [repr(x)]
    return [p for y in x for p in payloads(y)]


def spec_form_selftest():
    """spec_form changes match tokens and nothing else."""
    cases = [
        (['string', '~='], ['string', '~=']),
        (['url', '|='], ['url', '|=']),
        (['{}', '~=', ['string', '*=']], ['{}', '~', '=', ['string', '*=']]),
        (['function', 'f', '^=', ' '], ['function', 'f', '^', '=', ' ']),
        (['declaration', 'a', ['$=', ['ident', '||']], False],
         ['declaration', 'a', ['$', '=', ['ident', '||']], False]),
        ([['qualified rule', ['a', '||'], ['~=']]],
         [['qualified rule', ['a', '|', '|'], ['~', '=']]]),
        (['||', ['error', '~=']], ['|', '|', ['error', '~=']]),
    ]
    for given, want in cases:
        got = spec_form(given)
        if got != want:
            sys.exit(f'spec_form self-test: {given!r} -> {got!r}, want {want!r}')


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
    spec_form_selftest()
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
            spec = spec_form(want)
            if payloads(spec) != payloads(want):
                sys.exit(f'spec_form rewrote a payload in {name}: {given!r}')
            want = trim_declarations(spec)
            if not same(got, want):
                failed += 1
                if failed <= 40:
                    print(f'FAIL {name}: {given!r}\n  want {json.dumps(want)[:400]}'
                          f'\n  got  {json.dumps(got)[:400]}')
    print(f'css-parsing-tests: {total} cases, {failed} failed, '
          f'{skipped} skipped (unicode-range tokens, encodings not read)')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
