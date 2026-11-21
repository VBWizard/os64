#!/usr/bin/env python3
"""One deliberate break at a time to N2, a read that asks for what it reads
(BRIEF_LAYOUT_NEEDS.md N2): libhtml's merged change log (dom.c, log_merge),
libgarb's inline diff and its style-attribute switch (cascade.c, select.c),
libflow's pending verdict and its table (pending.inc), libdom's need
(geometry.c, view.c) and yonder's reads and covered windows (yonder.c),
each against the suites that can see it: a mutant is CAUGHT when they fail,
MISSED when they pass, which means a test is missing and gets written. A
break that changes nothing a rule promises is not in the list; the comment
after it says which and why. A build failure is neither.

The shape is test_restyle_mutants.py's: each mutant is applied to the tree's
copy of its file, its suites are run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run
it from a clean tree, and never beside another harness that edits the same
tree; the per-mutant logs are in --logs.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/pending-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

DOM_C = 'userland/libhtml/dom.c'
CASCADE = 'userland/libgarb/cascade.c'
SELECT = 'userland/libgarb/select.c'
PENDING = 'userland/libflow/pending.inc'
RESTYLE = 'userland/libflow/restyle.inc'
GEOMETRY = 'userland/libdom/geometry.c'
YONDER = 'userland/apps/yonder/yonder.c'

# The suites, by what they see.
HTML = 'bash tools/test_html_dom_host.sh'
GARB = 'bash tools/test_garb_host.sh'
FLOW = 'bash tools/test_libflow_host.sh --restyle'
DOM = 'bash tools/test_dom_host.sh --scroll'
SCRIPTS = 'bash tools/test_yonder_scripts_host.sh'

mutants = [
    # The log merged (libhtml).
    ('no-merge', DOM_C, '    log_merge(d, c);', '    if (0) log_merge(d, c);', [HTML]),
    ('merge-past-children', DOM_C, '        d->log_barrier = copy;\n        log_index_clear(d);', '        d->log_barrier = copy;',
     [HTML]),
    ('merge-across-attributes', DOM_C, '    if (c->kind == OS64_HTML_CHANGE_TEXT) return true;', '    return true;', [HTML]),
    ('merged-entry-left-in-the-index', DOM_C, '    if (!at) return;\n    log_index_drop(d, at);', '    if (!at) return;', [HTML]),
    # What an inline write changed (libgarb).
    ('importance-not-compared', CASCADE,
     '    return a->len == b->len && os64_memcmp(a->name, b->name, a->len) == 0 && a->important == b->important &&',
     '    return a->len == b->len && os64_memcmp(a->name, b->name, a->len) == 0 &&', [GARB]),
    ('children-not-compared', CASCADE, '            !values_same(x->children, x->nchildren, y->children, y->nchildren))',
     '            0)', [GARB]),
    ('customs-not-asked', CASCADE, '    if (!customs_same || new_custom != old_custom || matched_custom != old_custom)',
     '    if (0)', [GARB]),
    ('baseline-not-kept', CASCADE, '        slot->inl = inl;', '        (void)inl;', [GARB]),
    ('style-attribute-not-seen', SELECT, '                    out->style_attribute = true;', '                    (void)0;',
     [GARB, SCRIPTS]),
    # The verdict (libflow).
    ('the-table-admits-padding', PENDING, '    [GARB_FILL] = PENDING_PLAIN,',
     '    [GARB_FILL] = PENDING_PLAIN,\n    [GARB_PADDING_TOP] = PENDING_PLAIN,', [FLOW, SCRIPTS]),
    ('transform-side-not-judged', PENDING,
     '            if (wide || (none ? f_transformed(s) : f_transformed(s) != (s->display != FLOW_DISPLAY_INLINE)))',
     '            if (wide)', [FLOW]),
    ('filter-side-not-judged', PENDING,
     '            if (wide || s->display == FLOW_DISPLAY_INLINE ||\n                (none ? f_filtered(s, false) : !f_filtered(s, false)))',
     '            if (wide)', [FLOW]),
    ('css-wide-keywords-admitted', PENDING, '        bool wide = os64_streq(c->keyword, "initial") ||',
     '        bool wide = false && os64_streq(c->keyword, "initial") ||', [FLOW]),
    ('origin-says-nothing', PENDING, '        case PENDING_ORIGIN:\n            tree->pending_transforms = true;',
     '        case PENDING_ORIGIN:\n            (void)0;', [FLOW]),
    ('collapse-admitted', PENDING, '            if (s->visibility == FLOW_COLLAPSE ||', '            if (0 &&', [FLOW]),
    ('removal-and-var-admitted', PENDING, '        if (kind == 0 || !c->declared || c->var)', '        if (kind == 0)', [FLOW]),
    ('motion-admitted', PENDING, '    if (rec == NULL || rec->motion != NULL)', '    if (rec == NULL)', [FLOW]),
    ('cascade-version-not-asked', PENDING,
     '        garb_cascade_version(cascade) != tree->pending_version || !garb_cascade_tracks_changes(cascade) ||',
     '        !garb_cascade_tracks_changes(cascade) ||', [FLOW]),
    ('style-attribute-switch-not-asked', PENDING, '        garb_cascade_reads_style_attribute(cascade))', '        0)',
     [FLOW, SCRIPTS]),
    ('verdict-falls-back', PENDING, '            if (v > tree->pending_verdict)', '            if (1)', [FLOW]),
    ('transforms-not-said', PENDING, '            tree->pending_transforms = true;\n            if (wide ||',
     '            (void)0;\n            if (wide ||', [FLOW, SCRIPTS]),
    ('not-current-after-a-kept-restyle', RESTYLE, '        pending_open(tree);', '        (void)0;', [FLOW]),
    # What a read takes (libdom) and who answers it (yonder).
    ('scroll-reads-ask-for-boxes', GEOMETRY,
     '                                  : magic == G_SCROLL_LEFT || magic == G_SCROLL_TOP ? OS64_DOM_NEED_REACH',
     '                                  : magic == G_SCROLL_LEFT || magic == G_SCROLL_TOP ? OS64_DOM_NEED_BOXES', [DOM]),
    ('the-need-not-passed', GEOMETRY, '    out->need = need;', '    (void)need;', [DOM, SCRIPTS]),
    ('a-reach-read-kept-with-a-transform', YONDER,
     '(need == OS64_DOM_NEED_BOXES || (need == OS64_DOM_NEED_REACH && !pending.transforms));',
     '(need == OS64_DOM_NEED_BOXES || need == OS64_DOM_NEED_REACH);', [SCRIPTS]),
    ('every-read-kept', YONDER,
     '(need == OS64_DOM_NEED_BOXES || (need == OS64_DOM_NEED_REACH && !pending.transforms));',
     '(need != 99);', [SCRIPTS]),
    ('covered-not-owed', YONDER,
     '    if (g.covered && page_rebuild_owed(&g.page) && shown_pending().verdict != FLOW_PENDING_RESTYLE) {',
     '    if (0) {', [SCRIPTS]),
    ('owed-while-seen', YONDER,
     '    if (g.covered && page_rebuild_owed(&g.page) && shown_pending().verdict != FLOW_PENDING_RESTYLE) {',
     '    if (page_rebuild_owed(&g.page) && shown_pending().verdict != FLOW_PENDING_RESTYLE) {', [SCRIPTS]),
    ('owed-not-paid-before-a-layout', YONDER,
     '    if (page_rebuild_owed(&g.page) && !script_rebuild())\n        return false;\n    int32_t offset = 0;',
     '    int32_t offset = 0;', [SCRIPTS]),
]

# Not run, because no host case can tell:
# - `p->laid_state` against the form state's version (shown_pending): no page
#   in the hosts has a control whose state moves a box, and the guard follows
#   the graft's own source test, which carries the same version.
# - window_seen paying the owed rebuild, and pointer_turn's owed rebuild
#   before a re-hit: both need the window system's events, which the scripts
#   host does not drive. script_rendering_step's two sides are tested.
# - script_turn making the page current when asks wait (g.nasks): an ask a
#   covered page's script makes, which the scripts host's cases do not.
# - The log's barrier and its walk (log_find with the index broken): the
#   walk is the fallback for an index that could not grow, which the hosts
#   do not reach; with the index, the barrier's work is the index's clear,
#   which `merge-past-children` breaks.

env = dict(os.environ, YONDER_PROFILE_ONLY='1')
caught, missed, unbuilt = [], [], []
for name, path_rel, old, new, suites in mutants:
    if args.only and args.only not in name:
        continue
    path = root / path_rel
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {path_rel}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            code = 0
            for suite in suites:
                try:
                    result = subprocess.run(['bash', '-c', suite], cwd=root, stdout=out, stderr=out, env=env,
                                            timeout=1800)
                    code = result.returncode
                except subprocess.TimeoutExpired:
                    code = 'timeout'
                if code != 0:
                    break
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {path_rel} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\npending mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
