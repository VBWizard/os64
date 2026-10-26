#!/usr/bin/env python3
"""Independent catalog selection and acceptance gate; see README.md."""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import tempfile
from urllib.parse import urljoin, urlparse, unquote
from xml.parsers import expat
from fetch import ARCHIVE, ROOT, SHA256, verify


def catalog(root):
    root = root.resolve()
    rows = []
    bases = [root.as_uri() + '/']

    def local(uri):
        u = urlparse(uri)
        if u.scheme != 'file' or u.netloc:
            raise ValueError(f'nonlocal catalog URI: {uri}')
        path = Path(unquote(u.path)).resolve()
        path.relative_to(root)
        return path

    def configure(parser, path):
        parser.SetBase(path.as_uri())
        parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_ALWAYS)

        def start(tag, attrs):
            base = urljoin(bases[-1], attrs.get('xml:base', ''))
            bases.append(base)
            if tag == 'TEST':
                uri = local(urljoin(base, attrs['URI']))
                if not uri.is_file():
                    raise ValueError(f'missing test: {uri}')
                row = dict(attrs)
                row['path'] = str(uri.relative_to(root))
                rows.append(row)

        def external(context, base, system, public):
            target = local(urljoin(base, system))
            child = parser.ExternalEntityParserCreate(context)
            configure(child, target)
            # Elements in an external parsed entity use that entity's base.
            bases.append(target.as_uri())
            with target.open('rb') as source:
                child.ParseFile(source)
            bases.pop()
            return 1

        parser.StartElementHandler = start
        parser.EndElementHandler = lambda tag: bases.pop()
        parser.ExternalEntityRefHandler = external

    parser = expat.ParserCreate()
    configure(parser, root / 'xmlconf.xml')
    with (root / 'xmlconf.xml').open('rb') as source:
        parser.ParseFile(source)
    if len({r['ID'] for r in rows}) != len(rows):
        raise ValueError('duplicate catalog IDs')
    return rows


def selection(row):
    if row.get('VERSION') and '1.0' not in row['VERSION'].split():
        return 'XML version'
    if row.get('EDITION') and '5' not in row['EDITION'].split():
        return 'XML edition'
    rec = row.get('RECOMMENDATION', 'XML1.0')
    if rec.startswith(('XML1.1', 'NS1.1')):
        return 'XML/namespace 1.1 recommendation'
    if row.get('NAMESPACE', 'yes') == 'no':
        return 'requires namespace-disabled parser'
    if row.get('ENTITIES', 'none') != 'none':
        return 'requires external entities: ' + row['ENTITIES']
    if row['TYPE'] == 'error':
        return 'optional error behavior'
    return ''


def verdict(expected, actual):
    if actual in ('UNSUPPORTED', 'UNSUPPORTED_ENCODING'):
        return 'unsupported'
    if actual == 'DONE':
        return 'unexpected-accept' if expected == 'not-wf' else 'accepted-' + expected
    if actual == 'MALFORMED':
        return 'rejected-not-wf' if expected == 'not-wf' else 'unexpected-reject'
    return 'unexpected-failure'


def run(driver, paths):
    result = subprocess.run([str(driver), '--batch'], input=''.join(str(p) + '\n' for p in paths), text=True, capture_output=True, check=True, timeout=90)
    if result.stderr:
        print(result.stderr, end='')
    statuses = result.stdout.splitlines()
    allowed = {'DONE', 'MALFORMED', 'UNSUPPORTED', 'UNSUPPORTED_ENCODING', 'LIMIT', 'NO_MEMORY', 'CANCELLED', 'CONSUMER_ABORT', 'BAD_ARGUMENT'}
    if len(statuses) != len(paths) or any(s not in allowed for s in statuses):
        raise ValueError('driver protocol failure')
    return statuses


def controls(driver):
    vectors = json.loads((Path(__file__).parent / 'controls.json').read_text())
    if vectors['version'] != 1:
        raise ValueError('unknown control-vector version')
    samples = [(v['input'], v['expected'], v['actual']) for v in vectors['cases']]
    with tempfile.TemporaryDirectory() as directory:
        paths = []
        for i, (text, _, _) in enumerate(samples):
            path = Path(directory) / f'{i}.xml'
            path.write_text(text)
            paths.append(path)
        actual = run(driver, paths)
    if actual != [s[2] for s in samples]:
        raise ValueError(f'control verdicts: {actual}')
    # Deliberately broken adapters must not satisfy the gate's oracle.
    for wrong in ('DONE', 'MALFORMED'):
        if not any(verdict(s[1], wrong).startswith('unexpected') for s in samples):
            raise ValueError('gate accepted a broken adapter')
    if verdict('not-wf', 'UNSUPPORTED') != 'unsupported' or verdict('invalid', 'DONE') != 'accepted-invalid':
        raise ValueError('profile/validity verdict controls failed')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--driver', type=Path, required=True)
    ap.add_argument('--report', type=Path, required=True)
    ap.add_argument('--require-complete', action='store_true', help='also fail on unsupported selected cases')
    args = ap.parse_args()
    verify()
    # Re-extract from the checked archive: modified working corpus bytes must
    # not silently inherit the pinned digest in a report.
    import tarfile
    with tarfile.open(ARCHIVE) as archive:
        archive.extractall(ROOT / '.vendor', filter='data')
    root = ROOT / '.vendor/xmlconf'
    controls(args.driver.resolve())
    cases = []
    for row in catalog(root):
        reason = selection(row)
        cases.append(dict(id=row['ID'], path=row['path'], expected=row['TYPE'],
                          selection='excluded' if reason else 'selected', reason=reason,
                          actual=None, verdict='excluded' if reason else None))
    selected = [c for c in cases if c['selection'] == 'selected']
    results = run(args.driver.resolve(), [root / c['path'] for c in selected])
    for case, actual in zip(selected, results):
        case['actual'] = actual
        case['verdict'] = verdict(case['expected'], actual)
        if case['verdict'] == 'unsupported':
            case['reason'] = 'parser refused a profile/encoding feature; not counted as a pass'
    required = json.loads((Path(__file__).parent / 'required.json').read_text())
    ids = required['required']
    if required['version'] != 1 or ids != sorted(set(ids)):
        raise ValueError('required-case format or duplicate ID')
    by_id = {c['id']: c for c in cases}
    regressions = [i for i in ids if i not in by_id or by_id[i]['verdict'] not in
                   ('accepted-valid', 'accepted-invalid', 'rejected-not-wf')]
    counts = dict(sorted(Counter(c['verdict'] for c in cases).items()))
    args.report.write_text(json.dumps(dict(version=1, archive_sha256=SHA256, cases=cases, counts=counts, required_regressions=regressions), indent=2) + '\n')
    print('xmlconf:', json.dumps(counts, sort_keys=True))
    unexpected = [c for c in cases if c['verdict'].startswith('unexpected')]
    for case in unexpected[:30]:
        print(case['verdict'], case['id'], case['path'], case['actual'])
    if regressions:
        print('required-case regressions:', ', '.join(regressions[:30]))
    return bool(unexpected or regressions or (args.require_complete and counts.get('unsupported', 0)))


if __name__ == '__main__':
    raise SystemExit(main())
