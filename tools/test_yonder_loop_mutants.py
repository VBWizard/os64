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
     '        if (!has_handlers(s))\n            return OS64_JS_OK;',
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
    ('stream-ready-before-the-blocking-script', YONDER,
     '    if (host != NULL && scripts_owed(host))\n        return stream_task();\n    if (g.stream.stopped) {',
     '    if (g.stream.stopped) {', WINDOW),
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
     '            os64_dom_event_t ev = mouse_event("click", true, &in);\n            if (page_event(&ev, in.node))\n                break;\n            int32_t link',
     '            os64_dom_event_t ev = mouse_event("click", true, &in);\n            (void)page_event(&ev, in.node);\n            int32_t link', WINDOW),
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
