#!/usr/bin/env python3
"""One deliberate break at a time to custom elements' first cut
(CUSTOM_ELEMENTS.md): libhtml's listener and what each verb and the parser
tell it, the custom state and the name it is read with, libgarb's `:defined`
with the switch on and off, and yonder's census. A mutant is CAUGHT when one
of its harnesses fails, MISSED when they all pass, which means a test is
missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, its harnesses
are run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/custom-elements-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

DOM_C = 'userland/libhtml/dom.c'
TREE_C = 'userland/libhtml/tree.c'
FRAGMENT_C = 'userland/libhtml/fragment.c'
SELECT_C = 'userland/libgarb/select.c'
CASCADE_C = 'userland/libgarb/cascade.c'
COLLECTION_C = 'userland/libdom/collection.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
HTML = 'tools/test_html_dom_host.sh'
GARB = 'tools/test_garb_host.sh'
DOM = 'tools/test_dom_host.sh'
WINDOW = 'tools/test_yonder_scripts_host.sh'

mutants = [
    # ── The listener ────────────────────────────────────────────────────
    ('observe-takes-away', DOM_C, '    d->changed = changed;\n',
     '    d->changed = changed ? changed : d->changed;\n', [HTML]),
    ('record-version', DOM_C, '    c->version = d->version;', '    c->version = 0;', [HTML]),
    ('seen-previous', DOM_C, '    seen->previous = before ? before->prev : parent->last_child;',
     '    seen->previous = before ? before->prev : parent->first_child;', [HTML]),
    ('seen-fragment-count', DOM_C, '            seen->count++;', '            ;', [HTML]),
    ('seen-move', DOM_C, '        seen->from = node->parent;\n', '', [HTML]),
    ('seen-move-siblings', DOM_C, '        seen->from_prev = node->prev;', '        seen->from_prev = NULL;', [HTML]),
    ('drained-record', DOM_C, '            tell(d, &drained);', '            (void)drained;', [HTML]),
    ('replace-record-removed', DOM_C, '.nremoved = replaced != NULL,', '.nremoved = 0,', [HTML]),
    ('placed-next-sibling', DOM_C, '.previous_sibling = seen->previous, .next_sibling = before};',
     '.previous_sibling = seen->previous, .next_sibling = before ? before->next : NULL};', [HTML]),
    ('removed-siblings', DOM_C,
     '    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent, .removed = node,\n'
     '                            .nremoved = 1, .previous_sibling = prev, .next_sibling = next};',
     '    os64_html_change_t c = {.kind = OS64_HTML_CHANGE_CHILDREN, .target = parent, .removed = node,\n'
     '                            .nremoved = 1, .previous_sibling = next, .next_sibling = prev};', [HTML]),
    ('attr-local-name', DOM_C, '            local = c + 1;', '            local = c;', [HTML]),
    ('attr-local-needs-namespace', DOM_C, '    for (const char *c = name; ns && *c; c++)',
     '    for (const char *c = name; *c; c++)', [HTML]),
    # ── What each verb tells, and when ──────────────────────────────────
    ('insert-tells', DOM_C,
     '    d_tell_placed(d, parent, node, before, NULL, &seen);\n    if (node->kind == FRAGMENT)',
     '    if (node->kind == FRAGMENT)', [HTML]),
    ('replace-self-tells', DOM_C,
     '        HSeen seen = {.previous = old->prev};\n        d_tell_placed(d, parent, node, old->next, old, &seen);\n',
     '', [HTML]),
    ('replace-seen-after-old-leaves', DOM_C,
     '    take_out(d, old, NULL);\n'
     '    /* The node leaves its own parent after `old` has gone, so its removal\n'
     '     * names the siblings it had then; the parent\'s record names `old`\'s. */\n'
     '    HSeen seen;\n    d_seen(d, parent, node, before, &seen);\n',
     '    HSeen seen;\n    d_seen(d, parent, node, before, &seen);\n    take_out(d, old, NULL);\n', [HTML]),
    ('replace-previous', DOM_C, '    seen.previous = previous;', '    seen.previous = previous ? NULL : NULL;',
     [HTML]),
    ('remove-tells-before-reclaim', DOM_C,
     '    d_tell_removed(d, parent, node, prev, next);\n    d_detached(d, node);\n    d_collect(d);',
     '    d_detached(d, node);\n    d_collect(d);\n    d_tell_removed(d, parent, node, prev, next);', [HTML]),
    ('seat-html-tells', DOM_C,
     '        landmarks(d, NULL);\n    d_tell_placed(d, &d->root, &d->html, NULL, NULL, &seen);',
     '        landmarks(d, NULL);\n    (void)seen;', [HTML]),
    ('attr-add-tells', DOM_C, '    tell_attr(d, e, fresh->name, fresh->ns, NULL, 0);\n', '', [HTML]),
    ('set-attr-same-value-tells', DOM_C,
     '        tell_attr(d, e, name, existing->ns, existing->value, h_len(existing->value));\n', '', [HTML]),
    ('set-attr-tells-before-retire', DOM_C,
     '    tell_attr(d, e, name, fresh->ns, old ? old->value : NULL, old ? h_len(old->value) : 0);\n'
     '    d_retire(d, old);',
     '    d_retire(d, old);\n'
     '    tell_attr(d, e, name, fresh->ns, old ? old->value : NULL, old ? h_len(old->value) : 0);', [HTML]),
    ('remove-attr-tells-before-retire', DOM_C,
     '    tell_attr(d, e, name, old->ns, old->value, h_len(old->value));\n    d_retire(d, old);',
     '    d_retire(d, old);\n    tell_attr(d, e, name, old->ns, old->value, h_len(old->value));', [HTML]),
    ('batch-remove-clears', DOM_C, '                ns = value = NULL;', '                ns = NULL;', [HTML]),
    ('batch-set-value', DOM_C, '                value = changes[j].value ? changes[j].value : "";',
     '                value = value ? value : "";', [HTML]),
    ('batch-remove-absent-not-told', DOM_C, '        if (!c->remove || value)', '        if (c)', [HTML]),
    ('batch-unchanged-tells', DOM_C,
     '    if (!effective) {\n        tell_attrs(d, e, e->attrs, changes, count);',
     '    if (!effective) {', [HTML]),
    ('batch-tells-before-retire', DOM_C,
     '    tell_attrs(d, e, old, changes, count);\n    if (private)\n        while (old) {\n'
     '            HAttr *next = old->next;\n            d_retire(d, old);\n            old = next;\n        }',
     '    HAttr *had = old;\n    if (private)\n        while (old) {\n'
     '            HAttr *next = old->next;\n            d_retire(d, old);\n            old = next;\n        }\n'
     '    tell_attrs(d, e, had, changes, count);', [HTML]),
    ('set-text-same-tells', DOM_C, '        d_tell_text(d, n, n->text, n->text_len);\n        return OS64_HTML_OK;',
     '        return OS64_HTML_OK;', [HTML]),
    ('set-text-old', DOM_C, '    d_tell_text(d, n, was, was_len);', '    d_tell_text(d, n, was, was_len ? was_len - 1 : 0);', [HTML]),
    ('clone-copies-state', DOM_C, '    n->ns = from->ns;\n    n->tag = from->tag;\n',
     '    n->ns = from->ns;\n    n->tag = from->tag;\n    n->ce = from->ce;\n', [HTML]),
    # ── The parser ──────────────────────────────────────────────────────
    ('parser-attach-tells', TREE_C,
     '        h_attach(d, parent, before, n);\n        d_tell_placed(d, parent, n, before, NULL, &seen);',
     '        h_attach(d, parent, before, n);', [HTML]),
    ('parser-place-tells', TREE_C,
     '    if (verdict == OS64_HTML_OK)\n        d_tell_placed(d, parent, n, before, NULL, &seen);\n', '', [HTML]),
    ('parser-detach-tells', TREE_C, '    d_tell_removed(d, parent, n, prev, next);\n}',
     '    (void)prev;\n    (void)next;\n}', [HTML]),
    ('parser-append-tells', TREE_C,
     '    if (h_text_put(p, n, c))\n        d_tell_text(p->d, n, n->text, had);',
     '    (void)had;\n    h_text_put(p, n, c);', [HTML]),
    ('parser-append-old-length', TREE_C, '        d_tell_text(p->d, n, n->text, had);',
     '        d_tell_text(p->d, n, n->text, had + 1);', [HTML]),
    ('fragment-parse-listens', FRAGMENT_C, '    d->id = owner->id;\n',
     '    d->id = owner->id;\n    d->changed = owner->changed;\n    d->changed_opaque = owner->changed_opaque;\n',
     [HTML]),
    # ── Custom state and the name ───────────────────────────────────────
    ('set-ce-version', DOM_C, '    if (e->ce != ce) {\n        d->version++;', '    if (e->ce != ce) {', [HTML]),
    ('set-ce-element-only', DOM_C,
     '!ours(d, e) || e->kind != ELEMENT || (unsigned)ce > OS64_HTML_CE_FAILED',
     '!ours(d, e) || (unsigned)ce > OS64_HTML_CE_FAILED', [HTML]),
    ('set-ce-range', DOM_C, '(unsigned)ce > OS64_HTML_CE_FAILED)', '(unsigned)ce > 3)', [HTML]),
    ('name-first-letter', DOM_C, "    if (!name || name[0] < 'a' || name[0] > 'z')", "    if (!name || name[0] < 'A')",
     [HTML]),
    ('name-characters', DOM_C, "*c == '-' ||\n              *c == '.' || *c == '_'))",
     "*c == '-' ||\n              *c == '_'))", [HTML]),
    ('name-reserved', DOM_C, '        if (h_eq(name, kReserved[i]))\n            return false;',
     '        if (h_eq(name, kReserved[i]) && i)\n            return false;', [HTML]),
    ('name-needs-hyphen', DOM_C, '    return hyphen;\n}', '    return true;\n}', [HTML]),
    # ── :defined ────────────────────────────────────────────────────────
    ('defined-switch-off', SELECT_C, '        return sel->modern_off || el->ce == OS64_HTML_CE_CUSTOM ||',
     '        return el->ce == OS64_HTML_CE_CUSTOM ||', [GARB]),
    ('defined-custom', SELECT_C, '        return sel->modern_off || el->ce == OS64_HTML_CE_CUSTOM ||',
     '        return sel->modern_off ||', [GARB]),
    ('defined-html-only', SELECT_C, '               !(html(el) && os64_html_custom_name(el->name));',
     '               !os64_html_custom_name(el->name);', [GARB]),
    ('selectors-keep-switch', SELECT_C, '    s->modern_off = modern_off;', '    s->modern_off = modern_off && !modern_off;',
     [GARB]),
    ('cascade-passes-switch', CASCADE_C, 'c->quirks,\n                                                 c->env.modern_off);',
     'c->quirks,\n                                                 false);', [GARB]),
    ('query-passes-switch', COLLECTION_C, '                               !dom->options.modern) : NULL;',
     '                               false) : NULL;', [DOM]),
    # ── yonder's census ─────────────────────────────────────────────────
    ('census-skips-custom', YONDER_C,
     '            if (n->ns == OS64_HTML_NS_HTML && n->ce != OS64_HTML_CE_CUSTOM &&',
     '            if (n->ns == OS64_HTML_NS_HTML &&', [WINDOW]),
    ('census-name-rule', YONDER_C, '                os64_html_custom_name(n->name)) {',
     "                os64_strchr(n->name, '-') != NULL) {", [WINDOW]),
    ('census-failed-line', YONDER_C, '        if (custom[c].failed)\n', '        if (custom[c].failed && !c)\n',
     [WINDOW]),
    ('census-failed-apart', YONDER_C,
     '                while (c < ncustom && !(custom[c].failed == (n->ce == OS64_HTML_CE_FAILED) &&\n'
     '                                        os64_streq(custom[c].name, n->name)))',
     '                while (c < ncustom && !os64_streq(custom[c].name, n->name))', [WINDOW]),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harnesses in mutants:
    if args.only and args.only not in name:
        continue
    path = root / rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    codes = []
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            for harness in harnesses:
                try:
                    result = subprocess.run(['bash', str(root / harness)], cwd=root, stdout=out, stderr=out,
                                            timeout=900)
                    codes.append(result.returncode)
                except subprocess.TimeoutExpired:
                    codes.append('timeout')
                if codes[-1] != 0:
                    break
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
    if not built:
        unbuilt.append(name)
        verdict = 'did not build'
    elif any(code != 0 for code in codes):
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\ncustom elements mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
