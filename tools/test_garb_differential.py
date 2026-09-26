#!/usr/bin/env python3
"""libgarb's parser against tinycss2 (GARB.md, G1): whole sheets, every
style rule's block, and sheets mutated at random, compared node for node.

    python3 tools/test_garb_differential.py DRIVER [SHEETS_GLOB] [SEEDS]

DRIVER is the harness's garb_driver (tools/test_garb_host.sh builds it; keep
its work directory with a failing run, or build it by hand the same way).
SHEETS_GLOB defaults to tools/garb_corpus/*.css; point it at a real site's
sheets for a real-world run. tinycss2 1.4 is the reference and is not a
system package here — make a scratch venv for it:

    python3 -m venv /tmp/garbvenv && /tmp/garbvenv/bin/pip install tinycss2==1.4.0
    /tmp/garbvenv/bin/python tools/test_garb_differential.py DRIVER

The reference predates three things the current Syntax draft does, and each
is brought to the draft before comparing (test_garb_suite.py does the same
for the suite): match operators are two delimiters, a declaration's value
is trimmed, and a top-level rule whose prelude begins `--x:` is thrown away
(§5.4.3). Inputs with a unicode-range token are skipped.
"""
import glob
import json
import random
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_garb_suite import has_urange, same, spec_form, trim_declarations  # noqa: E402

try:
    import tinycss2
except ImportError:
    print(__doc__)
    sys.exit(2)


def cv(n):
    kind = type(n).__name__
    simple = {'WhitespaceToken': lambda: ' ', 'LiteralToken': lambda: n.value}
    if kind in simple:
        return simple[kind]()
    if kind == 'IdentToken':
        return ['ident', n.value]
    if kind == 'AtKeywordToken':
        return ['at-keyword', n.value]
    if kind == 'HashToken':
        return ['hash', n.value, 'id' if n.is_identifier else 'unrestricted']
    if kind == 'StringToken':
        return ['string', n.value]
    if kind == 'URLToken':
        return ['url', n.value]
    if kind in ('NumberToken', 'PercentageToken', 'DimensionToken'):
        name = {'NumberToken': 'number', 'PercentageToken': 'percentage',
                'DimensionToken': 'dimension'}[kind]
        out = [name, n.representation, n.value, 'integer' if n.is_integer else 'number']
        return out + [n.unit] if kind == 'DimensionToken' else out
    if kind == 'UnicodeRangeToken':
        return ['unicode-range', n.start, n.end]
    if kind in ('CurlyBracketsBlock', 'SquareBracketsBlock', 'ParenthesesBlock'):
        head = {'CurlyBracketsBlock': '{}', 'SquareBracketsBlock': '[]',
                'ParenthesesBlock': '()'}[kind]
        return [head] + listed(n.content)
    if kind == 'FunctionBlock':
        return ['function', n.name] + listed(n.arguments)
    if kind == 'Comment':
        return None
    if kind == 'ParseError':
        return ['error', n.kind]
    if kind == 'QualifiedRule':
        return ['qualified rule', listed(n.prelude), listed(n.content)]
    if kind == 'AtRule':
        return ['at-rule', n.at_keyword, listed(n.prelude),
                listed(n.content) if n.content is not None else None]
    if kind == 'Declaration':
        return ['declaration', n.name, listed(n.value), n.important]
    raise TypeError(kind)


def listed(nodes):
    return [c for c in (cv(n) for n in nodes) if c is not None]


def lookalike(rule):
    if not (isinstance(rule, list) and rule and rule[0] == 'qualified rule'):
        return False
    prelude = [v for v in rule[1] if v != ' ']
    return (len(prelude) >= 2 and isinstance(prelude[0], list) and prelude[0][0] == 'ident'
            and prelude[0][1].startswith('--') and prelude[1] == ':')


def drive(driver, mode, inputs):
    feed = bytearray()
    for text in inputs:
        feed += f'{len(text)}\n'.encode() + text + b'-1\n-1\n'
    run = subprocess.run([driver, mode], input=bytes(feed), capture_output=True)
    if run.returncode != 0:
        print(f'FAIL: the driver exited {run.returncode}\n'
              + run.stderr.decode(errors='replace')[:3000])
        sys.exit(1)
    return run.stdout.decode('utf-8', 'replace').splitlines()


def compare(label, inputs, answers, expected):
    bad = skipped = 0
    for given, line, want in zip(inputs, answers, expected):
        if want is None:
            skipped += 1
            continue
        got = json.loads(line)
        if not same(got, want):
            bad += 1
            if bad <= 3:
                print(f'FAIL {label}: {given[:160]!r}\n  want {json.dumps(want)[:400]}'
                      f'\n  got  {json.dumps(got)[:400]}')
    print(f'{label}: {len(inputs)}, {bad} differ, {skipped} skipped (unicode-range)')
    return bad


def sheet_expectation(data):
    rules = listed(tinycss2.parse_stylesheet_bytes(data, skip_comments=True,
                                                   skip_whitespace=True)[0])
    if has_urange(rules):
        return None
    return trim_declarations(spec_form([['error', 'invalid'] if lookalike(r) else r
                                        for r in rules]))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    driver = sys.argv[1]
    pattern = sys.argv[2] if len(sys.argv) > 2 else 'tools/garb_corpus/*.css'
    seeds = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    sheets = [Path(f).read_bytes() for f in sorted(glob.glob(pattern))]
    if not sheets:
        print(f'no sheets match {pattern}')
        sys.exit(2)
    bad = 0

    # Whole sheets.
    answers = [json.loads(a)[0] for a in drive(driver, 'stylesheet_bytes', sheets)]
    bad += compare('sheets', sheets, [json.dumps(a) for a in answers],
                   [sheet_expectation(s) for s in sheets])

    # Every rule's block, as a block's contents, nested rules included.
    blocks = []
    for data in sheets:
        stack = list(tinycss2.parse_stylesheet_bytes(data, skip_comments=True,
                                                     skip_whitespace=True)[0])
        while stack:
            rule = stack.pop()
            if type(rule).__name__ in ('QualifiedRule', 'AtRule') and rule.content is not None:
                blocks.append(tinycss2.serialize(rule.content).encode())
                stack += [x for x in tinycss2.parse_blocks_contents(rule.content)
                          if type(x).__name__ in ('QualifiedRule', 'AtRule')]
    expected = []
    for b in blocks:
        items = listed(n for n in tinycss2.parse_blocks_contents(b.decode(), skip_comments=True,
                                                                  skip_whitespace=True))
        expected.append(None if has_urange(items) else trim_declarations(spec_form(items)))
    bad += compare('blocks', blocks, drive(driver, 'blocks_contents', blocks), expected)

    # Sheets mutated at random: bytes changed, added, taken out, and cut short.
    alphabet = b'{}[]();:,!@#%.-+\\"\'/*<>~^$|=u0123456789 \n\tabcdefxyz\x00\x80\xc3\xa9\xff'
    for seed in range(1, seeds + 1):
        rng = random.Random(seed)
        cases = []
        for _ in range(3000):
            s = bytearray(rng.choice(sheets))
            if len(s) > 3000:
                at = rng.randrange(len(s) - 3000)
                s = s[at:at + 3000]
            for _ in range(rng.randint(1, 8)):
                op, p = rng.random(), rng.randrange(len(s) + 1)
                if op < 0.4 and s:
                    s[min(p, len(s) - 1)] = rng.choice(alphabet)
                elif op < 0.7:
                    s[p:p] = bytes([rng.choice(alphabet)])
                elif op < 0.9 and s:
                    del s[min(p, len(s) - 1)]
                else:
                    s = s[:p]
            cases.append(bytes(s))
        answers = [json.dumps(json.loads(a)[0]) for a in drive(driver, 'stylesheet_bytes', cases)]
        bad += compare(f'fuzz seed {seed}', cases, answers, [sheet_expectation(c) for c in cases])
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
