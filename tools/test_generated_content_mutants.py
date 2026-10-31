#!/usr/bin/env python3
"""One deliberate break at a time to generated content (slice 8a,
GENERATED_CONTENT.md), each against the suite that holds its rule: a
mutant is CAUGHT when the suite fails, MISSED when it passes, which means a
test is missing and gets written. A build failure is neither.

The rules: a rule's ::before and ::after selectors are filed for their
pseudo-element, each apart, and matched against the originating element,
once per target; a pseudo-element takes no inline style; `content` reads
to its grammar and names what it does not draw; a pseudo-element exists
only with content, never on a replaced element; its content's attr() is
the element's attribute; it is walked first and last among the children;
an author's content replaces q's own mark; and a public box names its
originating element, which then measures as its own box alone.

The shape is test_bytecode_mutants.py's: each mutant is applied to the
worktree's copy of its file, the suite is run, and the file is put back
from the bytes read before the change, checked byte for byte, whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/generated-content-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

CASCADE = 'userland/libgarb/cascade.c'
PROPS = 'userland/libgarb/props.c'
STYLE = 'userland/libflow/style.c'
BOXES = 'userland/libflow/boxes.c'
FLOW = 'userland/libflow/flow.c'
GEOMETRY = 'userland/apps/yonder/geometry.c'
GARB = 'bash tools/test_garb_host.sh'
LIBFLOW = 'bash tools/test_libflow_host.sh'
WINDOW = 'bash tools/test_yonder_scripts_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states.
mutants = [
    # libgarb: filing and the cascade (§ 3).
    ('a-before-rule-styles-its-element', CASCADE, '            file_selector(c, index, s, TARGET_BEFORE);',
     '            file_selector(c, index, s, TARGET_ELEMENT);', GARB),
    ('an-after-rule-is-filed-as-before', CASCADE, '            file_selector(c, index, s, TARGET_AFTER);',
     '            file_selector(c, index, s, TARGET_BEFORE);', GARB),
    ('a-rule-is-counted-once-across-targets', CASCADE, '    if (r->seen == el && r->seen_target == target) {',
     '    if (r->seen == el) {', GARB),
    ('a-pseudo-element-takes-inline-style', CASCADE, '    int32_t ninl = own ? inline_decls(c, el, &inl) : 0;',
     '    int32_t ninl = inline_decls(c, el, &inl);', GARB),
    ('no-pseudo-element-is-cascaded', CASCADE,
     '    return ix->ids.n > 0 || ix->classes.n > 0 || ix->types.n > 0 || ix->universal >= 0;',
     '    return ix == NULL;', GARB),
    # libgarb: content's grammar.
    ('a-slash-needs-its-alternative', PROPS, '        if (nalt == 0)\n            return false;\n', '', GARB),
    ('an-undrawn-picture-is-not-named', PROPS,
     '            if (it->kind == GARB_V_URL)\n                return "url";',
     '            if (it->kind == GARB_V_URL)\n                return NULL;', GARB),
    # libflow pass 1 (§ 4).
    ('content-none-makes-a-pseudo-element', STYLE,
     '    if (content == NULL || content->kind == GARB_V_WIDE || content->kind == GARB_V_KEYWORD)\n        return true;',
     '    if (content == NULL)\n        return true;', LIBFLOW),
    ('a-replaced-element-has-pseudo-elements', STYLE, '    return el->ns == OS64_HTML_NS_HTML && !f_replaced(c->env, el);',
     '    return el->ns == OS64_HTML_NS_HTML && (c != NULL || !f_replaced(c->env, el));', LIBFLOW),
    ('attr-is-case-sensitive', STYLE, "name[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;",
     'name[i] = ch;', LIBFLOW),
    # libflow pass 2.
    ('before-is-not-first', BOXES, '    if (s != NULL && s->pseudo[0] != NULL)\n        return s->pseudo[0];',
     '    if (s != NULL && s->pseudo[0] != NULL && false)\n        return s->pseudo[0];', LIBFLOW),
    ('after-is-not-last', BOXES, '    return c->next != NULL ? c->next : after_of(b, c->parent);', '    return c->next;', LIBFLOW),
    ('a-mark-drawn-over-the-authors', BOXES, '    if (before != NULL && !(s->pseudo_declared & 1))',
     '    if (before != NULL)', LIBFLOW),
    # The boundary.
    ('a-synthetic-node-leaves-libflow', FLOW,
     '    if (pseudo != GARB_PSEUDO_NONE)\n        node = f_pseudo_origin(node);\n', '', LIBFLOW),
    ('an-element-measures-its-pseudo-boxes', GEOMETRY, '(box->node != node && !caption) || box->pseudo != 0 || box->kind',
     '(box->node != node && !caption) || box->kind', WINDOW),
    ('the-roots-before-is-not-walked', BOXES, 'build_container(&b, out->root, kids(&b, html),',
     'build_container(&b, out->root, html->first_child,', LIBFLOW),
    ('an-atomic-pseudo-takes-the-underline', STYLE, '    if (!decoration_edge(node, &sp.s, c->quirks))\n        f_decoration_inherit(&out->decoration',
     '    if (true)\n        f_decoration_inherit(&out->decoration', LIBFLOW),
    ('a-pseudo-border-has-no-medium', STYLE, '    medium_widths(c, &sp, st);\n    if (sp.s.display == FLOW_DISPLAY_NONE)',
     '    if (sp.s.display == FLOW_DISPLAY_NONE)', LIBFLOW),
    ('a-pseudo-takes-from-a-ticking-parent', STYLE, '    taken_from(of, &sp.taken);\n', '', LIBFLOW),
    ('content-is-not-charged', STYLE, 'out = list_alloc(c, total + 1, &spent);', 'out = f_arena_alloc(&c->out->arena, total + 1);', LIBFLOW),
    ('a-pseudo-clip-takes-its-elements-text', FLOW, '(box->pseudo != 0 && c->pseudo != box->pseudo) ||', '', LIBFLOW),
    ('a-pseudo-box-skips-its-element-for-motion', FLOW, '        up = b->node;\n', '', LIBFLOW),
    ('a-pseudo-box-takes-by-its-elements-record', FLOW, 'own = gen != NULL ? f_style_of(tree->styles, gen) : NULL;',
     'own = gen != NULL ? own : NULL;', LIBFLOW),
    ('a-pseudo-box-is-styled-by-its-element', FLOW, '    const os64_html_node_t *e = styled_by;', '    const os64_html_node_t *e = styled_by != NULL ? node : NULL;', LIBFLOW),
    ('a-pseudo-group-ticks-for-its-element', FLOW, 'l->box->node != NULL && l->box->pseudo == GARB_PSEUDO_NONE\n', 'l->box->node != NULL\n', LIBFLOW),
    ('a-pseudo-group-is-its-elements-part', FLOW, 'l->box->pseudo != GARB_PSEUDO_NONE ||\n', '\n', LIBFLOW),
    # Slice 8b (GENERATED_CONTENT.md § 8.2): counters and quotes in tree order.
    ('increment-before-reset', STYLE,
     'kOrder[] = {GARB_COUNTER_RESET, GARB_COUNTER_INCREMENT, GARB_COUNTER_SET};',
     'kOrder[] = {GARB_COUNTER_INCREMENT, GARB_COUNTER_RESET, GARB_COUNTER_SET};', LIBFLOW),
    ('set-before-increment', STYLE,
     'kOrder[] = {GARB_COUNTER_RESET, GARB_COUNTER_INCREMENT, GARB_COUNTER_SET};',
     'kOrder[] = {GARB_COUNTER_RESET, GARB_COUNTER_SET, GARB_COUNTER_INCREMENT};', LIBFLOW),
    ('siblings-nest', STYLE, '    if (in != NULL && in->scope == scope) {', '    if (in != NULL && in->scope == scope && false) {', LIBFLOW),
    ('a-scope-never-ends', STYLE, '        c->ncounters--;\n', '        break;\n', LIBFLOW),
    ('an-increment-makes-no-counter', STYLE, '            if (f == NULL)\n                f = instantiate(c, name, 0, scope);\n', '', LIBFLOW),
    ('display-none-counts', STYLE, '    if (of->style.display == FLOW_DISPLAY_NONE)\n        return true;\n    if (c->env->cascade != NULL) {',
     '    if (c->env->cascade != NULL) {', LIBFLOW),
    ('a-pseudo-elements-own-counters-ignored', STYLE, '    count_on(c, st, el, el);\n', '', LIBFLOW),
    ('inherit-takes-nothing', STYLE, 'os64_streq(v->keyword, "inherit") && from != NULL', 'false && from != NULL', LIBFLOW),
    ('counters-is-the-innermost-alone', STYLE, '    if (n <= 1) {', '    if (n <= 1 || true) {', LIBFLOW),
    ('the-style-is-ignored', STYLE, '!list_type_named(&it->items[it->nitems - 1], &type)', 'true', LIBFLOW),
    ('a-close-at-depth-0-draws', STYLE, '        if (c->quote_depth == 0)\n            return;\n', '', LIBFLOW),
    ('pairs-wrap-past-the-last', STYLE, '2 * (depth < pairs ? depth : pairs - 1)', '2 * (depth % pairs)', LIBFLOW),
    ('no-open-quote-draws', STYLE, 'bool drawn = os64_streq(it->keyword, "open-quote") || os64_streq(it->keyword, "close-quote");',
     'bool drawn = it->keyword != NULL;', LIBFLOW),
    ('a-counter-pass-is-not-ordered', STYLE, '    c->out->ordered = true;\n    FCounter *in = innermost(c, name);', '    FCounter *in = innermost(c, name);', LIBFLOW),
]

caught, missed, unbuilt = [], [], []
for name, rel, old, new, harness in mutants:
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
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', harness], cwd=root, stdout=out, stderr=out, timeout=1800)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
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
    elif code != 0:
        caught.append(name)
        verdict = 'caught'
    else:
        missed.append(name)
        verdict = 'MISSED'
    print(f'{name}: {verdict}', flush=True)

print(f'\ngenerated content mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
