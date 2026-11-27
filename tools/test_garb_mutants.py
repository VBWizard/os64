#!/usr/bin/env python3
"""One deliberate break at a time to libgarb, against its host suite
(tools/test_garb_host.sh): a mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure is neither.

The first are the kept parse scratch's (HEAP_BINS.md § Part A): a block
read borrows its owner's scratch and hands it back, and whoever holds a
scratch frees it.

The shape is test_yonder_diag_mutants.py's: each mutant is applied to the
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/garb-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PARSE = 'userland/libgarb/parse.c'
CASCADE = 'userland/libgarb/cascade.c'
SELECT = 'userland/libgarb/select.c'
GARB = 'tools/test_garb_host.sh'
# The cascade's updates by hand, then against a fresh cascade on the walk.
UPDATES = 'tools/test_garb_host.sh --updates && tools/test_garb_host.sh --oracle'
PROPS = 'userland/libgarb/props.c'
STYLE = 'userland/libflow/style.c'
COMPUTED = 'userland/libflow/computed.c'
PAINT = 'userland/apps/yonder/paint.c'
FLOW = 'tools/test_libflow_host.sh'
FLOW_COMPUTED = 'tools/test_libflow_host.sh --computed'
YON = 'tools/test_yonder_host.sh'
FLOWC = 'userland/libflow/flow.c'
COPY = 'userland/libflow/style_copy.inc'
YONDER = 'userland/apps/yonder/yonder.c'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states.
mutants = [
    # The renewal is the bytes (RELAYOUT.md § the retained budget): no
    # count of updates renews, what a renewal reports is what it retained,
    # and yonder's record says each one.
    ('renewal-by-a-count-of-updates', CASCADE,
     '    if (added >= c->retained_budget) { c->refusal = GARB_UPDATE_RETAINED; return false; }',
     '    if (added >= c->retained_budget || c->updates >= 64) { c->refusal = GARB_UPDATE_RETAINED; return false; }',
     UPDATES),
    ('renewal-retained-not-said', CASCADE,
     '    return os64_arena_stats(c->self.arena).used_bytes - c->initial_state_bytes + c->update_attribute_bytes;',
     '    return 0;', UPDATES),
    ('renewal-not-recorded', YONDER, '            sample.renewals = 1;\n', '', SCRIPTS),
    # The kept read (KEPT_READ.md): its key, its serial, its refcount, and
    # the copy that keeps it unwritten.
    ('kept-read-a-reparse-keeps-its-serial', PARSE,
     '    out->serial = __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED);',
     '    out->serial = 1 + 0 * __atomic_add_fetch(&serials, 1, __ATOMIC_RELAXED);', UPDATES),
    ('kept-read-key-ignores-the-environment', CASCADE,
     '    if (!env_equal(r->env, env)) return GARB_READ_ENVIRONMENT;',
     '    if (!env_equal(r->env, env) && false) return GARB_READ_ENVIRONMENT;', UPDATES),
    ('kept-read-key-ignores-the-serials', CASCADE,
     '        if (a->serial != serial || a->sheet != b->sheet || a->parent != b->parent',
     '        if ((a->serial != serial && false) || a->sheet != b->sheet || a->parent != b->parent', UPDATES),
    ('kept-read-key-ignores-the-address', CASCADE,
     '        if (a->serial != serial || a->sheet != b->sheet || a->parent != b->parent',
     '        if (a->serial != serial || a->parent != b->parent', UPDATES),
    ('kept-read-skips-grow-past-the-cap', CASCADE,
     '        if (bigger > GARB_SKIPS_MAX && c->skipcap < GARB_SKIPS_MAX)\n            bigger = GARB_SKIPS_MAX;\n',
     '', UPDATES),
    ('kept-read-key-ignores-the-limits', CASCADE,
     '    if (r->state_bytes != limits.state_bytes || r->read_bytes != limits.read_bytes) return GARB_READ_LIMITS;',
     '    if ((r->state_bytes != limits.state_bytes || r->read_bytes != limits.read_bytes) && false) return GARB_READ_LIMITS;',
     UPDATES),
    ('kept-read-key-ignores-the-document', CASCADE,
     '    if (r->doc != doc) return GARB_READ_DOCUMENT;\n', '', UPDATES),
    ('kept-read-key-ignores-the-scopes', CASCADE,
     '    if (r->ngiven != nscopes) return GARB_READ_SCOPES;\n', '', UPDATES),
    ('kept-read-a-short-read-is-taken', CASCADE,
     '    if (!r->keep) return GARB_READ_NOT_KEEPABLE;\n', '', UPDATES),
    ('kept-read-skipped-records-written-in-place', CASCADE,
     '    if (c->read == NULL || c->skips == NULL || c->skips != c->read->skips)\n        return true;',
     '    return true;', UPDATES),
    ('kept-read-taken-without-a-reference', CASCADE,
     '        offered->refs++;\n        c->read = offered;', '        c->read = offered;', UPDATES),
    # :empty, asked again (cascade.c empty_turned, mark_turned).
    ('empty-the-answer-is-not-asked-again', CASCADE,
     '    bool turned = now != s->empty;', '    bool turned = true; (void)now;', UPDATES),
    ('empty-a-turn-marks-nothing', CASCADE,
     '    } else if (turned && !mark_turned(c, parent)) {', '    } else if (false && turned) {', UPDATES),
    ('empty-a-text-turn-marks-nothing', CASCADE,
     '            marked = !asked || !empty_turned(c, parent) || mark_turned(c, parent);',
     '            marked = true;', UPDATES),
    ('empty-the-siblings-are-skipped', CASCADE,
     '    if (c->dependencies.filtered && x->parent)\n        return mark_siblings(c, x->parent->first_child);\n'
     '    if (!(c->reads_any & GARB_READS_EMPTY)) return true;\n',
     '    if (true) return true;\n', UPDATES),
    ('empty-the-hosts-tree-is-skipped', CASCADE,
     '    if (x->shadow_root || is_slot(x)) return mark_changed(c, x, GARB_MARK_PARENT);\n', '',
     'tools/test_garb_host.sh --shadow'),
    ('empty-the-slots-assigned-are-skipped', CASCADE,
     '    if (x->shadow_root || is_slot(x)) return mark_changed(c, x, GARB_MARK_PARENT);\n',
     '    if (x->shadow_root) return mark_changed(c, x, GARB_MARK_PARENT);\n',
     'tools/test_garb_host.sh --shadow'),
    ('empty-the-answer-is-kept-from-no-read', CASCADE,
     '        slot->empty = c->dependencies.parent_state && garb_element_empty(el);\n', '', UPDATES),
    ('empty-details-loses-its-whole-mark', CASCADE,
     '        if (!mark_changed(c, parent, GARB_MARK_PARENT)) return false;\n    } else if (turned',
     '        (void)0;\n    } else if (turned', UPDATES),
    # The position indexes, counted again (cascade.c index_walk, select.c
    # index_need), and the readers they mark through (readers_at).
    ('back-the-count-is-not-taken-again', CASCADE,
     '    if ((c->dependencies.back_child || c->dependencies.back_type) && !index_turns(c, e, false)) return false;\n',
     '    if (false && !index_turns(c, e, false)) return false;\n', UPDATES),
    ('front-the-count-is-not-taken-again', CASCADE,
     '    if ((c->dependencies.front_child || c->dependencies.front_type) && !index_turns(c, e, true)) return false;\n',
     '    if (false && !index_turns(c, e, true)) return false;\n', UPDATES),
    ('index-a-turn-marks-the-element-alone', CASCADE,
     '            if (!mark_reached(c, x, got)) return INDEX_FAILED;',
     '            (void)got;\n            if (!mark_element(c, x)) return INDEX_FAILED;',
     'tools/test_garb_host.sh --updates && tools/test_garb_host.sh --shadow && tools/test_garb_host.sh --oracle'),
    ('index-a-turn-skips-the-siblings-after-it', CASCADE,
     '            if (!mark_reached(c, x, got)) return INDEX_FAILED;',
     '            got.on = got.next = false;\n            if (!mark_reached(c, x, got)) return INDEX_FAILED;', UPDATES),
    ('index-saturation-one-short', CASCADE,
     'static uint8_t index_bound(uint8_t need) { return need ? (uint8_t)(need + 1) : 0; }',
     'static uint8_t index_bound(uint8_t need) { return need; }', UPDATES),
    ('index-early-stop-one-too-soon', CASCADE,
     '        if (compare && !kt && *kept == kc && n == kc) return INDEX_STOPPED;',
     '        if (compare && !kt && *kept + 1 >= kc && n + 1 >= kc) return INDEX_STOPPED;', UPDATES),
    ('index-unknown-counts-as-unchanged', CASCADE,
     '        uint8_t turned = (uint8_t)((*kept != n ? child_key : 0) | (*kept_type != t ? type_key : 0));',
     '        uint8_t turned = (uint8_t)((*kept && *kept != n ? child_key : 0) | (*kept_type && *kept_type != t ? type_key : 0));',
     UPDATES),
    ('only-child-not-a-front-form', SELECT,
     '    case P_ONLY_CHILD: return GARB_READS_FRONT | GARB_READS_BACK;',
     '    case P_ONLY_CHILD: return GARB_READS_BACK;', UPDATES),
    ('only-child-front-need-not-taken', SELECT,
     '                case P_ONLY_CHILD:\n                    index_need(&out->front_child, fu, 0, 1);\n',
     '                case P_ONLY_CHILD:\n', UPDATES),
    ('unbounded-treated-as-bounded', SELECT,
     '    int64_t n = a > 1 ? GARB_INDEX_MAX + 1 : a == 1 ? (int64_t)b - 1 : b;',
     '    int64_t n = a == 1 ? (int64_t)b - 1 : b;', UPDATES),
    ('n-plus-k-settles-one-late', SELECT,
     '    int64_t n = a > 1 ? GARB_INDEX_MAX + 1 : a == 1 ? (int64_t)b - 1 : b;',
     '    int64_t n = a > 1 ? GARB_INDEX_MAX + 1 : a == 1 ? (int64_t)b - 2 : b;', UPDATES),
    ('back-the-build-counts-nothing', CASCADE,
     '            if (next_element(x) == NULL) {\n'
     '                if (c->dependencies.back_child || c->dependencies.back_type) index_walk(c, x, false, false);\n',
     '            if (next_element(x) == NULL) {\n', UPDATES),
    ('front-the-build-counts-nothing', CASCADE,
     '                if (c->dependencies.front_child || c->dependencies.front_type)\n'
     '                    index_walk(c, first_element(x), false, true);\n            }\n        }\n    os64_free(scratch.items);',
     '            }\n        }\n    os64_free(scratch.items);', UPDATES),
    ('back-what-came-in-is-not-counted', CASCADE,
     '                if (x != root && next_element(x) == NULL) {\n'
     '                    if (c->dependencies.back_child || c->dependencies.back_type) index_walk(c, x, false, false);\n',
     '                if (x != root && next_element(x) == NULL) {\n', UPDATES),
    ('back-a-restyle-forgets-the-count', CASCADE,
     '.changed = c->updates + 1, .back = old.back, .back_type = old.back_type,',
     '.changed = c->updates + 1,', UPDATES),
    ('front-a-restyle-forgets-the-count', CASCADE,
     '                .front = old.front, .front_type = old.front_type};',
     '                .front = 0};', UPDATES),
    ('back-overflow-leaves-stale-counts', CASCADE,
     '                    else if (u) u->back = u->back_type = 0;', '                    else (void)u;', UPDATES),
    ('index-the-stamp-skips-the-first-walk', CASCADE,
     '    if (!walked || *walked != c->updates + 1) {', '    if (!walked || *walked == c->updates + 1) {',
     UPDATES),
    ('marks-a-class-write-counted-as-other', CASCADE,
     '                kind = os64_streq(e->attr_name, "class") ? GARB_MARK_CLASS',
     '                kind = os64_streq(e->attr_name, "class") ? GARB_MARK_CHANGED', UPDATES),
    ('marks-an-index-turn-counted-as-siblings', CASCADE,
     '            c->mark_now = front ? GARB_MARK_FRONT : GARB_MARK_BACK;',
     '            c->mark_now = GARB_MARK_SIBLINGS;', UPDATES),
    # The subject :has() filter (cascade.c subject_may_turn, subjects_live;
    # select.c garb_has_subject_children_may_turn) and the match lists'
    # sessions (RuleSeen).
    ('has-subject-filter-passes-nothing', CASCADE,
     '        if ((live || !held) && garb_anchor_filters_pass(&c->subjects[i], a)) {',
     '        if (false && garb_anchor_filters_pass(&c->subjects[i], a)) {', UPDATES),
    ('has-subject-filter-passes-everything', CASCADE,
     '    bool may = c->subjects == NULL, held = may;', '    bool may = true, held = may;', UPDATES),
    ('has-subject-every-change-turns-every-compound', CASCADE,
     '        bool may = e->kind == OS64_HTML_CHANGE_CHILDREN\n',
     '        bool may = true ? true : e->kind == OS64_HTML_CHANGE_CHILDREN\n', UPDATES),
    ('has-subject-a-grown-set-keeps-the-old-visits', CASCADE,
     '    if (grew) c->visit_epoch++;', '    (void)grew;', UPDATES),
    ('has-argument-position-filterable', SELECT,
     '            if (reads_around(&c->s[k])) return ASK_ALWAYS;', '            if (false && reads_around(&c->s[k])) return ASK_ALWAYS;',
     UPDATES),
    ('has-argument-sibling-combinator-filterable', SELECT,
     '        if ((j > 0 && (c->left == C_NEXT || c->left == C_SUBSEQUENT)) || !has_filter(c)) return ASK_ALWAYS;',
     '        if (!has_filter(c)) return ASK_ALWAYS;', UPDATES),
    ('has-argument-plus-filterable', SELECT,
     '            for (size_t r = 0; x->relative == C_NEXT && r < n; r++)',
     '            for (size_t r = 0; false && x->relative == C_NEXT && r < n; r++)', UPDATES),
    ('has-argument-roots-only-for-every-argument', SELECT,
     '    return x->n == 1 && beside ? ASK_ROOTS : ASK_SUBTREES;', '    (void)beside;\n    return ASK_ROOTS;', UPDATES),
    ('has-argument-a-subtree-put-back-is-trusted', CASCADE,
     '            c->batch_detached = batch_holds(c->batch_removed, c->nbatch_removed, x);',
     '            c->batch_detached = false;', UPDATES),
    ('has-argument-a-removed-subtree-is-trusted', CASCADE,
     '    return c->batch_detached || stale(ctx, x);', '    (void)c;\n    return stale(ctx, x);', UPDATES),
    ('rule-seen-one-session-for-every-list', CASCADE,
     '    c->match_session++;\n    int32_t nm = matching(c, &q, m, 0, mcap);',
     '    int32_t nm = matching(c, &q, m, 0, mcap);', UPDATES),
    ('marks-the-childrens-siblings-counted-as-siblings', CASCADE,
     '    c->mark_now = GARB_MARK_SUFFIX;\n    if (c->dependencies.filtered)\n',
     '    c->mark_now = GARB_MARK_SIBLINGS;\n    if (c->dependencies.filtered)\n', UPDATES),
    ('marks-inherited-counts-the-changes-too', CASCADE,
     '    c->marks.inherited = c->ndirty - c->marks.marked;', '    c->marks.inherited = c->ndirty;', UPDATES),
    ('back-of-type-not-counted', CASCADE,
     '            if (types[i].n < kt) types[i].n++;', '            (void)kt;', UPDATES),
    # The readers (select.c readers, cascade.c readers_at and the walks).
    ('reader-a-chain-is-plus-alone', SELECT,
     '    else out.reach = sib == 1 && !continues ? GARB_REACH_NEXT : GARB_REACH_CHAIN;',
     '    else out.reach = GARB_REACH_NEXT;\n    (void)continues;', UPDATES),
    ('reader-a-chain-reaches-one-sibling', CASCADE,
     '        got.on |= r->reach == GARB_REACH_FOLLOWING || r->reach == GARB_REACH_CHAIN;\n'
     '        got.down |= r->down;\n    }\n    if (any) w->reached[asker]++;\n    return got;\n}\nstatic void reached_add',
     '        got.on |= r->reach == GARB_REACH_FOLLOWING;\n        got.next |= r->reach == GARB_REACH_CHAIN;\n'
     '        got.down |= r->down;\n    }\n    if (any) w->reached[asker]++;\n    return got;\n}\nstatic void reached_add', UPDATES),
    ('reader-an-is-alternative-loses-the-holder-reach', SELECT,
     '                readers(sel, s->list, reach, foreign || host,',
     '                readers(sel, s->list, (Reach){GARB_REACH_SELF, false}, foreign || host,', UPDATES),
    ('reader-bare-passes-nothing', SELECT,
     '    if (reader->bare) return true;\n    if (!is_el(element)) return false;\n    for (int32_t o = 0; o < reader->nouter; o++)\n        if (!filters_pass(reader->list,',
     '    if (reader->bare) return false;\n    if (!is_el(element)) return false;\n    for (int32_t o = 0; o < reader->nouter; o++)\n        if (!filters_pass(reader->list,', UPDATES),
    ('reader-untested-is-filtered', CASCADE,
     '        if (!all && !(r->untested & r->reads & key) && !garb_reader_filters_pass(r, x)) continue;',
     '        if (!all && !garb_reader_filters_pass(r, x)) continue;', UPDATES),
    ('reader-a-stale-stand-in-is-tested', CASCADE,
     '    bool all = x == NULL || (stand_in && stale(c, x)), any = false;',
     '    bool all = x == NULL, any = false;\n    (void)stand_in;', UPDATES),
    ('reader-the-unbounded-walk-marks-subtrees', CASCADE,
     '            Reached got = readers_at(c, GARB_READS_FRONT_ANY, REACH_ANY, x, true, GARB_ASKED_FRONT_WALK);',
     '            Reached got = readers_at(c, GARB_READS_FRONT_ANY, REACH_ANY, x, true, GARB_ASKED_FRONT_WALK);\n'
     '            got.down = true;', UPDATES),
    ('reader-the-unbounded-walk-starts-at-the-first-child', CASCADE,
     '            on = on || walk_flagged(c, x, WALK_FROM);', '            on = true;', UPDATES),
    ('reader-the-unbounded-back-walk-passes-the-change', CASCADE,
     '            if (!on) { on = walk_flagged(c, x, WALK_TO); continue; }', '            if (!on) on = true;', UPDATES),
    ('reader-tilde-acts-at-the-pair', CASCADE,
     '    unsigned plus = (1u << GARB_REACH_NEXT) | (1u << GARB_REACH_CHAIN);',
     '    unsigned plus = (1u << GARB_REACH_NEXT) | (1u << GARB_REACH_CHAIN) | (1u << GARB_REACH_FOLLOWING);', UPDATES),
    ('reader-the-pair-made-is-not-asked', CASCADE,
     '    if (made) reached_add(&pair,', '    if (false && made) reached_add(&pair,', UPDATES),
    ('reader-the-element-after-takes-what-the-batch-brought', CASCADE,
     '    while (after && batch_holds(c->batch_added, c->nbatch_added, after)) after = next_element(after);\n',
     '', UPDATES),
    ('reader-host-argument-loses-its-sibling-reach', SELECT,
     '            Reach reach = reach_right(x, j, around);',
     '            Reach reach = foreign ? (Reach){GARB_REACH_SELF, true} : reach_right(x, j, around);',
     'tools/test_garb_host.sh --shadow'),
    ('reader-an-append-walks-the-front', CASCADE,
     '    if (front && (!p || walk_flagged(c, parent, WALK_ALL_FRONT | WALK_SOME_FRONT))) {',
     '    if (front) {', UPDATES),
    ('position-reads-overflows-b', SELECT,
     '    int64_t n = a > 1 ? GARB_INDEX_MAX + 1 : a == 1 ? (int64_t)b - 1 : b;',
     '    int64_t n = a > 1 ? GARB_INDEX_MAX + 1 : a == 1 ? b - 1 : b;', UPDATES),
    ('reader-a-slotted-compound-reaches-its-slot-alone', SELECT,
     '            reach.down |= slotted;', '            (void)slotted;', 'tools/test_garb_host.sh --shadow'),
    ('reader-a-removed-element-passes-every-reader', CASCADE,
     '            reached_add(&moved, readers_at(c, GARB_READS_NEIGHBOURS, run, e->removed[i], true, GARB_ASKED_ADDED_REMOVED));',
     '            reached_add(&moved, readers_at(c, GARB_READS_NEIGHBOURS, run, NULL, true, GARB_ASKED_ADDED_REMOVED));', UPDATES),
    ('reader-an-element-moved-later-still-stands', CASCADE,
     '    return x == NULL || (x->parent == parent && !batch_holds(c->batch_removed, c->nbatch_removed, x));',
     '    (void)c;\n    return x == NULL || x->parent == parent;', UPDATES),
    ('reader-an-empty-turn-reaches-no-sibling', CASCADE,
     '    got.self = false;\n    return mark_reached(c, x, got);', '    (void)got;\n    return true;', UPDATES),
    ('reader-text-moves-elements', CASCADE,
     '    if (!moves_elements(e)) return true;\n', '', UPDATES),
    ('reader-an-attribute-on-a-brought-element-marks', CASCADE,
     '                brought = batch_holds(c->batch_added, c->nbatch_added, x);',
     '                brought = false && batch_holds(c->batch_added, c->nbatch_added, x);', UPDATES),
    # A change of an attribute by its keys (cascade.c mark_attribute,
    # select.c garb_reader_filters_pass_except).
    ('attr-new-classes-not-keys', CASCADE,
     '            n = class_difference(now, s->klass, k, n);\n', '', UPDATES),
    ('attr-old-classes-not-keys', CASCADE,
     '            n = class_difference(s->klass, now, k, n);\n', '', UPDATES),
    ('attr-other-tests-ignored', CASCADE,
     '            if (!all && !garb_reader_filters_pass_except(r, el, c->change_keys, c->nchange_keys)) continue;',
     '            if (!all && false && !garb_reader_filters_pass_except(r, el, c->change_keys, c->nchange_keys)) continue;', UPDATES),
    ('attr-changed-tests-kept', SELECT,
     '        if (filters(&c->s[k]) && !tests_changed(&c->s[k], changed, n) &&',
     '        if (filters(&c->s[k]) && (tests_changed(&c->s[k], changed, n) || true) &&', UPDATES),
    ('attr-a-second-change-trusts-the-first', CASCADE,
     '    bool twice = lo + 1 < c->nbatch_attrs && c->batch_attrs[lo + 1] == el;',
     '    bool twice = false;\n    (void)lo;', UPDATES),
    ('attr-up-keys-ignored', CASCADE,
     '    if (c->change_unknown || c->change_up) return mark_changed(c, el, kind);',
     '    if (c->change_unknown) return mark_changed(c, el, kind);', UPDATES),
    ('attr-element-not-touched', CASCADE,
     '    if (s) s->attr_touched = true;\n', '    (void)s;\n', UPDATES),
    ('attr-class-not-refreshed-on-restore', CASCADE,
     '        s->klass = attr_value(x, "class");\n        s->idv = attr_value(x, "id");\n        if (old.attr_touched)',
     '        if (old.attr_touched)', UPDATES),
    ('attr-every-change-marks-the-subtree', CASCADE,
     '    if (c->change_unknown || c->change_up) return mark_changed(c, el, kind);',
     '    return mark_changed(c, el, kind);', UPDATES),
    ('attr-every-subject-compound-live', CASCADE,
     '                       : !keyed || garb_has_subject_reads_key(a, c->change_keys, c->nchange_keys);',
     '                       : keyed || true;', UPDATES),
    ('attr-customs-never-differ', CASCADE,
     '    if (a == NULL || b == NULL || a->parent != b->parent || a->n != b->n) return false;',
     '    return true;', UPDATES),
    ('attr-a-namespaced-class-is-no-class', CASCADE,
     '    if (os64_streq(e->attr_name, "class")) {',
     '    if (e->attr_ns == NULL && os64_streq(e->attr_name, "class")) {', UPDATES),
    ('attr-holder-keys-dropped', SELECT,
     '            for (int32_t o = 0; subject && o < nholders && nheld <= GARB_ANCHOR_FILTERS; o++) held[nheld++] = holders[o];\n',
     '            (void)holders;\n', UPDATES),
    # THE RECOMPUTE (L4 part 2): what an element's changed custom property
    # map owes what is under it. Two of its guards have no mutant, because no
    # case today can tell them broken: a declaration whose new substitution
    # sets other longhands than before takes the whole cascade (every
    # shorthand libgarb reads sets the longhands prop_longhands names, which
    # its `unset` fallback uses); and an element restyled alone whose
    # element it inherits from moved names nothing (a slot move changes an
    # assignment, which marks the assigned element's whole subtree, so it is
    # never restyled alone for it).
    ('recompute-a-map-change-reaches-nothing-below', CASCADE,
     '    if (moved) moved_slot(c, s);\n', '    (void)moved;\n', UPDATES),
    ('recompute-the-prune-stops-while-the-map-differs', CASCADE,
     '        if (own == NULL || own->reads_up) return false;\n    }\n    return true;',
     '        if (own == NULL) return false;\n    }\n    return true;', UPDATES),
    ('recompute-substitution-not-redone', CASCADE,
     '    if (s->npend > 0 && !recompute_winners(c, s, map, &whole)) {',
     '    if (false && s->npend > 0 && !recompute_winners(c, s, map, &whole)) {', UPDATES),
    ('recompute-sets-changed-always', CASCADE,
     '    if (sets != NULL) {\n        st->sets = sets;',
     '    s->changed = c->updates + 1;\n    if (sets != NULL) {\n        st->sets = sets;', UPDATES),
    ('recompute-sets-changed-never', CASCADE,
     '        st->sets = sets;\n        s->changed = c->updates + 1;\n',
     '        st->sets = sets;\n', UPDATES),
    ('recompute-hides-a-match-mark', CASCADE,
     '            s = slot_for(c, x, false);\n            if (s && s->dirty) failed = !restyle(c, x, s, &m, &mcap, &scratch);\n'
     '            else if (s && c->any_moved && recompute_owed(c, x)) failed = !recompute(c, x, s, &m, &mcap, &scratch);',
     '            s = slot_for(c, x, false);\n            if (s && c->any_moved && recompute_owed(c, x)) failed = !recompute(c, x, s, &m, &mcap, &scratch);\n'
     '            else if (s && s->dirty) failed = !restyle(c, x, s, &m, &mcap, &scratch);', UPDATES),
    ('recompute-counted-toward-the-half', CASCADE,
     '    c->marks.recomputed++;\n    if (s->pseudo_vars',
     '    c->marks.recomputed++;\n    if (++c->ndirty > c->slotcount / 2) { c->refusal = GARB_UPDATE_BROAD; return false; }\n    if (s->pseudo_vars', UPDATES),
    ('recompute-names-made-from-above-not-kept', CASCADE,
     '        if (s->customs->own[k].reads_up) moved_name(c, s->customs->own[k].name, s->customs->own[k].len);',
     '        (void)k;', UPDATES),
    ('recompute-a-var-entry-reads-nothing-above', CASCADE,
     '    cu->reads_up = garb_decl_has_var(d);', '    cu->reads_up = false;', UPDATES),
    ('recompute-a-restyled-element-under-a-moved-map-stops', CASCADE,
     '    bool moved = customs_moved(c, &old, s) || (owed && !holds_moved(c, s));',
     '    bool moved = customs_moved(c, &old, s);', UPDATES),
    ('recompute-an-origin-names-nothing', CASCADE,
     '            moved_name(c, n->own[k].name, n->own[k].len);\n            moved = true;',
     '            moved = true;', UPDATES),
    ('recompute-a-pseudo-elements-var-is-not-whole', CASCADE,
     '    if (s->pseudo_vars || s->nframes > 0 || anim_inherits(s)) {',
     '    if (s->nframes > 0 || anim_inherits(s)) {', UPDATES),
    ('recompute-the-same-path-keeps-the-old-var-winners', CASCADE,
     '        s->pend = fresh.pend;\n        s->npend = fresh.npend;\n', '', UPDATES),
    ('recompute-the-same-path-keeps-the-old-map', CASCADE,
     '        s->customs = fresh.customs;\n        s->own_customs = fresh.own_customs;\n', '', UPDATES),
    ('recompute-the-same-path-keeps-the-old-pseudo-answer', CASCADE,
     '        s->pseudo_vars = fresh.pseudo_vars;\n', '', UPDATES),
    ('recompute-a-changed-animation-name-builds-no-keyframes', CASCADE,
     '            if (p == GARB_ANIMATION_NAME) { *whole = true; return false; }\n', '', UPDATES),
    ('recompute-a-changed-animation-name-reaches-nothing', CASCADE,
     '    if (!anim_same(&old, s)) {', '    if (false && !anim_same(&old, s)) {', UPDATES),
    ('recompute-an-inherited-animation-name-is-not-whole', CASCADE,
     '    if (s->pseudo_vars || s->nframes > 0 || anim_inherits(s)) {',
     '    if (s->pseudo_vars || s->nframes > 0 || (false && anim_inherits(s))) {', UPDATES),
    # The kept scratch.
    ('a-read-frees-its-scratch', PARSE,
     '    owner->scratch = p.scratch;\n    owner->scratch_cap = p.scratch_cap;',
     '    os64_free(p.scratch);\n    owner->scratch = NULL;\n    owner->scratch_cap = 0;', GARB),
    ('a-read-keeps-the-grown-scratch', PARSE,
     '    owner->scratch = p.scratch;\n    owner->scratch_cap = p.scratch_cap;',
     '', GARB),
    ('a-read-starts-its-own-scratch', PARSE,
     '               .scratch = owner->scratch, .scratch_cap = owner->scratch_cap};',
     '               .scratch = NULL, .scratch_cap = 0};', GARB),
    ('a-free-keeps-the-scratch', PARSE,
     '    os64_arena_destroy(parsed->arena);\n    os64_free(parsed->scratch);',
     '    os64_arena_destroy(parsed->arena);', GARB),
    ('a-text-parse-keeps-its-scratch', PARSE,
     '    os64_free(p.scratch);\n    return finish(out, &p, &tz, st);',
     '    return finish(out, &p, &tz, st);', GARB),
    ('the-cascade-frees-only-its-arena', CASCADE,
     '    os64_free(c->subjects_now);\n    garb_free(&c->self);',
     '    os64_free(c->subjects_now);\n    os64_arena_destroy(c->self.arena);', GARB),
    # CSS Images: a stop with two positions, and conic gradients.
    ('a-two-position-stop-counts-once', PROPS,
     '        colours += hint ? 0 : positions == 2 ? 2 : 1;',
     '        colours += !hint;', GARB),
    ('a-conic-stop-reads-a-length', PROPS,
     '    return kind == GRADIENT_CONIC ? ap_val(s, c, &v) : length_percentage(s, c);',
     '    (void)v;\n    (void)kind;\n    return length_percentage(s, c);', GARB),
    ('a-conic-has-no-prelude', PROPS,
     '         : kind == GRADIENT_CONIC  ? conic_prelude(s, g)',
     '         : kind == GRADIENT_CONIC  ? (false && conic_prelude(s, g))', GARB),
    ('an-angle-calc-costs-the-declaration', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                !((false && angle_calc(t->children, t->nchildren, false)) || writes_interpolation(t)))', GARB),
    ('any-invalid-conic-is-taken-by-name', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                ((void)angle_calc(t->children, t->nchildren, false), (void)writes_interpolation(t), false))', GARB),
    ('a-conic-is-read-as-radial', STYLE,
     '    } else if (is_word(geo, "conic") && geo->nitems == 3) {',
     '    } else if (false && is_word(geo, "conic") && geo->nitems == 3) {', FLOW),
    ('a-conic-stop-in-degrees-is-unwritten', STYLE,
     '    if (kind != FLOW_GRADIENT_CONIC || v->kind != GARB_V_ANGLE)',
     '    if (((void)kind, true) || v->kind != GARB_V_ANGLE)', FLOW),
    ('a-conic-is-written-as-radial', COMPUTED,
     '    } else if (g->kind == FLOW_GRADIENT_CONIC) {\n        // What the page left',
     '    } else if (false) {\n        // What the page left', FLOW_COMPUTED),
    ('a-conic-turns-anticlockwise', PAINT,
     '    double t = (atan2(u - l->cx, l->cy - v) - l->turn0)',
     '    double t = (atan2(l->cx - u, l->cy - v) - l->turn0)', YON),
    ('a-conic-ignores-its-from', PAINT,
     '        turn0 = g->angle / 1000.0 * 3.14159265358979323846 / 180;',
     '        turn0 = 0;', YON),
    ('a-conic-start-is-not-in-the-tile-key', PAINT,
     '                                line->turn0};',
     '                                0};', YON),
    ('an-interpolating-conic-costs-the-declaration', PROPS,
     '                !(angle_calc(t->children, t->nchildren, false) || writes_interpolation(t)))',
     '                !(angle_calc(t->children, t->nchildren, false) || (false && writes_interpolation(t))))', GARB),
    ('a-negative-conic-percentage-is-refused', PROPS,
     '    return vc_angle(c, true, out) || vc_dim(c, ACCEPT_PERCENT, true, false, &s->a, out);',
     '    return vc_angle(c, true, out) || vc_dim(c, ACCEPT_PERCENT, false, false, &s->a, out);', GARB),
    ('a-conic-centre-takes-the-far-side', PAINT,
     '    double t = (atan2(u - l->cx, l->cy - v) - l->turn0)',
     '    double t = (atan2(u - l->cx, -(v - l->cy)) - l->turn0)', YON),
    # Masks (MASKS.md): read, kept, written, grouped and drawn.
    ('a-mask-position-is-quirky', PROPS,
     '                      s->quirks && l->prop == GARB_BACKGROUND_POSITION_X, &s->a, out);',
     '                      s->quirks, &s->a, out);', GARB),
    ('the-mask-shorthand-is-quirky', PROPS,
     '            strict.quirks = false;\n            strict.a.short_of_memory = false;',
     '            strict.a.short_of_memory = false;', GARB),
    ('the-mask-position-shorthand-is-quirky', PROPS,
     '        strict.quirks = s->quirks && bg;',
     '        strict.quirks = s->quirks;', GARB),
    ('a-mask-position-takes-three-parts', PROPS,
     '            } else if (!have_pos && position(&strict, c, false, &x, &y)) {',
     '            } else if (!have_pos && position(&strict, c, true, &x, &y)) {', GARB),
    ('no-clip-takes-two-boxes-beside-it', PROPS,
     '            } else if (boxes < (no_clip ? 1 : 2) && (box[boxes] = vc_keyword(c, kBox)) != NULL) {',
     '            } else if (boxes < 2 && (box[boxes] = vc_keyword(c, kBox)) != NULL) {', GARB),
    ('one-box-sets-only-the-origin', PROPS,
     '        else if (boxes > 0)\n            l[M_CLIP] = kw(box[boxes - 1]);',
     '        else if (false)\n            l[M_CLIP] = kw(box[boxes - 1]);', GARB),
    ('a-mask-origin-starts-at-the-padding-box', PROPS,
     '                             percent(0),       auto_size(s),     kw("border-box"),',
     '                             percent(0),       auto_size(s),     kw("padding-box"),', GARB),
    ('a-mask-clip-takes-text', PROPS,
     'static const char *const kMaskClip[] = {"border-box", "padding-box", "content-box", "no-clip",\n',
     'static const char *const kMaskClip[] = {"border-box", "padding-box", "content-box", "no-clip", "text",\n', GARB),
    ('the-webkit-mask-is-unknown', PROPS,
     '    {"-webkit-mask", "mask", true},\n', '', GARB),
    ('a-mask-list-lands-in-the-backgrounds', STYLE,
     '    bool mask = p >= GARB_MASK_IMAGE && p <= GARB_MASK_CLIP;\n    flow_backgrounds_t *b',
     '    bool mask = false;\n    flow_backgrounds_t *b', FLOW_COMPUTED),
    ('the-initial-mask-origin-is-the-padding-box', STYLE,
     'static const flow_masks_t kMasks = {{&kNoImage, &kRepeat, &kAtStart, &kAtStart, &kOwnSize,\n'
     '                                     &kBorderBox, &kBorderBox,',
     'static const flow_masks_t kMasks = {{&kNoImage, &kRepeat, &kAtStart, &kAtStart, &kOwnSize,\n'
     '                                     &kPaddingBox, &kBorderBox,', FLOW_COMPUTED),
    ('inherit-takes-no-mask', STYLE,
     '    case GARB_MASK_COMPOSITE: case GARB_MASK_MODE:\n        f_take_mask(prop, d, s);',
     '    case GARB_MASK_COMPOSITE: case GARB_MASK_MODE:\n        (void)f_take_mask;', FLOW_COMPUTED),
    ('the-computed-mask-is-the-background', COMPUTED,
     '    const flow_backgrounds_t *bg = mask ? &s->masks.layers : &s->backgrounds;',
     '    const flow_backgrounds_t *bg = &s->backgrounds;', FLOW_COMPUTED),
    ('a-mask-makes-no-group', FLOWC,
     '            f_group_filter(b->style, f_atom(b)) || f_masked(b->style, f_atom(b)) ||',
     '            f_group_filter(b->style, f_atom(b)) ||', YON),
    ('a-mask-makes-no-stacked-box', FLOWC,
     '                   f_blended(src->style, false) || f_masked(src->style, false) ||',
     '                   f_blended(src->style, false) ||', YON),
    ('a-mask-cuts-no-bounds', FLOWC,
     '            done.r = mask_cut(tree->layers[done.layer].box,\n'
     '                              filter_grown(tree->layers[done.layer].box, done.r), scroll);',
     '            done.r = filter_grown(tree->layers[done.layer].box, done.r);', YON),
    ('no-clip-still-cuts', FLOWC,
     '        if (flow_mask_layer(box->style, i).layer.clip == FLOW_EDGE_NO_CLIP)\n            return r;',
     '        if (flow_mask_layer(box->style, i).layer.clip == FLOW_EDGE_NO_CLIP && false)\n            return r;', YON),
    ('a-copied-style-loses-its-mask', COPY,
     '    if(!style_copy_layers(c,&s->backgrounds) || !style_copy_layers(c,&s->masks.layers))return false;',
     '    if(!style_copy_layers(c,&s->backgrounds))return false;', FLOW),
    ('every-operator-adds', PAINT,
     '            yonder_mask_combine(run, one, n, m.composite);',
     '            yonder_mask_combine(run, one, n, FLOW_COMPOSITE_ADD);', YON),
    ('the-bottom-layer-is-composited', PAINT,
     '        if (i == layers - 1)\n            os64_memcpy(run, one, n);\n        else\n'
     '            yonder_mask_combine(run, one, n, m.composite);',
     '        if (i == layers - 1)\n            os64_memset(run, 0, n);\n'
     '        yonder_mask_combine(run, one, n, m.composite);', YON),
    ('luminance-is-read-as-alpha', PAINT,
     '            p->v->layer_take(p->v->ctx, one, m.mode == FLOW_MASK_LUMINANCE);',
     '            p->v->layer_take(p->v->ctx, one, false);', YON),
    ('a-mask-sized-to-nothing-shows', PAINT,
     '        if (nothing || (l.image == NULL && l.gradient == NULL))',
     '        if ((false && nothing) || (l.image == NULL && l.gradient == NULL))', YON),
    ('a-hiding-mask-is-drawn-plain-past-the-budget', PAINT,
     '    if (mask_hides_whole(box)) {\n        p->view = (os64_gui_rect_t){0, 0, 0, 0};',
     '    if (false && mask_hides_whole(box)) {\n        p->view = (os64_gui_rect_t){0, 0, 0, 0};', YON),
    ('a-mask-is-charged-no-captures', PAINT,
     '    int64_t area = (int64_t)shown.w * shown.h * (masked ? 1 + flow_mask_layers(box->style) : 1);',
     '    int64_t area = (int64_t)shown.w * shown.h * (masked ? 1 : 1);', YON),
    ('a-mask-picture-is-a-background-picture', PAINT,
     '            (void)p->v->backdrop(p->v->ctx, b, i | YONDER_MASK_LAYER, &area, origin, area.x,',
     '            (void)p->v->backdrop(p->v->ctx, b, i, &area, origin, area.x,', YON),
    ('a-mask-reference-is-drawn', PAINT,
     '        if (mask_reference(&l))\n            return false;',
     '        if (mask_reference(&l) && false)\n            return false;', YON),
    ('a-deep-hiding-mask-is-drawn', PAINT,
     '        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL) || mask_hides_whole(box))',
     '        if (flow_box_through(box, p->scroll, (double[6]){0}, NULL, NULL))', YON),
    ('a-refused-position-keeps-its-tokens', PROPS,
     '    if (position_read(s, c, three, x, y))\n        return true;\n    c->i = at;',
     '    if (position_read(s, c, three, x, y))\n        return true;\n    (void)at;', GARB),
    ('a-filtered-mask-is-cut-before-its-filter', FLOWC,
     '    if (!f_masked(box->style, f_atom(box)) || f_group_filter(box->style, f_atom(box)))\n        return r;',
     '    if (!f_masked(box->style, f_atom(box)))\n        return r;', YON),
    # The atom group: an atom on a line with an effect is stacked whole.
    ('an-effect-stacks-no-atom', FLOWC,
     '    if (moved || (atom && (f_opacity_context(s) || f_filtered(s, true) || f_blended(s, true) ||',
     '    if (moved || (false && atom && (f_opacity_context(s) || f_filtered(s, true) || f_blended(s, true) ||', YON),
    ('a-picture-is-a-plain-inline', 'userland/libflow/internal.h',
     '    return b->kind == FLOW_BOX_ATOMIC;\n}',
     '    (void)b;\n    return false;\n}', FLOW),
    ('a-stacked-atom-leaves-its-line', FLOWC,
     '            if (pieces != NULL && !(fr->kind == FF_ATOMIC && f_transforms(fr->style)))',
     '            if (pieces != NULL && !pieces->stacked)', FLOW),
    ('the-layer-budget-is-the-repaint', PAINT,
     '                          .filter_left = (int64_t)YONDER_FILTER_AREA_VIEWS * whole.w * whole.h};',
     '                          .filter_left = (int64_t)YONDER_FILTER_AREA_VIEWS * viewport.w * viewport.h + 0 * whole.w};', YON),
    ('a-mask-picture-is-never-sent-for', YONDER,
     '    return flow_background_layers(s) + flow_mask_layers(s);',
     '    return flow_background_layers(s) + 0 * flow_mask_layers(s);', SCRIPTS),
    ('a-mask-reference-is-sent-for', YONDER,
     '    if (l.image != NULL && l.image_len > 0 && l.image[0] == \'#\')\n        l.image = NULL;',
     '    if (false)\n        l.image = NULL;', SCRIPTS),
    # Masks M2: -webkit-mask-composite's legacy words, and a mask's
    # position and size animated (MASKS.md § 4, § 5).
    ('the-legacy-composite-reads-no-legacy-word', PROPS,
     '    static const Longhand kWebkitComposite = {GARB_MASK_COMPOSITE, G_MASK_LEGACY,\n'
     '                                              kWebkitMaskComposite};',
     '    static const Longhand kWebkitComposite = {GARB_MASK_COMPOSITE, G_MASK_KEYWORD,\n'
     '                                              kMaskComposite};', GARB),
    ('xor-is-read-as-add', PROPS,
     '    "add", "subtract", "intersect", "exclude", "intersect"};',
     '    "add", "subtract", "intersect", "add", "intersect"};', GARB),
    ('source-out-is-read-as-intersect', PROPS,
     '    "add", "subtract", "intersect", "exclude",\n    "add", "subtract",',
     '    "add", "subtract", "intersect", "exclude",\n    "add", "intersect",', GARB),
    ('the-standard-name-takes-the-legacy-words', PROPS,
     '{GARB_MASK_COMPOSITE, G_MASK_KEYWORD, kMaskComposite}',
     '{GARB_MASK_COMPOSITE, G_MASK_LEGACY, kWebkitMaskComposite}', GARB),
    ('destination-out-is-read', PROPS,
     '    "source-over", "source-out", "source-in", "xor", "destination-in", NULL};',
     '    "source-over", "source-out", "source-in", "xor", "destination-out", NULL};', GARB),
    ('a-mask-is-placed-by-the-background', 'userland/libflow/motion.c',
     'static const flow_backgrounds_t *layers_of(garb_prop_t p, const flow_style_t *s)\n{\n'
     '    return mask_placed(p) ? &s->masks.layers : &s->backgrounds;',
     'static const flow_backgrounds_t *layers_of(garb_prop_t p, const flow_style_t *s)\n{\n'
     '    return (void)mask_placed(p), &s->backgrounds;', FLOW),
    ('a-mask-size-is-not-mixed', 'userland/libflow/motion.c',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE: {\n        const flow_backgrounds_t *ga = layers_of(p, a), *gb = layers_of(p, b);\n        flow_backgrounds_t *gd',
     '    case GARB_BACKGROUND_SIZE: {\n        const flow_backgrounds_t *ga = layers_of(p, a), *gb = layers_of(p, b);\n        flow_backgrounds_t *gd', FLOW),
    ('a-mask-position-repaints-past-its-box', 'userland/libflow/motion.c',
     '    case GARB_MASK_POSITION_X: case GARB_MASK_POSITION_Y:\n    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_LEFT_RADIUS:',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_LEFT_RADIUS:', FLOW),
    ('a-mask-position-does-not-tick', 'userland/libflow/motion.c',
     '    case GARB_MASK_POSITION_X: case GARB_MASK_POSITION_Y:\n    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_COLOR:',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_COLOR:', FLOW),
    # A size repaints in place and ticks: a layout for a value a layout
    # places repaints the whole view (MASKS.md § 5).
    ('a-size-repaints-past-its-box', 'userland/libflow/motion.c',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_LEFT_RADIUS:',
     '    case GARB_BORDER_TOP_LEFT_RADIUS:', FLOW),
    ('a-mask-size-does-not-tick', 'userland/libflow/motion.c',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_COLOR:',
     '    case GARB_BACKGROUND_SIZE:\n    case GARB_BORDER_TOP_COLOR:', FLOW),
    ('a-background-size-does-not-tick', 'userland/libflow/motion.c',
     '    case GARB_BACKGROUND_SIZE: case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_COLOR:',
     '    case GARB_MASK_SIZE:\n    case GARB_BORDER_TOP_COLOR:', FLOW),
    ('a-picture-size-ticks', STYLE,
     '    if (sizes_a_picture(m, s))\n        return true;',
     '    if (false && sizes_a_picture(m, s))\n        return true;', FLOW),
    ('the-canvas-owner-is-not-marked', FLOWC,
     '        if (e->nboxes > 0 && !e->boxes[0]->unpainted && may_own_canvas(t, e->boxes[0]->node))',
     '        if (false && e->nboxes > 0 && !e->boxes[0]->unpainted && may_own_canvas(t, e->boxes[0]->node))', FLOW),
    # An anchored :has() under change tracking (select.h § AN ANCHORED
    # :has(); the oracle, test_garb_oracle.inc): where an anchor turned its
    # reach is marked; its bits are stored at every restyle and travel with
    # its record; its filters are the enclosing compounds' that must match
    # the same element, and type, id, class and attribute alone.
    ('a-turned-anchor-marks-no-subtree', CASCADE,
     '        if (c->anchors[i].down && !mark_subtree(c, a)) return false;\n',
     '', UPDATES),
    ('a-turned-anchor-marks-no-siblings', CASCADE,
     '        if (c->anchors[i].siblings && !mark_siblings(c, a->next)) return false;\n',
     '', UPDATES),
    ('a-restyle-stores-no-bits', CASCADE,
     '    anchor_bits(c, x, has_row(c, s));\n    if (!old.element_only) return true;',
     '    if (!old.element_only) return true;', UPDATES),
    ('growth-drops-the-bits', CASCADE,
     'for (uint32_t k = 0; k < w; k++) words[(size_t)at * w + k] = c->has_words[(size_t)i * w + k];',
     '', UPDATES),
    ('a-backshift-leaves-the-bits', CASCADE,
     '                c->has_words[(size_t)hole * c->has_width + k] = c->has_words[(size_t)at * c->has_width + k];',
     '                ;', UPDATES),
    ('the-second-word-folds-into-the-first', CASCADE,
     'out[i / 64] |= (uint64_t)1 << (i % 64);',
     'out[0] |= (uint64_t)1 << (i % 64);', UPDATES),
    ('filters-pass-into-a-non-subject', SELECT,
     'int32_t nmine = j == x->n - 1 ? nouter : 0;',
     'int32_t nmine = nouter;', UPDATES),
    # A bare anchor pays for itself: it is asked again where a change could
    # turn it, its turns mark its reach, and what it cost is counted.
    ('a-bare-anchor-marks-nothing', CASCADE,
     '        size_t before = c->ndirty;\n',
     '        size_t before = c->ndirty;\n        if (c->anchors[i].bare) continue;\n', UPDATES),
    ('a-bare-anchor-is-never-asked', CASCADE,
     '        if (garb_anchor_matches(&c->anchors[i], x)) out[i / 64] |= (uint64_t)1 << (i % 64);',
     '        if (!c->anchors[i].bare && garb_anchor_matches(&c->anchors[i], x)) out[i / 64] |= (uint64_t)1 << (i % 64);',
     UPDATES),
    ('a-bare-turn-is-not-counted', CASCADE,
     '            c->bare_work.turned++;\n',
     '', UPDATES),
    # A :has() walk is narrowed only where all it reaches lies under what
    # its leading `>`, `+` or `~` places, from what that compound matches;
    # an update visits each element once.
    ('a-sideways-walk-is-narrowed', SELECT,
     '        if (x->c[j].left != C_DESCENDANT && x->c[j].left != C_CHILD)\n            return false;',
     '        if (false)\n            return false;', UPDATES),
    ('a-narrowed-walk-skips-its-first-compound', SELECT,
     '    if (!compound_matches(sel, &x->c[0], start, el))\n        return false;',
     '', UPDATES),
    ('a-visit-repeats-in-an-update', CASCADE,
     '    if (s->visited == c->visit_epoch) return true;',
     '    if (false && s->visited == c->visit_epoch) return true;', UPDATES),
    ('a-pseudo-class-filters', SELECT,
     '    return s->kind == S_TYPE || s->kind == S_ID || s->kind == S_CLASS || s->kind == S_ATTR;',
     '    return s->kind == S_TYPE || s->kind == S_ID || s->kind == S_CLASS || s->kind == S_ATTR || s->kind == S_PSEUDO;',
     UPDATES),
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
                result = subprocess.run(['bash', '-c', harness], cwd=root, stdout=out, stderr=out, timeout=900)
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

print(f'\nlibgarb mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
