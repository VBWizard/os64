#!/usr/bin/env python3
"""One deliberate break at a time to the loop (docs/design/pending/DOM_D7.md
§ The loop, in yonder), against the finished harnesses: a mutant is CAUGHT
when the harness that holds its rule fails, and MISSED when it passes, which
means a test is missing and gets written. A build failure is neither.

The shape is test_yonder_stream_mutants.py's: each mutant is applied to the
worktree's copy of its file, the harness is run, and the file is put back
from the bytes read before the change — checked byte for byte — whatever
happens. Run it from a clean tree; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-loop-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

SCRIPTS = 'userland/apps/yonder/scripts.c'
YONDER = 'userland/apps/yonder/yonder.c'
UI = 'userland/libos64/ui_text.c'
ACTIVATE = 'userland/libpage/activate.c'
WINDOW = 'tools/test_yonder_scripts_host.sh'
LIBUI = 'tools/test_ui_text_host.py --real'
PAGE = 'tools/test_libpage_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states;
# none supplies a replacement expectation.
mutants = [
    # The page's script host
    ('host-blocking-src-blocks', SCRIPTS,
     '    s->blocking = (int32_t)(it - s->items);\n    return YONDER_STOP_BLOCK;\n}',
     '    s->blocking = (int32_t)(it - s->items);\n    return YONDER_STOP_RESUME;\n}', WINDOW),
    ('host-defer-after-the-parse', SCRIPTS,
     '    Item *d = s->parse_ended ? next_defer((yonder_scripts_t *)s) : NULL;',
     '    Item *d = next_defer((yonder_scripts_t *)s);', WINDOW),
    ('host-sticky-retires', SCRIPTS,
     '    if (sticky(out->status))\n        retire(s);',
     '    if (sticky(out->status) && false)\n        retire(s);', WINDOW),
    ('host-dead-runs-nothing', SCRIPTS,
     'bool yonder_scripts_pending(const yonder_scripts_t *s)\n{\n    if (s == NULL || s->dead)',
     'bool yonder_scripts_pending(const yonder_scripts_t *s)\n{\n    if (s == NULL)', WINDOW),
    ('host-failure-is-said', SCRIPTS,
     '        out->status = OS64_JS_EXCEPTION;\n        os64_strcopy(out->message, sizeof(out->message), it->why);',
     '        os64_strcopy(out->message, sizeof(out->message), it->why);', WINDOW),
    ('host-handlers-make-a-runtime', SCRIPTS,
     '        if ((handler_types(s) & os64_dom_handler_bit(event->type)) == 0)\n            return OS64_JS_OK;',
     '        return OS64_JS_OK;', WINDOW),
    ('host-note-kept', SCRIPTS,
     '    if (s != NULL)\n        os64_strcopy(s->note, sizeof(s->note), line);',
     '    (void)s;\n    (void)line;', WINDOW),
    ('host-limit-reaches-the-runtime', SCRIPTS,
     '    s->options.execution_ms = ms;\n    os64_js_outcome_t out;',
     '    os64_js_outcome_t out;', WINDOW),
    ('host-source-at-prepare', SCRIPTS,
     '    if (!inline_source(s, it)) {\n        fail_item(s, it, "the inline script is longer than a script may be");\n        return;\n    }\n    make_ready(s, it);',
     '    make_ready(s, it);', WINDOW),
    # The stream's turn
    ('stream-stops-at-a-script', YONDER,
     '        if (stream_stop(os64_html_parser_script(g.stream.parser)) != YONDER_STOP_RESUME) {',
     '        if (stream_stop(os64_html_parser_script(g.stream.parser)) == 99) {', WINDOW),
    ('stream-ready-while-the-src-is-out', YONDER,
     '            return scripts_owed(host) ? stream_task() : false;',
     '            return false;', WINDOW),
    ('stream-deferred-before-finish', YONDER,
     '    if (yonder_scripts_deferring(g.stream.scripts)) {',
     '    if (false) {', WINDOW),
    ('stream-domcontentloaded', YONDER,
     '        yonder_scripts_dispatch(fresh.scripts, doc->document, &loaded, NULL, &out);',
     '        (void)loaded;\n        os64_memset(&out, 0, sizeof(out));', WINDOW),
    ('stream-model-adopts-the-state', YONDER,
     '    os64_page_t *model = os64_page_build(doc, g.stream.head.url, NULL, fresh.state);',
     '    os64_page_t *model = os64_page_build(doc, g.stream.head.url, NULL, NULL);', WINDOW),
    ('stream-abandon', YONDER,
     '    os64_html_document_t *doc = g.stream.parser != NULL ? os64_html_parser_abandon(g.stream.parser) : NULL;',
     '    os64_html_document_t *doc = NULL;\n    if (g.stream.parser != NULL)\n        os64_html_parser_destroy(g.stream.parser);', WINDOW),
    ('stream-file-takes-the-stream', YONDER,
     '    g.stream.local = bytes;\n    g.stream.local_len = len;',
     '    os64_free(bytes);\n    g.stream.local = NULL;\n    g.stream.local_len = 0;', WINDOW),
    # The shown page
    ('page-load', YONDER,
     '        yonder_scripts_dispatch(g.page.scripts, NULL, &loaded, NULL, &out);',
     '        (void)loaded;\n        os64_memset(&out, 0, sizeof(out));', WINDOW),
    ('page-timer-fires', YONDER,
     '        ran = yonder_scripts_timer_fire(g.page.scripts, yonder_now_ms(), &out);',
     '        ran = false;', WINDOW),
    ('page-navigation-after-the-task', YONDER,
     '    if (!yonder_scripts_take_navigation(host, &ask))\n        return;',
     '    if (!yonder_scripts_take_navigation(host, &ask) || true)\n        return;', WINDOW),
    ('page-navigation-judged-go', YONDER,
     '        request_navigate(&request, ask->kind == OS64_DOM_NAVIGATE_REPLACE ? NAV_REFRESH : NAV_GO,\n                         WAY_ASK_GO);',
     '        request_navigate(&request, ask->kind == OS64_DOM_NAVIGATE_REPLACE ? NAV_REFRESH : NAV_GO,\n                         WAY_ASK_NEVER);', WINDOW),
    ('page-old-page-stays-live', YONDER,
     'static void stop_trip(void)\n{\n    coming_drop();',
     'static void stop_trip(void)\n{\n    yonder_scripts_free(g.page.scripts);\n    g.page.scripts = NULL;\n    coming_drop();', WINDOW),
    # Input events
    ('input-click-cancels-the-link', YONDER,
     '            if (page_event(&ev, in.node))\n                break;\n            // The listeners may have rebuilt the page: the image is found',
     '            (void)page_event(&ev, in.node);\n            // The listeners may have rebuilt the page: the image is found', WINDOW),
    ('input-checkbox-put-back', YONDER,
     '                (void)os64_page_set_checked(page_model(&g.page), control, in.was);',
     '                (void)in.was;', WINDOW),
    ('input-keydown-cancels-the-key', YONDER,
     '    key.type = "keydown";\n    if (page_event(&key, target))\n        return true;',
     '    key.type = "keydown";\n    (void)page_event(&key, target);', WINDOW),
    ('input-submit-cancels', YONDER,
     '            if (!plain_event("submit", true, true, control_form(control)))\n                form_send(',
     '            (void)plain_event("submit", true, true, control_form(control));\n            form_send(', WINDOW),
    ('input-focus', YONDER,
     '    input_queue(IN_FOCUS, fw->node, NULL, 0, 0, false);',
     '    (void)fw;', WINDOW),
    ('input-change-at-blur', YONDER,
     '            input_queue(IN_CHANGE, g.focus_node, NULL, 0, 0, false);',
     '            (void)0;', WINDOW),
    ('input-mouseout', YONDER,
     '                input_queue(IN_OUT, g.hover_node, over, x, y, false);',
     '', WINDOW),
    ('input-text-edit-is-input', YONDER,
     '            fw->u.field.on_change = field_changed;',
     '', WINDOW),
    # libui's change callback
    ('libui-change-only-on-edits', UI,
     '\t\tif (changed && tf->on_change)\n\t\t\ttf->on_change(tf, tf->edit_user);',
     '\t\tif (tf->on_change)\n\t\t\ttf->on_change(tf, tf->edit_user);', LIBUI),
    # libpage's form.submit()
    ('libpage-script-submit-not-validated', ACTIVATE,
     '    if (what.how == OS64_PAGE_ACTIVATE_FORM)\n        novalidate = true;',
     '', PAGE),
    # Fable rd on D7b: the parse first, a fragment kept, a runtime at its event
    ('stream-task-before-the-parse', YONDER,
     '    if (yonder_scripts_connected_pending(host))\n        return stream_connected();\n    if (g.stream.stopped) {',
     '    if (yonder_scripts_connected_pending(host))\n        return stream_connected();\n'
     '    if (host != NULL && scripts_owed(host))\n        return stream_task();\n    if (g.stream.stopped) {', WINDOW),
    ('stream-due-timers-before-domcontentloaded', YONDER,
     '                         yonder_scripts_timer_next(host) <= g.stream.ended_ms))',
     '                         false))', WINDOW),
    ('stream-zero-ms-chain-bounded', YONDER,
     '                         yonder_scripts_timer_next(host) <= g.stream.ended_ms))',
     '                         yonder_scripts_timer_next(host) <= yonder_now_ms()))', WINDOW),
    ('stream-connected-before-the-parse', YONDER,
     '    if (yonder_scripts_connected_pending(host))\n        return stream_connected();\n    if (g.stream.stopped) {',
     '    if (false && yonder_scripts_connected_pending(host))\n        return stream_connected();\n    if (g.stream.stopped) {', WINDOW),
    ('unshown-fragment-kept', YONDER,
     '        else if (!unshown_place(host, &here, &fragment, &cap, &has_fragment))\n            here = NULL;',
     '        else if (true || !unshown_place(host, &here, &fragment, &cap, &has_fragment))\n            here = NULL;', WINDOW),
    ('handler-runtime-at-its-event', SCRIPTS,
     '        if ((handler_types(s) & os64_dom_handler_bit(event->type)) == 0)\n            return OS64_JS_OK;',
     '        if (handler_types(s) == 0)\n            return OS64_JS_OK;', WINDOW),
    # document.write (DOM_D9.md): only the script the parse is stopped at
    # writes, and what it writes reaches the stream's parser and the audit.
    ('write-while-blocking', SCRIPTS,
     '    s->writing = true;\n    bool ran', '    bool ran', WINDOW),
    ('write-only-while-blocking', SCRIPTS,
     '    if (!s->writing || s->options.write == NULL)', '    if (s->options.write == NULL)', WINDOW),
    ('write-counted-for-the-audit', SCRIPTS,
     '    s->written += length;\n', '', WINDOW),
    # The join with D8 and D10 (DOM_D7.md § The cut, D7c). Teardown: the
    # page's runtime reclaims and reports a leak, and the window counts it.
    ('join-teardown-reclaims', SCRIPTS,
     'os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM,',
     'os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_FATAL,', WINDOW),
    ('join-teardown-leak-counted', SCRIPTS,
     '        if (teardown_leaks != SIZE_MAX) teardown_leaks++;\n', '', WINDOW),
    # Geometry: every host gets its page's provider, and each page not on
    # screen is measured beside itself, never published.
    ('join-provider-installed', SCRIPTS,
     'os64_dom_set_geometry(s->dom, s->options.geometry, s->options.opaque);',
     'os64_dom_set_geometry(s->dom, NULL, s->options.opaque);', WINDOW),
    ('join-stream-measured', YONDER,
     '    if (g.stream.parser == NULL || os64_html_parser_document(g.stream.parser) != doc)\n        return false;',
     '    if (true)\n        return false;', WINDOW),
    ('join-finishing-measured', YONDER,
     '    if (g.finishing.page != NULL && page_doc(g.finishing.page) == doc)',
     '    if (false && g.finishing.page != NULL && page_doc(g.finishing.page) == doc)', WINDOW),
    ('join-coming-measured', YONDER,
     '    if (g.coming.active && page_doc(&g.coming.page) == doc)\n        return measured_layout(',
     '    if (false && g.coming.active && page_doc(&g.coming.page) == doc)\n        return measured_layout(', WINDOW),
    ('join-coming-not-published', YONDER,
     '        return measured_layout(&g.coming.page, g.coming.page.state,',
     '        return lay_out_page(&g.coming.page, width, height) && measured_layout(&g.coming.page, g.coming.page.state,', WINDOW),
    ('join-style-elements-measured', YONDER,
     '        measured_style_elements(&s_measuring, model);\n',
     '        (void)measured_style_elements;\n', WINDOW),
    ('join-one-measured-layout', YONDER,
     '    measured_forget(s_measured.doc);\n    if (model == NULL)',
     '    measured_forget(doc);\n    if (model == NULL)', WINDOW),
    ('join-measured-follows-the-tree', YONDER,
     '    if (mine && s_measured.version == version && s_measured.width == width &&',
     '    if (mine && s_measured.width == width &&', WINDOW),
    ('join-measured-let-go-at-arrival', YONDER,
     '    measured_forget(page_doc(&g.page));\n    g.page_serial++;',
     '    g.page_serial++;', WINDOW),
    # The count and the sentence, for every task kind.
    ('join-count-from-zero-each-task', YONDER,
     '    (void)yonder_scripts_geometry_stats(host, true);\n    return s_script_audit',
     '    (void)host;\n    return s_script_audit', WINDOW),
    ('join-count-in-the-audit', YONDER,
     '                      (unsigned long)yonder_scripts_written(host), (unsigned long)forced.layouts,',
     '                      (unsigned long)yonder_scripts_written(host), (unsigned long)0,', WINDOW),
    ('join-count-said-when-ok', YONDER,
     '        if (at != 0)\n            status_rest(line);\n        return;',
     '        return;', WINDOW),
    ('join-count-said-with-the-failure', YONDER,
     '        os64_strcopy(line + at, sizeof(line) - at, "; ");\n        at += 2;',
     '        at = 0;', WINDOW),
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
                result = subprocess.run(['bash', '-c', harness if harness.endswith('.sh') else f'python3 {harness}'],
                                        cwd=root, stdout=out, stderr=out, timeout=900)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
                subprocess.run(['pkill', '-f', 'yonder-script[s]'])
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

print(f'\nloop mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
