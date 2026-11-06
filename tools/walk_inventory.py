#!/usr/bin/env python3
"""Every walk of the node tree, up or down, in the layers above libhtml's
own child lists (SHADOW_DOM.md Appendix A).

Usage: python3 -I tools/walk_inventory.py [--helpers] > inventory.txt

A line is a walk when it steps through `->first_child`, climbs with
`X = X->parent` or a loop whose condition reads `->parent`, or calls a
WALK HELPER: a function of these directories that returns a node pointer
and whose body takes a step along a tree link (`->parent`,
`->first_child`, `->last_child`, `->next`, `->prev`) or calls another walk
helper, found to a fixed point. Each line is printed as
path:line [enclosing function] text. --helpers prints the helper names.

--check DOC prints each enclosing function that DOC's Appendix A does not
name in backticks, and exits 1 when there is one. A name counts wherever
the appendix writes it, so the check proves no walk is unruled, not that
each ruling sits beside the right file; that half is the reader's.
"""
import os, re, sys

DIRS = ['userland/libhtml', 'userland/libdom', 'userland/libpage', 'userland/libgarb',
        'userland/libflow', 'userland/apps/yonder']
NODE_FN = re.compile(r'^(?:static\s+)?(?:inline\s+)?(?:const\s+)?(?:os64_html_node_t|HNode)\s*\*\s*'
                     r'([A-Za-z_][A-Za-z0-9_]*)\s*\(')
FDEF = re.compile(r'^[A-Za-z][^;]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$')
CLIMB = re.compile(r'\b([a-z_][a-z0-9_]*)\s*=\s*\1->parent\b|(?:while|for)\s*\(.*->parent\b')
DESCEND = re.compile(r'->first_child\b')
# A helper's body need only take one step along a tree link: a combinator
# is one step to a parent or a previous sibling. A link written (`->next
# = x`) builds a tree rather than walking one, so it is not a step.
STEP = re.compile(r'->(?:parent|first_child|last_child|next|prev)\b(?!\s*=[^=])')


def sources():
    for d in DIRS:
        for dirpath, _, files in os.walk(d):
            if '/tests' in dirpath:
                continue
            for f in sorted(files):
                if f.endswith(('.c', '.h', '.inc')):
                    yield os.path.join(dirpath, f)


def main():
    paths = sorted(sources())
    # Each node-returning function's body, by brace depth from its header.
    bodies = {}
    for p in paths:
        name, depth, body = None, 0, []
        for line in open(p, errors='replace'):
            if name is None:
                m = NODE_FN.match(line)
                if m and not line.rstrip().endswith(';'):
                    name, depth, body = m.group(1), 0, []
                else:
                    continue
            body.append(line)
            depth += line.count('{') - line.count('}')
            if depth <= 0 and '{' in ''.join(body):
                bodies.setdefault(name, []).extend(body)
                name = None
    helpers = {n for n, b in bodies.items() if any(STEP.search(l) for l in b)}
    while True:
        call = re.compile(r'\b(' + '|'.join(sorted(helpers)) + r')\s*\(')
        more = {n for n, b in bodies.items() if n not in helpers and any(call.search(l) for l in b[1:])}
        if not more:
            break
        helpers |= more
    if '--helpers' in sys.argv:
        print('\n'.join(sorted(helpers)))
        return
    call = re.compile(r'\b(' + '|'.join(sorted(helpers)) + r')\s*\(') if helpers else None
    ruled = None
    if '--check' in sys.argv:
        text = open(sys.argv[sys.argv.index('--check') + 1], encoding='utf-8').read()
        at = text.find('## Appendix A')
        ruled = set(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', ' '.join(re.findall(r'`([^`]*)`', text[at:]))))
        unruled = set()
    for p in paths:
        fn = ''
        for i, line in enumerate(open(p, errors='replace'), 1):
            m = FDEF.match(line)
            if m and not line.startswith(('return', 'if', 'for', 'while', 'else', 'typedef', '#')):
                fn = m.group(1)
            # A comment that names a helper (`element`, `children`) walks
            # nothing, and neither does a helper's prototype.
            if line.lstrip().startswith(('//', '/*', '*')) or (NODE_FN.match(line) and line.rstrip().endswith(';')):
                continue
            if CLIMB.search(line) or DESCEND.search(line) or (call and call.search(line)):
                if ruled is None:
                    print('%s:%d [%s] %s' % (p, i, fn, line.strip()[:140]))
                elif fn not in ruled:
                    unruled.add('%s [%s]' % (p, fn))
    if ruled is not None:
        for u in sorted(unruled):
            print('unruled: ' + u)
        sys.exit(1 if unruled else 0)


if __name__ == '__main__':
    main()
