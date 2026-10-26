#!/usr/bin/env python3
"""One deliberate break at a time to 0f's P1a (INTERFACES_FORMS.md): the
prototype kinds and their interface objects, the Text and Comment
constructors, the node row (getRootNode, wholeText, splitText), libpage's
option selectedness and list walk, and the select row (the option's
members, the select's, the options collection and its indexed setter, the
Option factory); and to the page fixes after it: the lists' interfaces,
HTMLElement's members, template content, the selections, DOMParser,
createElementNS and the formless button's status line; to P1b's
EventTarget, its constructor and Window; and to P2's selections (where one
begins, setRangeText, the select event, and the field's caret and the
record kept as one) - against the
harnesses that hold each rule: a mutant is
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
SELECTION_C = 'userland/libdom/selection.c'
IMPLEMENTATION_C = 'userland/libdom/implementation.c'
YONDER_C = 'userland/apps/yonder/yonder.c'
FRAGMENT_C = 'userland/libhtml/fragment.c'
EVENT_C = 'userland/libdom/event.c'
UI_TEXT_C = 'userland/libos64/ui_text.c'
DOM = 'tools/test_dom_host.sh'
PAGE = 'tools/test_libpage_host.sh'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'
HTML = 'tools/test_html_fragment_host.sh'

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
    # 5. the page fixes: the lists' interfaces, DOMParser, the selections,
    # createElementNS, template content, HTMLElement's members, and the
    # formless button
    ('live-node-list-item-static', COLLECTION_C,
     '        return collection_method(ctx, self, argc, argv, 1, data);\n    return static_item',
     '        return JS_NULL;\n    return static_item', (DOM,)),
    ('child-nodes-a-collection', COLLECTION_C, '    if (!elements) value = as_node_list(dom, ctx, value);',
     '    (void)elements;', (DOM,)),
    ('by-name-a-collection', COLLECTION_C, '    return by_name ? as_node_list(dom, ctx, result) : result;',
     '    (void)by_name;\n    return result;', (DOM,)),
    ('live-node-list-no-length', COLLECTION_C,
     '    if (d_accessor(dom, ctx, prototype, "length", collection_method, 0, false) < 0 ||\n        d_method',
     '    if (d_accessor(dom, ctx, prototype, "length_", collection_method, 0, false) < 0 ||\n        d_method',
     (DOM,)),
    ('lists-swapped', CLASSIC_C, '        {"NodeList", dom->static_list->value}, {"HTMLCollection", collection},',
     '        {"NodeList", collection}, {"HTMLCollection", dom->static_list->value},', (DOM,)),
    ('hidden-on-element', MIXIN_C, '{"hidden", R_HIDDEN, true, HTML_ELEMENTS}', '{"hidden", R_HIDDEN, true, ELEMENTS}',
     (DOM,)),
    ('tab-index-html-only', MIXIN_C, '{"tabIndex", R_TAB_INDEX, true, HTML_OR_SVG}',
     '{"tabIndex", R_TAB_INDEX, true, HTML_ELEMENTS}', (DOM,)),
    ('title-on-element', CLASSIC_C,
     'd_accessor(dom,ctx,dom->prototypes[D_PROTO_HTML_ELEMENT]->value,"title",classic,C_TITLE,true)',
     'd_accessor(dom,ctx,el,"title",classic,C_TITLE,true)', (DOM,)),
    ('template-shares-htmlelement', NODE_C, '        if (node->tag == OS64_HTML_TAG_TEMPLATE) return D_PROTO_TEMPLATE;',
     '        if (node->tag == OS64_HTML_TAG_TEMPLATE) return D_PROTO_HTML_ELEMENT;', (DOM,)),
    ('content-is-template', MIXIN_C, '    return d_wrap(dom, ctx, self->template_contents);',
     '    return d_wrap(dom, ctx, (os64_html_node_t *)self);', (DOM,)),
    ('value-keeps-selection', STATE_C, '    if (changed) {\n        edit->range_start = edit->range_end = utf16_units(text, value_len);',
     '    if (changed && false) {\n        edit->range_start = edit->range_end = utf16_units(text, value_len);', (PAGE, DOM)),
    ('selection-counts-bytes', STATE_C, '            units += c >= 0xF0 ? 2 : 1;\n    }\n    return units;',
     '            units += 1;\n    }\n    return units;', (PAGE, DOM)),
    ('selection-read-failure-ignored', STATE_C, '    if (read < 0)\n        return read;\n    *units = utf16_units(value, len);',
     '    if (read < 0)\n        value = NULL;\n    *units = utf16_units(value, len);', (PAGE, DOM)),
    ('password-has-no-selection', STATE_C,
     '                                           input == OS64_PAGE_INPUT_PASSWORD));',
     '                                           input == OS64_PAGE_INPUT_TEXT));', (PAGE, DOM)),
    ('start-leaves-end', SELECTION_C, '    case X_START: start = a; if (end < start) end = start; break;',
     '    case X_START: start = a; break;', (DOM,)),
    ('select-selects-nothing', SELECTION_C, '    default: start = 0; end = UINT32_MAX;',
     '    default: start = 0; end = 0;', (DOM,)),
    ('no-selection-reads-zero', SELECTION_C, '        if (!has) return JS_NULL;',
     '        if (!has) return JS_NewUint32(ctx, 0);', (DOM,)),
    ('no-selection-set-quiet', SELECTION_C, '        return magic == X_SELECT ? JS_UNDEFINED : no_selection(ctx);',
     '        return JS_UNDEFINED;', (DOM,)),
    ('range-at-null', SELECTION_C, '        return d_error(ctx, "IndexSizeError", "The selection holds no range");',
     '        return JS_NULL;', (DOM,)),
    ('parser-takes-xml', IMPLEMENTATION_C, '    else if (kind > 0) result = d_error(ctx, "NotSupportedError"',
     '    else if (kind > 9) result = d_error(ctx, "NotSupportedError"', (DOM,)),
    ('parser-scripts-unmarked', IMPLEMENTATION_C, '        if (built && d_script_mark_tree(dom, html) < 0)',
     '        if (built && false && d_script_mark_tree(dom, html) < 0)', (DOM,)),
    ('parser-root-a-fragment', FRAGMENT_C,
     '    return h_parse_fragment(doc, context, utf8, len, scripting, true, budget, NULL, status);',
     '    return h_parse_fragment(doc, context, utf8, len, scripting, false, budget, NULL, status);', (DOM, HTML)),
    ('parser-root-quirks-inherited', FRAGMENT_C, '    if (keep_root)\n        temp->pub.quirks = OS64_HTML_NO_QUIRKS;',
     '    if (keep_root && false)\n        temp->pub.quirks = OS64_HTML_NO_QUIRKS;', (HTML,)),
    ('parser-root-attributes-dropped', FRAGMENT_C, '    pairs[0] = (FPair){root, pack_node(p, root, !keep_root)};',
     '    pairs[0] = (FPair){root, pack_node(p, root, !keep_root || root != NULL)};', (DOM, HTML)),
    ('ns-svg-as-html', IMPLEMENTATION_C,
     '    else if (has_ns && same(ns->data, ns->length, NS_SVG)) *space = OS64_HTML_NS_SVG;',
     '    else if (has_ns && same(ns->data, ns->length, NS_SVG)) *space = OS64_HTML_NS_HTML;', (DOM,)),
    ('ns-empty-is-a-namespace', IMPLEMENTATION_C,
     '    if (has_ns && ns.length == 0) has_ns = false;\n    JSValue result = JS_UNDEFINED;\n    if (argc > 2',
     '    JSValue result = JS_UNDEFINED;\n    if (argc > 2', (DOM,)),
    ('ns-element-owner-lost', IMPLEMENTATION_C,
     '        if (!JS_IsException(result)) d_made_in(dom, element, document);\n    }\n    d_string_free(dom, &ns);',
     '    }\n    d_string_free(dom, &ns);', (DOM,)),
    ('formless-button-quiet-without-scripts', YONDER_C,
     '        bool scripts_own_it = scripts_live() && (', '        bool scripts_own_it = true && (', (SCRIPTS,)),
    ('formless-button-refused-with-scripts', YONDER_C,
     '        bool scripts_own_it = scripts_live() && (', '        bool scripts_own_it = false && (', (SCRIPTS,)),
    # P1b: EventTarget.prototype holds the methods, and every target reaches it
    ('target-methods-on-node', EVENT_C,
     '        if (d_method(dom, ctx, target, targets[i].name, target_method, targets[i].argc, targets[i].magic) < 0)',
     '        if (d_method(dom, ctx, dom->prototypes[D_PROTO_NODE]->value, targets[i].name, target_method,\n'
     '                     targets[i].argc, targets[i].magic) < 0)', (DOM,)),
    ('node-off-event-target', EVENT_C,
     '        JS_SetPrototype(ctx, dom->prototypes[D_PROTO_NODE]->value, target) < 0 ||', '', (DOM,)),
    ('global-off-window', EVENT_C,
     '        JS_SetPrototype(ctx, global, dom->window_prototype->value) < 0)', '        false)', (DOM,)),
    ('window-past-window-prototype', EVENT_C,
     '        JS_SetPrototype(ctx, global, dom->window_prototype->value) < 0)',
     '        JS_SetPrototype(ctx, global, target) < 0)', (DOM,)),
    ('window-prototype-plain', EVENT_C,
     '    dom->window_prototype = d_retain(dom, ctx, JS_NewObjectProto(ctx, target));',
     '    dom->window_prototype = d_retain(dom, ctx, JS_NewObject(ctx));', (DOM,)),
    ('plain-target-left-on-object', EVENT_C,
     '    return JS_SetPrototype(ctx, prototype, target) < 0 ? -1 : 0;\n}', '    return 0;\n}', (DOM,)),
    ('plain-target-reached-ignored', EVENT_C, '    if (reached) return 0;', '', (DOM,)),
    ('event-target-own-class-walked', EVENT_C,
     '    if (JS_VALUE_GET_PTR(prototype) == JS_VALUE_GET_PTR(target)) return 0;', '', (DOM,)),
    ('construct-ignores-new-target', EVENT_C,
     '    JSValue object = JS_NewObjectProtoClass(ctx, JS_IsObject(prototype) ? (JSValueConst)prototype',
     '    JSValue object = JS_NewObjectProtoClass(ctx, false ? (JSValueConst)prototype', (DOM,)),
    ('construct-without-entry', EVENT_C,
     '    d_event_target_init(dom, &t->target, object);\n    JS_SetOpaque(object, t);',
     '    JS_SetOpaque(object, t);', (DOM,)),
    ('event-target-unconstructible', EVENT_C,
     '    JSValue event_target = d_interface_new(ctx, "EventTarget", 0, target_construct, target, JS_UNDEFINED);',
     '    JSValue event_target = d_interface_new(ctx, "EventTarget", 0, false ? target_construct : NULL, target,\n'
     '                                           JS_UNDEFINED);', (DOM,)),
    ('window-constructible', EVENT_C,
     '    JSValue window = d_interface_new(ctx, "Window", 0, NULL, dom->window_prototype->value, event_target);',
     '    JSValue window = d_interface_new(ctx, "Window", 0, target_construct, dom->window_prototype->value,\n'
     '                                     event_target);', (DOM,)),
    ('interfaces-not-adopted', EVENT_C,
     '                     JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(function) && JS_SetPrototype(ctx, value, parent) < 0)',
     '                     JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(function) && false)', (DOM,)),
    ('global-keeps-its-tag', EVENT_C,
     '    return adopted < 0 ? -1 : untag_global(ctx, global);', '    return adopted < 0 || (false && untag_global(ctx, global) < 0) ? -1 : 0;', (DOM,)),
    # P2: where a selection begins and how a value moves it
    ('selection-begins-at-end', STATE_C, '        *start = *end = 0;\n        *direction = OS64_PAGE_DIRECTION_NONE;',
     '        *start = *end = units;\n        *direction = OS64_PAGE_DIRECTION_NONE;', (PAGE, DOM)),
    ('same-value-moves-selection', STATE_C, '    if (changed) {\n        edit->range_start',
     '    if (true) {\n        edit->range_start', (PAGE,)),
    ('clamp-not-kept', STATE_C, '    edit->range_start = *start;\n    edit->range_end = *end;\n    return 0;', '    return 0;',
     (PAGE,)),
    # P2: setRangeText
    ('range-text-two-arguments', SELECTION_C, '    if (argc < 1 || argc == 2) return', '    if (argc < 1) return', (DOM,)),
    ('range-text-start-past-end', SELECTION_C, '    } else if (start > end) {', '    } else if (false && start > end) {',
     (DOM,)),
    ('range-text-select-collapses', SELECTION_C, '    case M_SELECT: sel_start = start; sel_end = new_end; break;',
     '    case M_SELECT: sel_start = sel_end = new_end; break;', (DOM,)),
    ('range-text-start-is-end', SELECTION_C, '    case M_START: sel_start = sel_end = start; break;',
     '    case M_START: sel_start = sel_end = new_end; break;', (DOM,)),
    ('range-text-preserve-stays', SELECTION_C,
     '        if (sel_start > end) sel_start = (uint32_t)((int64_t)sel_start + delta);',
     '        if (false) sel_start = (uint32_t)((int64_t)sel_start + delta);', (DOM,)),
    ('range-text-preserve-no-snap', SELECTION_C, '        else if (sel_end > start) sel_end = new_end;',
     '        else if (false) sel_end = new_end;', (DOM,)),
    ('range-text-bytes', SELECTION_C, '        seen += width == 4 ? 2 : 1;', '        seen += 1;', (DOM,)),
    ('range-text-mode-unchecked', SELECTION_C, '        if (mode < 0) return JS_ThrowTypeError',
     '        if (mode < -1) return JS_ThrowTypeError', (DOM,)),
    # P2: the select event
    ('select-never-queued', SELECTION_C, '    return d_timer_add(dom, ctx, fired, 0) == 0 ? -1 : 0;\n}',
     '    JS_FreeValue(ctx, fired);\n    return 0;\n}', (DOM,)),
    ('select-on-every-set', SELECTION_C,
     '        (now_start == start && now_end == end && now_direction == direction))\n        return 0;',
     '        (false && now_start == start && now_end == end && now_direction == direction))\n        return 0;', (DOM,)),
    ('select-cancelable', SELECTION_C, 'd_event_new(dom, ctx, "select", true, false, true)',
     'd_event_new(dom, ctx, "select", true, true, true)', (DOM,)),
    # P2: the field's caret and the record are one (yonder's forms)
    ('hook-never-pulls', YONDER_C, '    forms_pull_selections();\n    return true;',
     '    (void)forms_pull_selections;\n    return true;', (SCRIPTS,)),
    ('hook-never-pushes', YONDER_C, '        field_selection_push(fw, written);',
     '        if (false) field_selection_push(fw, written);', (SCRIPTS,)),
    ('hook-write-not-forced', YONDER_C, '    if (!written && same_selection(now, fw->field_seen))',
     '    if (same_selection(now, fw->field_seen))', (SCRIPTS,)),
    ('hook-backward-forward', YONDER_C, '    if (record.direction == OS64_PAGE_DIRECTION_BACKWARD)', '    if (false)',
     (SCRIPTS,)),
    ('hook-password-bytes', YONDER_C, '    return fw->kind == FW_PASSWORD ? chars : i;',
     '    return fw->kind == FW_PASSWORD ? i + (chars & 0) : i;', (SCRIPTS,)),
    ('hook-first-sight-ignores-record', YONDER_C, '    if (!record_selection(fw, &record)) {\n        field_selection_note(fw);',
     '    if (!fw->seen || !record_selection(fw, &record)) {\n        field_selection_note(fw);', (SCRIPTS,)),
    ('hook-untouched-moves', YONDER_C, '    if (same_selection(record, fw->record_seen))\n        return;',
     '    if (fw->seen && same_selection(record, fw->record_seen))\n        return;', (SCRIPTS,)),
    ('hook-rewrite-forgets-caret', YONDER_C, '                if (fw->presented)\n                    fw->record_seen.start = UINT32_MAX;',
     '                if (false)\n                    fw->record_seen.start = UINT32_MAX;', (SCRIPTS,)),
    ('select-range-never-selects', UI_TEXT_C, '\ttf->selected = tf->anchor != tf->cursor;', '\ttf->selected = false;',
     (SCRIPTS,)),
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
