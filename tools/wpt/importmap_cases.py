#!/usr/bin/env python3
"""WPT's data-driven import map cases (import-maps/data-driven/resources/
*.json, at the runner's pin) as lines tools/test_importmap_host.c reads.

    tools/wpt/importmap_cases.py <wpt checkout> > cases.txt

The directory's own page runs them in an iframe through import.meta.resolve,
neither of which yonder has, so the host test drives yonder's import map
(userland/apps/yonder/importmap.c) with them directly. A leaf of a file's
test tree becomes one case:

    CASE <name>
    MAP <import map base URL>
    ENTRY <scope or \\N for "imports"> <key> <value, or \\N when not a string>
    DUMP <one line of yonder_import_map_dump's text, the expected parse>
    RESOLVE <base URL> <specifier> <expected URL, or \\N for a TypeError>
    END

Fields are tab-separated, with a backslash, a tab and a newline in one
written \\\\, \\t and \\n. A map whose JSON does not parse, or whose shape
"parse an import map string" refuses, is libdom's to report and the DOM
host's to test: such a case is written with ERROR in place of its entries,
and the host test counts it without running it. The entries are walked in
the order JSON.parse gives an object's keys: integer-like keys first,
ascending, then the rest as written.
"""
import json
import pathlib
import sys

def field(text):
    """One field: None is \\N, and nothing else can spell it."""
    if text is None:
        return '\\N'
    return text.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')


def line(*fields):
    return '\t'.join(field(f) for f in fields)


def js_keys(obj):
    """An object's keys in the order JSON.parse's object enumerates them."""
    def index(key):
        if key == '0' or (key.isdigit() and key[0] != '0' and key.isascii()):
            n = int(key)
            if n < 2 ** 32 - 1:
                return n
        return None
    integers = sorted((k for k in obj if index(k) is not None), key=index)
    return integers + [k for k in obj if index(k) is None]


def units(text):
    """Code-unit order, as HTML sorts a map's keys."""
    return text.encode('utf-16-be', 'surrogatepass')


def shape_ok(m):
    if not isinstance(m, dict):
        return False
    for key in ('imports', 'scopes', 'integrity'):
        if key in m and not isinstance(m[key], dict):
            return False
    return all(isinstance(v, dict) for v in m.get('scopes', {}).values())


def entries(m):
    out = []
    for key in js_keys(m.get('imports', {})):
        value = m['imports'][key]
        out.append(line('ENTRY', None, key, value if isinstance(value, str) else None))
    scopes = m.get('scopes', {})
    for scope in js_keys(scopes):
        for key in js_keys(scopes[scope]):
            value = scopes[scope][key]
            out.append(line('ENTRY', scope, key, value if isinstance(value, str) else None))
    return out


def dump(expected):
    out = []
    for key in sorted(expected.get('imports', {}), key=units, reverse=True):
        value = expected['imports'][key]
        out.append(line('DUMP', 'imports', key, value if value is not None else 'null'))
    for scope in sorted(expected.get('scopes', {}), key=units, reverse=True):
        rules = expected['scopes'][scope]
        for key in sorted(rules, key=units, reverse=True):
            value = rules[key]
            out.append(line('DUMP', 'scope', scope, key, value if value is not None else 'null'))
    return out


def walk(node, inherited, name, out):
    """A test object and what it inherits; a leaf is named by its file and
    the test names on the way down to it."""
    here = dict(inherited)
    here.update({k: v for k, v in node.items() if k != 'tests'})
    tests = node.get('tests')
    if tests:
        for test_name, child in tests.items():
            walk(child, here, f'{name}: {test_name}', out)
        return
    out.append(line('CASE', name))
    out.append(line('MAP', here['importMapBaseURL']))
    import_map = here['importMap']
    if isinstance(import_map, str):
        try:
            import_map = json.loads(import_map)
        except ValueError:
            import_map = None
    if import_map is None or not shape_ok(import_map):
        out.append('ERROR')
    else:
        out.extend(entries(import_map))
        if here.get('expectedParsedImportMap') is not None:
            out.extend(dump(here['expectedParsedImportMap']))
        for specifier, expected in here.get('expectedResults', {}).items():
            out.append(line('RESOLVE', here['baseURL'], specifier, expected))
    out.append('END')


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        sys.exit(2)
    resources = pathlib.Path(sys.argv[1]) / 'import-maps' / 'data-driven' / 'resources'
    files = sorted(resources.glob('*.json'))
    if not files:
        print(f'importmap_cases: no data-driven cases in {resources}; run tools/wpt_fetch.sh', file=sys.stderr)
        sys.exit(2)
    out = []
    for path in files:
        walk(json.loads(path.read_text(encoding='utf-8')), {}, path.stem, out)
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
