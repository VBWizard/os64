#!/usr/bin/env python3
"""One deliberate break at a time to the stream (docs/design/pending/DOM_D4.md),
against the finished harnesses: a mutant is CAUGHT when the harness that
holds its rule fails, and MISSED when it passes, which means a test is
missing and gets written. A build failure is neither.

Each mutant is applied to the worktree's copy of its file, the harness is
run, and the file is put back from the bytes read before the change —
checked byte for byte — whatever happens. Nothing else is touched. Run it
from a clean tree and read the summary; the per-mutant logs are in --logs.
"""
import argparse
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/yonder-stream-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

MAIL = 'userland/apps/yonder/mail.c'
YONDER = 'userland/apps/yonder/yonder.c'
LOAD = 'userland/libway/load.c'
SESSION = 'userland/libway/session.c'
RING = 'tools/test_yonder_stream_host.sh'
WINDOW = 'tools/test_yonder_scripts_host.sh'
FETCH = 'tools/test_way_fetch_host.sh'
PURE = 'tools/test_way_host.sh'

# (name, file, old, new, harness). Each breaks one rule the design states;
# none supplies a replacement expectation.
mutants = [
    # The ring
    ('ring-wrap', MAIL,
     'uint32_t i = (m->first + m->count) % YONDER_STREAM_CHUNKS;',
     'uint32_t i = m->first + m->count;', RING),
    ('ring-count-down', MAIL,
     '    m->first = (m->first + 1) % YONDER_STREAM_CHUNKS;\n    m->count--;',
     '    m->first = (m->first + 1) % YONDER_STREAM_CHUNKS;', RING),
    ('ring-split-large-post', MAIL,
     'size_t take = len < YONDER_STREAM_CHUNK ? len : YONDER_STREAM_CHUNK;',
     'size_t take = len;', RING),
    ('ring-waiting-flag', MAIL,
     '        m->waiting = true;\n        os64_lock_release(&m->lock);\n        Note note;',
     '        os64_lock_release(&m->lock);\n        Note note;', RING),
    ('ring-note-per-take', MAIL,
     '    bool wake = m->waiting;', '    bool wake = true;', RING),
    ('ring-small-cap', MAIL,
     '    if (cap < YONDER_STREAM_CHUNK)\n        return 0;', '    (void)cap;', RING),
    ('ring-streaming-verdict', MAIL,
     'bool some = m->head_new || m->count > 0 || m->verdict_new;',
     'bool some = m->head_new || m->count > 0;', RING),
    ('ring-cancel-ends-wait', MAIL,
     '        if (cancelled(ctx))\n            return false;\n        os64_lock_acquire(&m->lock);\n        if (m->count < YONDER_STREAM_CHUNKS) {',
     '        (void)cancelled;\n        (void)ctx;\n        os64_lock_acquire(&m->lock);\n        if (m->count < YONDER_STREAM_CHUNKS) {', RING),
    # The window
    ('window-slice-budget', YONDER,
     'while (g.stream.has_head && fed < STREAM_SLICE_BYTES) {',
     'while (g.stream.has_head) {', WINDOW),
    ('window-verdict-before-drain', YONDER,
     '    if (yonder_mail_streaming(mail))\n        return true;                    // chunks remain: next turn\n',
     '', WINDOW),
    ('window-mode-captured', YONDER,
     '    g.stream.scripting = g.scripts_on;', '    g.stream.scripting = false;', WINDOW),
    ('window-resume-past-script', YONDER,
     '        while (r == OS64_HTML_SCRIPT)\n            r = os64_html_parser_resume(g.stream.parser);',
     '', WINDOW),
    ('window-cancel-on-refusal', YONDER,
     '            if (g.nav.id != 0 && g.pool != NULL)\n                os64_work_cancel(g.pool, g.nav.id);\n            g.nav.id = 0;\n            stream_finish(OS64_FETCH_OK, NULL);',
     '            stream_finish(OS64_FETCH_OK, NULL);', WINDOW),
    ('window-request-copy', YONDER,
     '        g.stream.has_sent = true;\n        trip->request = *request;',
     '        trip->request = *request;', WINDOW),
    ('window-form-kept', YONDER,
     '    if (fresh.way.posted && g.stream.has_sent) {\n        fresh.sent = g.stream.sent;',
     '    if (false) {\n        fresh.sent = g.stream.sent;', WINDOW),
    ('window-stop-lit-by-stream', YONDER,
     'bool loading = g.stream.active || g.coming.active;',
     'bool loading = g.nav.id != 0 || g.coming.active;', WINDOW),
    ('window-parser-freed', YONDER,
     '    if (g.stream.parser != NULL)\n        os64_html_parser_destroy(g.stream.parser);',
     '', WINDOW),
    ('window-text-encoding', YONDER,
     'opt.charset = g.stream.text_utf8 ? "utf-8" : "windows-1252";',
     'opt.charset = "utf-8";', WINDOW),
    ('window-generation', YONDER,
     '    if ((mask & BELL_MAIL) && g.stream.mail != NULL &&\n        yonder_mail_generation(g.stream.mail) == g.generation) {',
     '    if ((mask & BELL_MAIL) && g.stream.mail != NULL) {', WINDOW),
    ('window-plaintext-prefix', YONDER,
     '        os64_html_parser_feed(g.stream.parser, kOpen, sizeof(kOpen) - 1);', '        (void)kOpen;', WINDOW),
    # libway's pieces
    ('load-progress-sentence', LOAD,
     'if (progress->produced >= o->shown + 64u * 1024u) {',
     'if (false) {', FETCH),
    ('load-head-copy-url', LOAD,
     'os64_strcopy(out->url, sizeof(out->url), head->url_text);\n    out->posted = head->method == OS64_FETCH_METHOD_POST;',
     'os64_strcopy(out->url, sizeof(out->url), "");\n    out->posted = head->method == OS64_FETCH_METHOD_POST;', FETCH),
    ('load-text-judged', LOAD,
     'bool plain = !html && type_is_text(type);', 'bool plain = false && type_is_text(type);', FETCH),
    ('note-refusal-sentence', SESSION,
     '    if (out->doc && out->doc->refusal && said < sizeof(trouble))',
     '    if (false)', PURE),
    ('note-wire-sentence', SESSION,
     '    if (fetch != OS64_FETCH_OK && said < sizeof(trouble))', '    if (false && fetch != OS64_FETCH_OK)', PURE),
    ('text-byte-order-mark', SESSION,
     'return text && len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF;',
     'return false && text != NULL && len != 0;', PURE),
    ('text-label-first', SESSION,
     "    if (head->charset[0] != '\\0')\n        return charset_is_utf8(head->charset);", "    if (false && head->charset[0] != '\\0')\n        return charset_is_utf8(head->charset);", PURE),
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
                # A broken ring can hang its two-thread harness (a poster that
                # never finds room, a taker that never sees a chunk); the
                # hang is the catch, so the ring's harness gets a short rope.
                limit = 180 if harness == RING else 900
                result = subprocess.run([str(root / harness)], cwd=root, stdout=out, stderr=out, timeout=limit)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
                subprocess.run(['pkill', '-f', 'stream_drive[r]'])
                subprocess.run(['pkill', '-f', 'yonder-script[s]'])
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {rel} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    built = 'error:' not in body and 'undefined reference' not in body
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

print(f'\nstream mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
