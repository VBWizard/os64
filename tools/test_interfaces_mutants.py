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
record kept as one); and to P3a's controls (their reflections by each of
HTML's rules, indeterminate, autocomplete, form and labels); to P3b's form,
label, fieldset, legend and output, the form's controls collection and
RadioNodeList; to the node row's element names (localName,
namespaceURI, prefix); and to P4a's constraint validation (libpage's
judge and its state, the binding's pattern, ValidityState, members and
invalid, and yonder's validated submission and report) - against the
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
CONTROLS_C = 'userland/libdom/controls.c'
WINDOW_C = 'userland/libdom/window.c'
FORMS_C = 'userland/libdom/forms.c'
RESOLVE_C = 'userland/libpage/resolve.c'
SHEETS_C = 'userland/libdom/sheets.c'
VALIDITY_C = 'userland/libdom/validity.c'
SUBMIT_C = 'userland/libpage/submit.c'
ACTIVATE_C = 'userland/libpage/activate.c'
SCRIPTS_C = 'userland/apps/yonder/scripts.c'
VALUE_C = 'userland/libpage/value.c'
RANGE_C = 'userland/libpage/range.c'
UI_CONTROLS_C = 'userland/libos64/ui_controls.c'
DOM = 'tools/test_dom_host.sh'
PAGE = 'tools/test_libpage_host.sh'
SCRIPTS = 'tools/test_yonder_scripts_host.sh'
HTML = 'tools/test_html_fragment_host.sh'
UI_TEXT = 'tools/test_ui_text_host.py --real'

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
    ('range-text-mode-unchecked', SELECTION_C, '        if (mode < 0) {\n            JS_FreeCString(ctx, added);',
     '        if (mode < -1) {\n            JS_FreeCString(ctx, added);', (DOM,)),
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
    ('writes-not-counted', STATE_C, '    edit->range_kept = true;\n    edit->range_writes++;\n    return 0;',
     '    edit->range_kept = true;\n    return 0;', (PAGE, SCRIPTS)),
    ('hook-writes-ignored', YONDER_C, ' && a.direction == b.direction && a.writes == b.writes;',
     ' && a.direction == b.direction;', (SCRIPTS,)),
    ('password-edits-the-end', YONDER_C,
     '    size_t from = password_byte(fw, lo), to = password_byte(fw, hi);',
     '    size_t from = fw->secret_len + 0 * password_byte(fw, lo), to = fw->secret_len + 0 * password_byte(fw, hi);',
     (SCRIPTS,)),
    ('hook-press-chooses-nothing', YONDER_C,
     '    if (ev->type == OS64_GUI_EVENT_MOUSE_BUTTON_DOWN || g.ui.focus != had)\n        field_selection_chosen(g.ui.focus);',
     '    if (g.ui.focus != had && false)\n        field_selection_chosen(g.ui.focus);', (SCRIPTS,)),
    ('hook-chosen-not-forced', YONDER_C, '        fw->field_seen.start = UINT32_MAX;\n        return;',
     '        return;', (SCRIPTS,)),
    ('select-range-never-selects', UI_TEXT_C, '\ttf->selected = tf->anchor != tf->cursor;', '\ttf->selected = false;',
     (SCRIPTS,)),
    # P3b. the form's, label's, fieldset's, legend's and output's own
    ('form-method-missing-empty', CONTROLS_C, '    {"method", "method", K_ENUM, FO, kMethods, "get", "get", 0},',
     '    {"method", "method", K_ENUM, FO, kMethods, "", "get", 0},', (DOM,)),
    ('form-autocomplete-off', CONTROLS_C, '{"autocomplete", "autocomplete", K_ENUM, FO, kAutocomplete, "on", "on", 0},',
     '{"autocomplete", "autocomplete", K_ENUM, FO, kAutocomplete, "off", "on", 0},', (DOM,)),
    ('output-has-no-labels', CONTROLS_C, '{"labels", M_LABELS, false, IN | TA | SE | BU | OU},',
     '{"labels", M_LABELS, false, IN | TA | SE | BU},', (DOM,)),
    ('image-button-listed', CLASSIC_C, '        return d_listed(node) && !image_button(node) && d_form_owner(node) == root;',
     '        return d_listed(node) && (!image_button(node) || true) && d_form_owner(node) == root;', (DOM,)),
    ('fieldset-drops-image-buttons', CLASSIC_C, '    case D_QUERY_FIELDSET: return d_listed(node);',
     '    case D_QUERY_FIELDSET: return d_listed(node) && !image_button(node);', (DOM,)),
    ('fieldset-not-listed', CLASSIC_C,
     '    case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_FIELDSET: case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_OBJECT:',
     '    case OS64_HTML_TAG_BUTTON: case OS64_HTML_TAG_INPUT: case OS64_HTML_TAG_OBJECT:', (DOM,)),
    ('form-has-no-index', CLASSIC_C, '        ? d_index_key(ctx, atom, &index) : 0;', '        ? 0 * d_index_key(ctx, atom, &index) : 0;',
     (DOM,)),
    ('builtins-win', CLASSIC_C, '    if (!(kind == D_QUERY_CONTROLS && entry->dom->controls_prototype != NULL)) {',
     '    if (true || !(kind == D_QUERY_CONTROLS && entry->dom->controls_prototype != NULL)) {', (DOM,)),
    ('form-names-unlisted', CLASSIC_C,
     '    DValue *entry = modern_form(object);\n    /* The ask below, for the ordinary keys alone. */',
     '    DValue *entry = modern_form(object) != NULL ? NULL : NULL;\n    /* The ask below, for the ordinary keys alone. */',
     (DOM,)),
    ('form-name-redefinable', CLASSIC_C,
     '    int named = numeric == 0 && entry != NULL ? named_property(ctx, NULL, object, atom) : 0;',
     '    int named = numeric == 0 && entry != NULL ? 0 * named_property(ctx, NULL, object, atom) : 0;', (DOM,)),
    ('unsupported-index-definable', CLASSIC_C,
     '    int numeric = entry != NULL ? d_index_key(ctx, atom, &index) : 0;',
     '    int numeric = entry != NULL ? 0 * d_index_key(ctx, atom, &index) : 0;', (DOM,)),
    ('index-named-twice', CLASSIC_C, '            for (uint32_t s = 0; s < n && !seen; s++)',
     '            for (uint32_t s = count; s < n && !seen; s++)', (DOM,)),
    ('expando-named-twice', CLASSIC_C, '            for (uint32_t s = 0; s < own_count && !seen; s++)',
     '            for (uint32_t s = own_count; s < own_count && !seen; s++)', (DOM,)),
    ('base-uri-loses-fragment', MIXIN_C,
     '    JSValue result = os64_page_base_url_in(dom->document, window ? NULL : d_node_document(dom, self), url, base,',
     '    JSValue result = os64_page_base_in(dom->document, url, base,', (DOM,)),
    ('windowless-base-ignored', MIXIN_C, 'os64_page_base_url_in(dom->document, window ? NULL : d_node_document(dom, self),',
     'os64_page_base_url_in(dom->document, window ? NULL : NULL,', (DOM,)),
    ('base-fragment-is-the-documents', RESOLVE_C, '    page->base_fragment = os64_strchr(clean, \'#\');\n    page->base_hierarchical',
     '    page->base_hierarchical', (PAGE,)),
    ('form-index-deletable', CLASSIC_C, '    return named < 0 ? -1 : !named;', '    return named < 0 ? -1 : 1;', (DOM,)),
    ('elements-plain-collection', CLASSIC_C, '        if (kind == D_QUERY_CONTROLS && dom->controls_prototype != NULL &&',
     '        if (kind == D_QUERY_CONTROLS && dom->controls_prototype != NULL && false &&', (DOM,)),
    ('named-item-first-only', COLLECTION_C,
     '    if (radios && count > 1 && query->kind == D_QUERY_CONTROLS && query->named == NULL &&\n',
     '    if (radios && count > 1 && query->kind == D_QUERY_CONTROLS && query->named == NULL && false &&\n',
     (DOM,)),
    ('base-named-item-radios', COLLECTION_C,
     '? named_item(query, ctx, name.data != NULL ? name.data : "", magic == 3)',
     '? named_item(query, ctx, name.data != NULL ? name.data : "", true)', (DOM,)),
    ('radio-list-made-each-time', COLLECTION_C, '        if (os64_streq(made->named, name)) return JS_DupValue(ctx, made->entry->value);',
     '        if (os64_streq(made->named, name) && false) return JS_DupValue(ctx, made->entry->value);', (DOM,)),
    ('radio-value-counts-checkboxes', COLLECTION_C, '        if (!is_radio(node)) continue;\n',
     '        if (!is_radio(node) && false) continue;\n', (DOM,)),
    ('radio-set-any-value', COLLECTION_C,
     '            if (length != wanted.length || os64_memcmp(text, wanted.data != NULL ? wanted.data : "", length) != 0)\n                continue;',
     '            if (false)\n                continue;', (DOM,)),
    ('node-list-has-names', COLLECTION_C, '        if (!query->elements || query->node_list) return 0;',
     '        if (!query->elements) return 0;', (DOM,)),
    ('output-value-keeps-no-default', FORMS_C,
     '                  keep_default(dom, ctx, entry, kept.data != NULL ? kept.data : "", kept.length);',
     '                  true;', (DOM,)),
    ('output-default-always-text', FORMS_C, '    if (entry->output_default_set)\n        return JS_NewStringLen(',
     '    if (entry->output_default_set && false)\n        return JS_NewStringLen(', (DOM,)),
    ('label-form-its-own', FORMS_C, '        return d_wrap(dom, ctx, control != NULL ? d_form_owner(control) : NULL);',
     '        return d_wrap(dom, ctx, control != NULL ? d_form_owner(node) : NULL);', (DOM,)),
    ('submitter-unchecked', WINDOW_C, '        if (!submit_button(submitter)) return JS_ThrowTypeError(',
     '        if (!submit_button(submitter) && false) return JS_ThrowTypeError(', (DOM,)),
    ('submitter-of-any-form', WINDOW_C, '        if (d_form_owner(submitter) != node)\n',
     '        if (d_form_owner(submitter) != node && false)\n', (DOM,)),
    ('detached-form-submits', WINDOW_C, '    if (!d_window_document(dom, form) || !d_connected(form)) return 1;',
     '    if (!d_window_document(dom, form)) return 1;', (DOM,)),
    # The page's style sheets: document.styleSheets, a sheet, a node's sheet.
    ('sheets-match-anything', COLLECTION_C, '    if (query->kind == D_QUERY_SHEETS) return os64_page_names_sheet(node);',
     '    if (query->kind == D_QUERY_SHEETS) return true;', (DOM,)),
    ('sheets-are-nodes', COLLECTION_C, '    return query->kind == D_QUERY_SHEETS ? d_sheet(',
     '    return false ? d_sheet(', (DOM,)),
    ('sheet-made-each-time', SHEETS_C, '    if (owner->sheet != NULL) return JS_DupValue(ctx, owner->sheet->value);',
     '    if (false) return JS_DupValue(ctx, owner->sheet->value);', (DOM,)),
    ('sheet-list-made-each-time', COLLECTION_C,
     '    if (entry->style_sheets != NULL) return JS_DupValue(ctx, entry->style_sheets->entry->value);',
     '    if (false) return JS_DupValue(ctx, entry->style_sheets->entry->value);', (DOM,)),
    ('sheet-list-has-names', COLLECTION_C,
     "    /* Its entries are reached by index alone, as a NodeList's are. */\n    query->node_list = true;",
     "    /* Its entries are reached by index alone, as a NodeList's are. */", (DOM,)),
    ('removed-sheet-keeps-owner', SHEETS_C,
     '    case S_OWNER_NODE: return associated(node) ? d_wrap(dom, ctx, node) : JS_NULL;',
     '    case S_OWNER_NODE: return d_wrap(dom, ctx, node);', (DOM,)),
    ('detached-node-has-sheet', SHEETS_C, '    return root->kind == OS64_HTML_DOCUMENT;',
     '    return root->kind == OS64_HTML_DOCUMENT || true;', (DOM,)),
    ('empty-title-is-a-title', SHEETS_C, "    return attr != NULL && attr->value[0] != '\\0' ? JS_NewString",
     '    return attr != NULL ? JS_NewString', (DOM,)),
    ('sheet-href-unresolved', SHEETS_C, '        ? JS_NewString(ctx, absolute) : JS_NewString(ctx, attr->value);',
     '        ? JS_NewString(ctx, attr->value) : JS_NewString(ctx, absolute);', (DOM,)),
    # the node row: an element's names
    ('names-swapped', MIXIN_C, "    const char *name = magic == E_LOCAL_NAME ? self->name : magic == E_PREFIX ? self->prefix",
     "    const char *name = magic == E_PREFIX ? self->name : magic == E_LOCAL_NAME ? self->prefix", (DOM,)),
    ('namespace-always-html', MIXIN_C, "                                                                             : os64_html_namespace_uri(self);",
     '                                                                             : "http://www.w3.org/1999/xhtml";',
     (DOM,)),
    ('names-of-any-node', MIXIN_C,
     '    if (!is_element(self)) return JS_ThrowTypeError(ctx, "An element\'s name requires an element");\n',
     '', (DOM,)),
    # P3a. the reflections' rules
    ('enum-case-sensitive', CONTROLS_C, 'if (os64_strlen(*word) == length && same_folded(*word, attr->value, length))',
     'if (os64_strlen(*word) == length && os64_memcmp(*word, attr->value, length) == 0)', (DOM,)),
    ('enum-invalid-is-missing', CONTROLS_C, '        return JS_NewString(ctx, row->invalid);',
     '        return JS_NewString(ctx, row->missing);', (DOM,)),
    ('form-action-empty-as-written', CONTROLS_C, "        if (attr == NULL || attr->value[0] == '\\0')",
     '        if (attr == NULL)', (DOM,)),
    ('integer-no-leading-space', CONTROLS_C, '    while (is_space(*text)) text++;\n    bool negative',
     '    bool negative', (DOM,)),
    ('minus-zero-negative', CONTROLS_C, '    return negative && value != 0 ? -1 : value;',
     '    return negative ? -1 : value;', (DOM,)),
    ('integer-past-range-kept', CONTROLS_C, '        return JS_NewInt32(ctx, value >= 0 && value <= INT32_MAX ? (int32_t)value : -1);',
     '        return JS_NewInt32(ctx, value >= 0 ? (int32_t)value : -1);', (DOM,)),
    ('limited-zero-read', CONTROLS_C, '        int64_t least = row->kind == K_UNSIGNED ? 0 : 1;',
     '        int64_t least = 0;', (DOM,)),
    ('negative-set-allowed', CONTROLS_C, '        if (number < 0) return d_error(ctx, "IndexSizeError"',
     '        if (number < -2) return d_error(ctx, "IndexSizeError"', (DOM,)),
    ('zero-set-allowed', CONTROLS_C, '        if (number == 0 && row->kind == K_LIMITED_UNSIGNED)',
     '        if (number == 0 && row->kind == K_UNSIGNED)', (DOM,)),
    ('past-range-set-kept', CONTROLS_C, '                          number >= least && number <= INT32_MAX ? number : row->number);',
     '                          number >= least ? number : row->number);', (DOM,)),
    ('brand-unchecked', CONTROLS_C, '        if ((row->on & kind) == 0) return JS_ThrowTypeError(ctx, "Illegal invocation");',
     '        if ((row->on & kind) == 0 && false) return JS_ThrowTypeError(ctx, "Illegal invocation");', (DOM,)),
    # P3a. the members that are no plain reflection
    ('text-length-in-bytes', CONTROLS_C, 'JS_NewUint32(ctx, d_utf16_length(text, length));',
     'JS_NewUint32(ctx, (uint32_t)length + 0 * d_utf16_length(text, length));', (DOM,)),
    ('text-default-null-empty', CONTROLS_C, '        JSValue text = JS_ToString(ctx, value);',
     '        JSValue text = JS_IsNull(value) ? JS_NewString(ctx, "") : JS_ToString(ctx, value);', (DOM,)),
    ('autocomplete-any-contact-kind', CONTROLS_C,
     '    if (contact && first > 0 && token_in(tokens[first - 1], kContactKinds)) first--;',
     '    if ((contact || true) && first > 0 && token_in(tokens[first - 1], kContactKinds)) first--;', (DOM,)),
    ('autocomplete-hidden-on', CONTROLS_C, '    if (count == 1 && !hidden && ',
     '    if (count == 1 && (!hidden || true) && ', (DOM,)),
    ('image-width-any-type', CONTROLS_C,
     '        return image_input(node) ? reflection_get(dom, ctx, node, row) : JS_NewUint32(ctx, 0);',
     '        return image_input(node) || true ? reflection_get(dom, ctx, node, row) : JS_NewUint32(ctx, 0);', (DOM,)),
    # P3a. indeterminate
    ('click-keeps-indeterminate', WINDOW_C,
     '        if (status >= 0 && checkbox) status = os64_page_node_set_indeterminate(dom->state, node, false);',
     '        if (status >= 0 && checkbox && false) status = os64_page_node_set_indeterminate(dom->state, node, false);',
     (DOM,)),
    ('cancel-forgets-indeterminate', WINDOW_C,
     '                status = os64_page_node_set_indeterminate(dom->state, node, was_indeterminate);',
     '                status = os64_page_node_set_indeterminate(dom->state, node, was_indeterminate && false);', (DOM,)),
    ('tick-keeps-indeterminate', CORE_C, '    if (status >= 0 && checkbox)\n        (void)os64_page_node_set_indeterminate',
     '    if (status >= 0 && checkbox && false)\n        (void)os64_page_node_set_indeterminate', (PAGE, SCRIPTS)),
    ('clone-drops-indeterminate', STATE_C, '                fresh->indeterminate = old != NULL && old->indeterminate;',
     '                fresh->indeterminate = false;', (PAGE, DOM)),
    ('indeterminate-unpublished', CORE_C, '    c->indeterminate = edit != NULL && edit->indeterminate;',
     '    c->indeterminate = false;', (PAGE, SCRIPTS)),
    ('radio-drawn-indeterminate', YONDER_C, '                                               c->indeterminate && c->input == OS64_PAGE_INPUT_CHECKBOX);',
     '                                               c->indeterminate);', (SCRIPTS,)),
    ('yonder-cancel-forgets-indeterminate', YONDER_C, '                                                           (in.was & WAS_INDETERMINATE) != 0);',
     '                                                           false);', (SCRIPTS,)),
    ('widget-click-keeps-indeterminate', UI_CONTROLS_C, '    cb->indeterminate = false;\n    if (cb->on_change)',
     '    if (cb->on_change)', (UI_TEXT,)),
    # P3a. form and labels
    ('hidden-input-labelable', CONTROLS_C,
     '        return type == NULL || os64_strlen(type->value) != 6 || !same_folded(type->value, "hidden", 6);',
     '        return type == NULL || true;', (DOM,)),
    ('label-for-searches-on', CONTROLS_C, '            if (id != NULL && os64_streq(id->value, named->value)) return labelable(at) ? at : NULL;',
     '            if (id != NULL && os64_streq(id->value, named->value) && labelable(at)) return at;', (DOM,)),
    ('method-no-dialog', CONTROLS_C, 'static const char *const kMethods[] = {"get", "post", "dialog", NULL};',
     'static const char *const kMethods[] = {"get", "post", NULL};', (DOM,)),
    ('label-empty-for-matches', CONTROLS_C, "    if (named != NULL && named->value[0] == '\\0') return NULL;\n",
     '', (DOM,)),
    ('labels-root-left-out', COLLECTION_C,
     '    if (at == NULL) return query->kind == D_QUERY_LABELS ? query->root : query->root->first_child;',
     '    if (at == NULL) return query->root->first_child;', (DOM,)),
    ('labels-not-rerooted', COLLECTION_C,
     '    if ((query->kind == D_QUERY_CONTROLS || query->kind == D_QUERY_LABELS) && query->owner != NULL) {',
     '    if (query->kind == D_QUERY_CONTROLS && query->owner != NULL) {', (DOM,)),
    ('labels-made-each-time', COLLECTION_C, '    entry->labels = query;\n    return value;', '    return value;', (DOM,)),
    ('form-empty-id-matches', WINDOW_C, "        if (named->value[0] == '\\0') return NULL;\n", '', (DOM,)),
    ('form-detached-attribute-wins', WINDOW_C, '    if (named != NULL && d_connected(node)) {',
     '    if (named != NULL) {\n        if (!d_connected(node)) return NULL;', (DOM,)),
    ('radio-owner-empty-id-matches', STATE_C,
     "        for (const os64_html_node_t *at = root; at != NULL && form[0] != '\\0'; at = tree_next(root, at)) {",
     '        for (const os64_html_node_t *at = root; at != NULL; at = tree_next(root, at)) {', (PAGE,)),
    # P4a. constraint validation: libpage's judge and its state
    ('unnamed-radio-missing', STATE_C,
     "    if (name == NULL || name[0] == '\\0')\n        return false;\n    bool required = false;",
     '    (void)name;\n    bool required = false;', (PAGE, DOM)),
    ('unnamed-radio-blocks-submission', SUBMIT_C,
     "    if (name == NULL || name[0] == '\\0')\n        return true;\n    int32_t head",
     '    (void)name;\n    int32_t head', (PAGE,)),
    ('radio-group-needs-own-tick', STATE_C, '        if (checked_raw(state, at))\n            return false;',
     '        if (checked_raw(state, at) && at == node)\n            return false;', (PAGE, DOM)),
    ('readonly-bars-text-only', CORE_C,
     '    bool readonly = c->readonly || (c->element == OS64_PAGE_EL_INPUT && p_has_attr(c->node, "readonly"));',
     '    bool readonly = c->readonly;', (PAGE,)),
    ('node-readonly-bars-text-only', SUBMIT_C,
     '    bool readonly = element != OS64_PAGE_EL_SELECT && element != OS64_PAGE_EL_BUTTON && p_has_attr(n, "readonly");',
     '    bool readonly = node_readonly(n, element, input);', (PAGE, DOM)),
    ('select-readonly-barred', SUBMIT_C,
     '    bool readonly = element != OS64_PAGE_EL_SELECT && element != OS64_PAGE_EL_BUTTON && p_has_attr(n, "readonly");',
     '    bool readonly = p_has_attr(n, "readonly");', (DOM,)),
    ('legend-shields-nothing', SUBMIT_C, '        if (child != legend)\n            return true;',
     '        if (child != legend || true)\n            return true;', (DOM,)),
    ('missing-ignores-mutability', SUBMIT_C, '    } else if (required && len == 0 && is_mutable &&',
     '    } else if (required && len == 0 && (is_mutable || true) &&', (DOM,)),
    ('tick-missing-needs-mutability', SUBMIT_C,
     '        if (required && os64_page_node_checked(state, n, &on) >= 0 && !on)',
     '        if (required && is_mutable && os64_page_node_checked(state, n, &on) >= 0 && !on)', (PAGE, DOM)),
    ('email-single-takes-a-list', SUBMIT_C,
     '        !(p_has_attr(n, "multiple") ? email_list_valid(value, len) : email_valid(value, len)))',
     '        !email_list_valid(value, len))', (DOM,)),
    ('email-label-starts-with-hyphen', SUBMIT_C, "            if ((label == 0 && c == '-') || ++label > 63)",
     '            if (++label > 63)', (DOM,)),
    ('url-host-optional', SUBMIT_C, "    return at < len && s[at] != '?' && s[at] != '#';", '    return true;', (DOM,)),
    ('url-scheme-optional', SUBMIT_C, "    if (at == len || s[at] != ':')\n        return false;",
     "    if (at == len)\n        return true;", (DOM,)),
    ('pattern-on-any-type', SUBMIT_C,
     '    if (matcher == NULL || pattern == NULL || len == 0 || !pattern_applies(input))',
     '    if (matcher == NULL || pattern == NULL || len == 0 || (!pattern_applies(input) && false))', (DOM,)),
    ('pattern-judges-empty', SUBMIT_C,
     '    if (matcher == NULL || pattern == NULL || len == 0 || !pattern_applies(input))',
     '    if (matcher == NULL || pattern == NULL || !pattern_applies(input))', (DOM,)),
    ('email-list-matched-whole', SUBMIT_C,
     '    bool list = input == OS64_PAGE_INPUT_EMAIL && p_has_attr(n, "multiple");', '    bool list = false;', (DOM,)),
    ('type-change-keeps-bad-input', STATE_C, '    if (typed != NULL)\n        typed->bad_input = false;',
     '    if (typed != NULL && false)\n        typed->bad_input = false;', (PAGE,)),
    ('script-text-is-option-text', SUBMIT_C,
     '        } else if (n->kind == OS64_HTML_ELEMENT && !p_is(n, OS64_HTML_TAG_SCRIPT) && !text_blank(n->first_child)) {',
     '        } else if (n->kind == OS64_HTML_ELEMENT && !text_blank(n->first_child)) {', (PAGE,)),
    ('host-validation-unbudgeted', VALIDITY_C,
     '        os64_js_call(dom->runtime, OS64_JS_ABI_ID, dom->validate->value, JS_UNDEFINED, 1, &argument, &invalid_form,\n'
     '                     &call);',
     '        invalid_form = d_form_validate(dom, ctx, form, true) == 0;', (DOM,)),
    ('bad-input-unrecorded', STATE_C, '    bool bad = user && value_len == 0 && unparsed(input, utf8, len);',
     '    bool bad = false && unparsed(input, utf8, len);', (PAGE,)),
    ('bad-input-from-script', STATE_C, '    bool bad = user && value_len == 0 && unparsed(input, utf8, len);',
     '    bool bad = value_len == 0 && unparsed(input, utf8, len);', (PAGE, DOM)),
    ('white-space-is-bad-input', STATE_C,
     "            return true;\n    return false;\n}\n\nint64_t os64_page_node_set_value",
     "            return true;\n    return len != 0;\n}\n\nint64_t os64_page_node_set_value", (PAGE,)),
    ('reset-keeps-bad-input', STATE_C, '            edit->bad_input = false;\n', '', (PAGE,)),
    ('custom-leaked', STATE_C, '        p_state_dealloc(state, edit->custom);\n', '', (PAGE,)),
    ('custom-unjudged', SUBMIT_C,
     '    if (p_node_custom(state, n, &custom_len) != NULL)\n        flags |= OS64_PAGE_VALIDITY_CUSTOM;',
     '    (void)p_node_custom(state, n, &custom_len);', (PAGE, DOM)),
    ('submission-judges-missing-only', SUBMIT_C,
     '        if (judge(page->state, c->node, c->element, c->input, c->value, c->value_len, radio_missing, NULL,\n'
     '                  NULL) != 0) {',
     '        if (judge(page->state, c->node, c->element, c->input, c->value, c->value_len, radio_missing, NULL,\n'
     '                  NULL) & (OS64_PAGE_VALIDITY_MISSING | OS64_PAGE_VALIDITY_TOO_LONG)) {', (PAGE,)),
    ('message-ignores-custom', SUBMIT_C, '    if ((flags & OS64_PAGE_VALIDITY_CUSTOM) && custom != NULL) {',
     '    if (false && custom != NULL) {', (PAGE, DOM)),
    ('any-ask-has-a-submitter', SUBMIT_C,
     '    return (what.how == OS64_PAGE_ACTIVATE_CONTROL || what.how == OS64_PAGE_ACTIVATE_IMPLICIT) && c != NULL &&',
     '    return c != NULL &&', (PAGE,)),
    ('email-takes-a-nul', SUBMIT_C, "        if (c == '\\0' || (!ascii_alnum(c)", '        if (false || (!ascii_alnum(c)',
     (PAGE, DOM)),
    ('custom-sentence-cut-in-a-character', SUBMIT_C,
     "        while (n < custom_len && n > 0 && ((unsigned char)custom[n] & 0xc0) == 0x80)\n            n--;\n", '',
     (PAGE,)),
    ('custom-message-through-a-buffer', VALIDITY_C, '        if (flags & OS64_PAGE_VALIDITY_CUSTOM) {\n            /*',
     '        if (false) {\n            /*', (DOM,)),
    ('validated-submission-judged-again', ACTIVATE_C, '    if (what.how == OS64_PAGE_ACTIVATE_FORM || validated)',
     '    if (what.how == OS64_PAGE_ACTIVATE_FORM || (validated && false))', (PAGE,)),
    ('model-validates-twice', YONDER_C,
     '    os64_page_verdict_t verdict = validated ? os64_page_activate_validated(',
     '    os64_page_verdict_t verdict = validated && false ? os64_page_activate_validated(', (SCRIPTS,)),
    ('submission-reentered', WINDOW_C, '        if (at->node == form) return 1;',
     '        if (at->node == form && false) return 1;', (DOM,)),
    ('onsubmit-page-unvalidated', SCRIPTS_C,
     '        if ((handler_types(s) & (os64_dom_handler_bit("invalid") | os64_dom_handler_bit("submit"))) == 0)',
     '        if ((handler_types(s) & os64_dom_handler_bit("invalid")) == 0)', (SCRIPTS,)),
    ('person-submit-event-reentered', EVENT_C, '    if (submitting) dom->submitting = &self;',
     '    if (submitting && false) dom->submitting = &self;', (SCRIPTS,)),
    ('single-select-placeholder-ignored', SUBMIT_C,
     '        return one_line && index == 0 && first != NULL',
     '        return false && one_line && index == 0 && first != NULL', (PAGE, DOM)),
    # P4b. min, max and step over each type read as a number
    ('date-in-days', VALUE_C,
     '        *out = (double)days_from_civil(year, month, day) * 86400000.0;\n        return true;\n    case OS64_PAGE_INPUT_MONTH:',
     '        *out = (double)days_from_civil(year, month, day);\n        return true;\n    case OS64_PAGE_INPUT_MONTH:', (PAGE,)),
    ('month-from-one', VALUE_C, '        *out = (double)(((int64_t)year - 1970) * 12 + month - 1);',
     '        *out = (double)(((int64_t)year - 1970) * 12 + month);', (PAGE,)),
    ('week-from-january-4', VALUE_C, '        int64_t monday = jan4 - (((jan4 % 7) + 7 + 3) % 7);',
     '        int64_t monday = jan4;', (PAGE,)),
    ('time-fraction-unscaled', VALUE_C, '    while (places++ < 3)\n        fraction *= 10;\n',
     '    while (places++ < 3)\n        fraction *= 1;\n', (PAGE,)),
    ('week-base-zero', VALUE_C, '    case OS64_PAGE_INPUT_WEEK: *scale = 604800000; *base = -259200000; break;',
     '    case OS64_PAGE_INPUT_WEEK: *scale = 604800000; *base = 0; break;', (PAGE,)),
    ('time-never-wraps', SUBMIT_C, '    if (input == OS64_PAGE_INPUT_TIME && has_min && has_max && max < min) {',
     '    if (false && input == OS64_PAGE_INPUT_TIME && has_min && has_max && max < min) {', (PAGE,)),
    ('step-any-ignored', SUBMIT_C, '    if (step_text != NULL && os64_streq_nocase(step_text, "any"))\n        return flags;\n',
     '', (PAGE,)),
    ('step-base-ignores-min', SUBMIT_C, '    if (min_spelled)\n        base = min;', '    if (false)\n        base = min;',
     (PAGE, DOM)),
    ('step-base-ignores-value-attribute', SUBMIT_C,
     '    else if (p_input_number(input, p_attr(n, "value"), &from))',
     '    else if (false && p_input_number(input, p_attr(n, "value"), &from))', (PAGE,)),
    ('number-read-strictly', VALUE_C, '        return p_number_parse(s, false, out);',
     '        return p_number_parse(s, true, out);', (PAGE,)),
    ('scale-in-binary', RANGE_C, '    multiply(&stride,scale);', '    stride=from_double(step*(double)scale);', (PAGE,)),
    ('year-uncapped', VALUE_C, '        if (y > 275760)', '        if (y > 999999999)', (PAGE,)),
    ('range-max-below-min-kept', SUBMIT_C, '        if (max < min) max = min;', '        if (false) max = min;', (PAGE,)),
    ('wrapped-time-said-once', SUBMIT_C,
     '        if (input == OS64_PAGE_INPUT_TIME && (flags & OS64_PAGE_VALIDITY_UNDERFLOW) &&',
     '        if (false && (flags & OS64_PAGE_VALIDITY_UNDERFLOW) &&', (PAGE,)),
    ('reversed-date-said-twice', SUBMIT_C,
     '        if (input == OS64_PAGE_INPUT_TIME && (flags & OS64_PAGE_VALIDITY_UNDERFLOW) &&',
     '        if ((flags & OS64_PAGE_VALIDITY_UNDERFLOW) &&', (PAGE,)),
    ('number-bound-as-written', SUBMIT_C, '            min = min_text;\n            max = max_text;',
     '            min = numeric ? p_attr(node, "min") : min_text;\n            max = max_text;', (PAGE,)),
    ('step-judged-inexactly', RANGE_C, '    return rem.used == 0;', '    return rem.used == 0 || step < 1e-9;', (PAGE,)),
    ('host-submission-reentered', VALIDITY_C,
     '    dom->submitting = &self;\n    /* Through the wrapper',
     '    (void)self;\n    /* Through the wrapper', (SCRIPTS,)),
    # P4a. the binding: the pattern, ValidityState, the members, invalid
    ('pattern-never-mismatches', VALIDITY_C, '        if (!JS_IsException(found)) result = JS_IsNull(found) ? 0 : 1;',
     '        if (!JS_IsException(found)) result = 1;', (DOM, SCRIPTS)),
    ('pattern-unanchored', VALIDITY_C, '        re = compile(m, source, plen + 6);', '        re = compile(m, pattern, plen);',
     (DOM,)),
    ('pattern-alone-unchecked', VALIDITY_C, '    JSValue alone = compile(m, pattern, plen);',
     '    JSValue alone = JS_UNDEFINED;', (DOM,)),
    ('pattern-flag-u', VALIDITY_C, '    JSValue args[2] = {JS_NewStringLen(ctx, source, len), JS_NewString(ctx, "v")};',
     '    JSValue args[2] = {JS_NewStringLen(ctx, source, len), JS_NewString(ctx, "u")};', (DOM,)),
    ('invalid-bubbles', VALIDITY_C, '    JSValue event = d_event_new(dom, ctx, "invalid", false, true, true);',
     '    JSValue event = d_event_new(dom, ctx, "invalid", true, true, true);', (DOM,)),
    ('invalid-not-cancelable', VALIDITY_C, '    JSValue event = d_event_new(dom, ctx, "invalid", false, true, true);',
     '    JSValue event = d_event_new(dom, ctx, "invalid", false, false, true);', (DOM,)),
    ('check-fires-nothing', VALIDITY_C, '        int heard = fire_invalid(dom, ctx, node);',
     '        int heard = 1;', (DOM,)),
    ('barred-still-counts', VALIDITY_C, '    *flags = 0;\n    if (!will) return 0;', '    *flags = 0;', (DOM,)),
    ('validity-made-each-time', VALIDITY_C,
     '    if (owner->validity != NULL) return JS_DupValue(ctx, owner->validity->value);',
     '    if (false) return JS_DupValue(ctx, owner->validity->value);', (DOM,)),
    ('valid-ignores-custom', VALIDITY_C, '    return JS_NewBool(ctx, magic == V_VALID ? flags == 0 :',
     '    return JS_NewBool(ctx, magic == V_VALID ? (flags & ~OS64_PAGE_VALIDITY_CUSTOM) == 0 :', (DOM,)),
    ('message-for-a-barred-control', VALIDITY_C, '    case M_MESSAGE: {\n        int bad = invalid(dom, ctx, node, &flags);',
     '    case M_MESSAGE: {\n        int bad = flags_of(dom, ctx, node, &flags) < 0 ? -1 : flags != 0;', (DOM,)),
    ('custom-dropped', VALIDITY_C,
     '        status = os64_page_node_set_custom_validity(dom->state, node, text.data, text.length);',
     '        status = 0;', (DOM,)),
    ('form-walks-every-control', VALIDITY_C, '        if (submittable(at) && d_form_owner(at) == form) {',
     '        if (submittable(at)) {', (DOM,)),
    ('form-reports-nothing', VALIDITY_C, '    if (result == 0 && report_first && unhandled != NULL &&',
     '    if (false && report_first && unhandled != NULL &&', (SCRIPTS,)),
    ('host-ignores-formnovalidate', VALIDITY_C, '    if (!dom->options.modern || skip) return OS64_JS_OK;',
     '    if (!dom->options.modern || (skip && false)) return OS64_JS_OK;', (SCRIPTS,)),
    ('submit-unvalidated', WINDOW_C, '    int valid = novalidate ? 1 : d_form_validate(dom, ctx, form, true);',
     '    int valid = novalidate || true ? 1 : d_form_validate(dom, ctx, form, true);', (DOM,)),
    ('submit-method-validates', WINDOW_C, '    if (!event) {\n        d_navigate(',
     '    if (!event && d_form_validate(dom, ctx, form, true) > 0) {\n        d_navigate(', (DOM,)),
    ('formnovalidate-ignored', WINDOW_C,
     '    bool novalidate = (submitter != NULL && os64_html_attr(submitter, "formnovalidate") != NULL) ||',
     '    bool novalidate = false ||', (DOM,)),
    ('oninvalid-misnamed', EVENT_C, '"message", "messageerror", "invalid"', '"message", "messageerror", "invalidx"',
     (DOM,)),
    # P4a. yonder: a person's submission validated, the report, the refusal's sentence
    ('person-submit-unvalidated', YONDER_C,
     '                if (!page_validates(control, OS64_PAGE_ACTIVATE_CONTROL, &validated))\n                    break;',
     '                if (false && !page_validates(control, OS64_PAGE_ACTIVATE_CONTROL, &validated))\n'
     '                    break;', (SCRIPTS,)),
    ('enter-unvalidated', YONDER_C,
     '            if (!page_validates(control, OS64_PAGE_ACTIVATE_IMPLICIT, &validated))\n                break;',
     '            if (false && !page_validates(control, OS64_PAGE_ACTIVATE_IMPLICIT, &validated))\n                break;',
     (SCRIPTS,)),
    ('report-unfocused', YONDER_C,
     '                if (fw != NULL && !fw->w->hidden)\n                    os64_ui_set_focus(&g.ui, fw->w);\n'
     '                if (yonder_scripts_report_message',
     '                if (yonder_scripts_report_message', (SCRIPTS,)),
    ('report-unsaid', YONDER_C, "sizeof(said)) && said[0] != '\\0')\n                    status_rest(said);",
     "sizeof(said)) && said[0] != '\\0')\n                    (void)said;", (SCRIPTS,)),
    ('refusal-says-its-reason', YONDER_C,
     "            // The control's own sentence, as the page's report says it.\n            status_rest(sentence);",
     "            // The control's own sentence, as the page's report says it.\n"
     "            status_rest(os64_page_reason_name(request.reason));", (SCRIPTS,)),
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
                # A harness is a script and its flags: a .py is Python's.
                script, *flags = harness.split()
                runner = 'python3' if script.endswith('.py') else 'bash'
                try:
                    result = subprocess.run([runner, str(root / script), *flags], cwd=root, stdout=out, stderr=out,
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
