#!/usr/bin/env python3
"""Read upstream HTML fixtures without rewriting their inputs or expectations."""
import hashlib
import json
import re
from pathlib import Path
ROOT = Path(__file__).resolve().parent / 'html5lib-tests'

def unescape(value):
    if isinstance(value, str):
        return re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m[1], 16)), value)
    if isinstance(value, list):
        return [unescape(x) for x in value]
    if isinstance(value, dict):
        return {unescape(k): unescape(v) for k, v in value.items()}
    return value

def cases():
    for path in sorted((ROOT / 'tokenizer').glob('*.test')):
        groups = json.loads(path.read_text())
        for group, tests in groups.items():
            for ordinal, test in enumerate(tests):
                if test.get('doubleEscaped'):
                    test = unescape(test)
                for state in test.get('initialStates', ['Data state']):
                    yield {'suite': 'tokenizer', 'file': str(path.relative_to(ROOT)),
                           'ordinal': ordinal, 'state': state, 'group': group,
                           'input': test['input'], 'expected': test['output'],
                           'lastStartTag': test.get('lastStartTag', ''),
                           'errors': test.get('errors', [])}
    # tree-construction/scripted/ is where a fixture's scripts RUN; everywhere
    # else a parser that runs scripts is expected to leave them unrun.
    for path in sorted((ROOT / 'tree').glob('*.dat')) + sorted((ROOT / 'tree' / 'scripted').glob('*.dat')):
        raw = path.read_bytes().decode('utf-8')
        for ordinal, record in enumerate(re.split(r'^#data\n', raw, flags=re.M)[1:]):
            parts = re.split(r'^#([^\n]+)\n', record, flags=re.M)
            fields = {'data': parts[0].removesuffix('\n')}
            for i in range(1, len(parts), 2):
                fields[parts[i]] = parts[i + 1].removesuffix('\n')
            yield {'suite': 'tree', 'file': str(path.relative_to(ROOT)),
                   'ordinal': ordinal, 'input': fields['data'],
                   'expected': fields.get('document', '').rstrip('\n'),
                   'fragment': fields.get('document-fragment'),
                   'script': ('on' if 'script-on' in fields else
                              'off' if 'script-off' in fields else 'both'),
                   'runs': path.parent.name == 'scripted',
                   'errors': fields.get('errors', '')}

# The one script a driver interprets (DOM_D9.md): document.write of string
# literals, joined by `+`. What a tree-construction case scripts is this or
# DOM verbs, and only this needs nothing but the parser.
LITERAL = r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\''
WRITE = re.compile(r'\s*document\.write\(\s*(?:' + LITERAL + r')(?:\s*\+\s*(?:' + LITERAL + r'))*\s*\)\s*;?\s*')

def script_texts(tree):
    """The text of every script element in an expected tree, written ones included."""
    lines = tree.split('\n')
    texts = []
    for i, line in enumerate(lines):
        if line.lstrip('| ') != '<script>':
            continue
        depth = len(line) - len(line.lstrip('| ')) + 2
        if i + 1 < len(lines) and lines[i + 1][:depth].strip(' |') == '' and lines[i + 1][depth:].startswith('"'):
            text = lines[i + 1][depth + 1:]
            j = i + 1
            while not text.endswith('"') and j + 1 < len(lines):
                j += 1
                text += '\n' + lines[j]
            texts.append(text[:-1])
        else:
            texts.append('')
    return texts

def unrunnable(case):
    """Why a case whose scripts run cannot be run by a parser alone, or None."""
    if not case.get('runs'):
        return None
    if any(not WRITE.fullmatch(t) for t in script_texts(case['expected'])):
        return 'script-not-document-write'
    return None

def scripting_modes(case):
    """A tree fixture holds with scripting off and on unless it is marked for one."""
    if case['suite'] != 'tree':
        return [False]
    return {'on': [True], 'off': [False], 'both': [False, True]}[case['script']]

def identity(case):
    # Mode and ordinal distinguish fixtures that share an input but exercise
    # different tokenizer states or scripting/fragment contexts.
    digest = hashlib.sha256(case['input'].encode('utf-8', 'surrogatepass')).hexdigest()
    return f"{case['file']}:{case['ordinal']}:{case.get('state', '')}:{digest}"

def inventory():
    files = {}
    skips = []
    for case in cases():
        counts = files.setdefault(case['file'], {'total': 0, 'run': 0, 'skip': {}})
        counts['total'] += 1
        reason = 'xml-output-coercion' if case.get('group') == 'xmlViolationTests' else unrunnable(case)
        if reason:
            counts['skip'][reason] = counts['skip'].get(reason, 0) + 1
            skips.append(identity(case) + '\t' + reason)
        else:
            counts['run'] += 1
    return files, skips

if __name__ == '__main__':
    files, skips = inventory()
    (ROOT / 'INVENTORY.json').write_text(json.dumps(files, indent=2, sort_keys=True) + '\n')
    (ROOT / 'SKIPS.tsv').write_text('\n'.join(skips) + '\n')
    print('cases:', sum(v['total'] for v in files.values()),
          'run:', sum(v['run'] for v in files.values()), 'skipped:', len(skips))
