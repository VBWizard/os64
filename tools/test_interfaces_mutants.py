#!/usr/bin/env python3
"""One deliberate break at a time to 0f's P1a (INTERFACES_FORMS.md): the
prototype kinds and their interface objects, the Text and Comment
constructors, the node row (getRootNode, wholeText, splitText), libpage's
option selectedness and list walk, and the select row (the option's
members, the select's, the options collection and its indexed setter, the
Option factory) - against the harnesses that hold each rule: a mutant is
CAUGHT when one of its harnesses fails, MISSED when they all pass, which
means a test is missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, its harnesses
are run, and the file is put back from the bytes read before the change -
checked byte for byte - whatever happens. Nothing else is touched. Run it
from a clean tree, one run at a time, or through tools/mutants_parallel.py.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/interfaces-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

NODE_C = 'userland/libdom/node.c'
CLASSIC_C = 'userland/libdom/classic.c'
MIXIN_C = 'userland/libdom/mixin.c'
OPTIONS_C = 'userland/libdom/options.c'
COLLECTION_C = 'userland/libdom/collection.c'
STATE_C = 'userland/libpage/state.c'
CORE_C = 'userland/libpage/core.c'
DOM = 'tools/test_dom_host.sh'
PAGE = 'tools/test_libpage_host.sh'

# (name, file, old, new, harnesses). Each breaks one rule the phase
# states; none supplies a replacement expectation.
mutants = [
    # 1. the prototype kinds and their interface objects
    ('text-shares-characterdata', NODE_C, '    if (node->kind == OS64_HTML_TEXT) return D_PROTO_TEXT;',
     '    if (node->kind == OS64_HTML_TEXT) return D_PROTO_CHARACTER_DATA;', (DOM,)),
    ('svg-is-element', NODE_C, '    if (node->ns == OS64_HTML_NS_SVG) return D_PROTO_SVG;',
     '    if (node->ns == OS64_HTML_NS_SVG) return D_PROTO_ELEMENT;', (DOM,)),
    ('option-shares-htmlelement', NODE_C, '        if (node->tag == OS64_HTML_TAG_OPTION) return D_PROTO_OPTION;',
     '        if (node->tag == OS64_HTML_TAG_OPTION) return D_PROTO_HTML_ELEMENT;', (DOM,)),
    ('text-inherits-node', NODE_C, '    case D_PROTO_TEXT: case D_PROTO_COMMENT: return D_PROTO_CHARACTER_DATA;',
     '    case D_PROTO_TEXT: case D_PROTO_COMMENT: return D_PROTO_NODE;', (DOM,)),
    ('text-not-constructed', CLASSIC_C, '            : proto == D_PROTO_TEXT || proto == D_PROTO_COMMENT',
     '            : proto == D_PROTO_COMMENT', (DOM,)),
    ('constructor-drops-data', CLASSIC_C,
     '    if (argc > 0 && !JS_IsUndefined(argv[0]) && !d_string(dom, ctx, argv[0], &data)) {',
     '    if (argc > 9 && !JS_IsUndefined(argv[0]) && !d_string(dom, ctx, argv[0], &data)) {', (DOM,)),
    # 2. the node row
    ('root-is-self', MIXIN_C,
     '        if (root == NULL) return JS_EXCEPTION;\n        while (root->parent != NULL) root = root->parent;',
     '        if (root == NULL) return JS_EXCEPTION;', (DOM,)),
    ('root-options-unread', MIXIN_C, '    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return 0;',
     '    if (argc < 9 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return 0;', (DOM,)),
    ('split-keeps-data', MIXIN_C,
     '    status = os64_page_node_set_text(dom->state, (os64_html_node_t *)self, kept, byte);',
     '    status = os64_page_node_set_text(dom->state, (os64_html_node_t *)self, kept, kept_length);', (DOM,)),
    ('offset-counts-bytes', MIXIN_C, '        units += width == 4 ? 2 : 1;', '        units += (uint32_t)width;', (DOM,)),
    ('whole-text-from-self', MIXIN_C,
     '        while (first->prev != NULL && first->prev->kind == OS64_HTML_TEXT) first = first->prev;',
     '        while (first->prev != NULL && first->prev->kind == OS64_HTML_COMMENT) first = first->prev;', (DOM,)),
    # 3. libpage's option selectedness and list walk
    ('option-off-no-reset', STATE_C, '        p_state_find(state, select)->selection_set = false;',
     '        p_state_find(state, select)->selection_set = true;', (PAGE, DOM)),
    ('single-option-own', STATE_C, '    if (single && on)\n        return select_assign',
     '    if (single && on && option == NULL)\n        return select_assign', (PAGE, DOM)),
    ('option-read-own', STATE_C,
     '    if (select == NULL || p_has_attr(select, "multiple")) {\n        *on = option_selected(state, option);',
     '    if (select != NULL || p_has_attr(option, "multiple")) {\n        *on = option_selected(state, option);',
     (PAGE, DOM)),
    ('option-select-any', CORE_C, '                if (o == option)\n                    return at;\n            return NULL;',
     '                if (o == option)\n                    return at;\n            return at;', (PAGE,)),
    # 4. the select row
    ('value-is-text', OPTIONS_C,
     '        return attr != NULL ? JS_NewString(ctx, attr->value) : option_text(dom, ctx, self);',
     '        return attr == NULL ? JS_NewString(ctx, "") : option_text(dom, ctx, self);', (DOM,)),
    ('text-keeps-space', OPTIONS_C, '            if (ascii_space(n->text[i])) { space = used != 0; continue; }',
     '            if (ascii_space(n->text[i]) && n == NULL) { space = used != 0; continue; }', (DOM,)),
    ('text-reads-scripts', OPTIONS_C, '    if (!script && at->first_child != NULL) return at->first_child;',
     '    if ((script || !script) && at->first_child != NULL) return at->first_child;', (DOM,)),
    ('selected-options-stale', COLLECTION_C,
     '    if (query->version == version && query->state_version == state_version) return true;',
     '    if (query->version == version && (query->state_version == state_version || true)) return true;', (DOM,)),
    ('index-setter-refused', COLLECTION_C,
     '    if (numeric != 0 && query->kind == D_QUERY_OPTIONS && (flags & JS_PROP_HAS_VALUE) != 0 &&',
     '    if (numeric != 0 && query->kind == D_QUERY_SELECTED_OPTIONS && (flags & JS_PROP_HAS_VALUE) != 0 &&', (DOM,)),
    ('add-ignores-index', OPTIONS_C, '    if (indexed) before = index >= 0 ? option_at(select, (uint32_t)index) : NULL;',
     '    if (indexed) before = index >= 0 ? NULL : NULL;', (DOM,)),
    ('remove-without-index-removes-option', OPTIONS_C, '        if (argc == 0) {\n            if (select->parent == NULL)',
     '        if (argc == 99) {\n            if (select->parent == NULL)', (DOM,)),
    ('length-never-appends', OPTIONS_C, '    if (wanted - count > OPTIONS_APPEND_MAX) return JS_UNDEFINED;',
     '    if (wanted - count > 0) return JS_UNDEFINED;', (DOM,)),
    ('factory-ignores-selected', OPTIONS_C,
     '                int64_t page = os64_page_node_init_option_selected(dom->state, option, selected != 0);',
     '                int64_t page = os64_page_node_init_option_selected(dom->state, option, selected != 0 && false);',
     (DOM,)),
    ('factory-option-dirty', STATE_C, '    edit->selected_dirty = false;\n    p_state_publish(state);\n    return 0;',
     '    edit->selected_dirty = true;\n    p_state_publish(state);\n    return 0;', (PAGE, DOM)),
    ('assign-dirties-siblings', STATE_C, '        if (i == index)\n            edit->selected_dirty = true;',
     '        edit->selected_dirty = true;', (PAGE, DOM)),
    ('assign-sibling-snapshot-stale', STATE_C,
     '        edit->selected = i == index ? 1 : 0;\n        edit->selected_attr = p_has_attr(at, "selected");',
     '        edit->selected = i == index ? 1 : 0;', (PAGE, DOM)),
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

print(f'\ninterfaces mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
