#!/usr/bin/env python3
"""One deliberate break at a time to the media keys' rule
(kernel/src/audio/media_keys.c: the four rules, the tie, the full-ring pass,
the toggle, the ring, the stamps, the state word), against
tools/test_media_keys_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither.

The shape is test_audio_stream_mutants.py's: each mutant is applied to the
tree's copy of the file, the suite is run, and the file is put back from the
bytes read before the change, checked byte for byte, whatever happens. Run it
from a clean tree, and never beside another harness that edits the same tree;
the per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/media-keys-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

STREAM = 'kernel/src/audio/media_keys.c'
CASES = 'bash tools/test_media_keys_host.sh 60'

mutants = [
    ('a-long-lineage-overruns', '\tif (n > MEDIA_KEYS_LINEAGE)\n\t\tn = MEDIA_KEYS_LINEAGE;\n', ''),
    ('the-wall-is-not-a-wall', '\t\t\treturn (int)i;\n\t\t}\n\treturn -1;',
     '\t\t\treturn (int)i;\n\t\t}\n\treturn 0;'),
    ('opening-is-not-activity', '\t\t\ts->active_seq = ++t->seq;   // opening is activity',
     '\t\t\t(void)0;   // opening is activity'),
    ('a-removal-keeps-the-slot', '\t\t\tt->slot[i] = NULL;\n\t\t\tt->listed--;',
     '\t\t\tt->listed--;'),
    ('leading-text-is-taken', '\tfor (size_t i = 0; i < n; i++)\n\t\tif (text[i] != word[i])\n\t\t\treturn false;',
     '\tfor (size_t i = 1; i < n; i++)\n\t\tif (text[i] != word[i])\n\t\t\treturn false;'),
    ('trailing-text-is-taken', '\t\tif (!space(text[i]))\n\t\t\treturn false;', '\t\tif (!space(text[i]) && 0)\n\t\t\treturn false;'),
    ('a-prefix-is-a-word', '\tif (length < n)\n\t\treturn false;', '\tif (length < n)\n\t\tn = length;'),
    ('newline-is-not-space', "c == ' ' || c == '\\t' || c == '\\n' || c == '\\r'", "c == ' ' || c == '\\t' || c == '\\r'"),
    ('paused-reads-as-playing', '\telse if (word_is(text, length, "paused"))\n\t\t*state = MEDIA_KEYS_PAUSED;',
     '\telse if (word_is(text, length, "paused"))\n\t\t*state = MEDIA_KEYS_PLAYING;'),
    ('saying-playing-again-restarts', '\tif (state == MEDIA_KEYS_PLAYING && s->state != MEDIA_KEYS_PLAYING)\n\t\ts->playing_seq = stamp;',
     '\tif (state == MEDIA_KEYS_PLAYING)\n\t\ts->playing_seq = stamp;'),
    ('a-state-write-is-not-activity', '\ts->state = state;\n\ts->active_seq = stamp;', '\ts->state = state;'),
    ('no-focus-matches-zero', '\tif (focus == 0)\n\t\treturn MEDIA_KEYS_LINEAGE;\n\tfor', '\tfor'),
    ('the-oldest-player-wins', '\t\tif (best < 0 || s->playing_seq > t->slot[best]->playing_seq)',
     '\t\tif (best < 0 || s->playing_seq < t->slot[best]->playing_seq)'),
    ('a-full-player-is-not-passed', '\t\tif (s == NULL || s->count == MEDIA_KEYS_RING || s->state != MEDIA_KEYS_PLAYING)',
     '\t\tif (s == NULL || s->state != MEDIA_KEYS_PLAYING)'),
    ('a-full-focus-is-not-passed', '\t\tif (s == NULL || s->count == MEDIA_KEYS_RING)\n\t\t\tcontinue;\n\t\tuint32_t near',
     '\t\tif (s == NULL)\n\t\t\tcontinue;\n\t\tuint32_t near'),
    ('the-farthest-ancestor-wins', '\t\tif (best < 0 || near < nearest ||', '\t\tif (best < 0 || near > nearest ||'),
    ('a-tie-goes-to-the-first-slot', ' || (near == nearest && s->active_seq > t->slot[best]->active_seq)) {', ') {'),
    ('a-tie-goes-to-the-least-active', '(near == nearest && s->active_seq > t->slot[best]->active_seq)',
     '(near == nearest && s->active_seq < t->slot[best]->active_seq)'),
    ('focus-is-skipped', '\tif (best >= 0) {\n\t\t*rule = MEDIA_KEYS_RULE_FOCUS;\n\t\treturn best;\n\t}',
     '\tif (best >= 0 && 0) {\n\t\t*rule = MEDIA_KEYS_RULE_FOCUS;\n\t\treturn best;\n\t}'),
    ('the-least-active-wins', '\t\tif (best < 0 || s->active_seq > t->slot[best]->active_seq)\n\t\t\tbest = (int)i;\n\t}\n\t*rule',
     '\t\tif (best < 0 || s->active_seq < t->slot[best]->active_seq)\n\t\t\tbest = (int)i;\n\t}\n\t*rule'),
    ('a-full-session-is-active-last', '\t\tif (s == NULL || s->count == MEDIA_KEYS_RING)\n\t\t\tcontinue;\n\t\tif (best < 0 || s->active_seq',
     '\t\tif (s == NULL)\n\t\t\tcontinue;\n\t\tif (best < 0 || s->active_seq'),
    ('the-pass-is-not-shown', '\t\t\tt->slot[i]->passed_full = t->slot[i]->count == MEDIA_KEYS_RING;',
     '\t\t\tt->slot[i]->passed_full = false;'),
    ('stop-is-a-toggle', '\tbool toggle = key == OS64_MEDIA_KEY_PLAY || key == OS64_MEDIA_KEY_PAUSE;\n\tif (at < 0)',
     '\tbool toggle = key == OS64_MEDIA_KEY_PLAY || key == OS64_MEDIA_KEY_PAUSE || key == OS64_MEDIA_KEY_STOP;\n\tif (at < 0)'),
    ('the-kernel-takes-next', '\t\tif (toggle)\n\t\t\troute.kernel_toggle = true;\n\t\telse\n\t\t\tt->unanswered++;',
     '\t\troute.kernel_toggle = true;'),
    ('unanswered-is-not-counted', '\t\telse\n\t\t\tt->unanswered++;', '\t\telse\n\t\t\t(void)0;'),
    ('the-toggle-is-the-key', '\tif (toggle)\n\t\tgiven = s->state == MEDIA_KEYS_PLAYING ? OS64_MEDIA_KEY_PAUSE : OS64_MEDIA_KEY_PLAY;',
     '\tif (0)\n\t\tgiven = s->state == MEDIA_KEYS_PLAYING ? OS64_MEDIA_KEY_PAUSE : OS64_MEDIA_KEY_PLAY;'),
    ('paused-toggles-to-pause', 'given = s->state == MEDIA_KEYS_PLAYING ? OS64_MEDIA_KEY_PAUSE : OS64_MEDIA_KEY_PLAY;',
     'given = s->state != MEDIA_KEYS_STOPPED ? OS64_MEDIA_KEY_PAUSE : OS64_MEDIA_KEY_PLAY;'),
    ('the-ring-writes-over-its-head', '\tos64_media_key_t *slot = &s->ring[(s->head + s->count) % MEDIA_KEYS_RING];',
     '\tos64_media_key_t *slot = &s->ring[s->head];'),
    ('the-serial-does-not-count', '\tslot->serial = ++s->serial;', '\tslot->serial = s->serial;'),
    ('the-time-is-not-stamped', '\tslot->at_us = now_us;', '\tslot->at_us = now_us / 2;'),
    ('a-delivery-is-not-activity', '\ts->count++;\n\ts->active_seq = ++t->seq;', '\ts->count++;'),
    ('the-kernel-failure-is-not-counted', 'void media_keys_kernel_failed(media_keys_table_t *t)\n{\n\tt->unanswered++;',
     'void media_keys_kernel_failed(media_keys_table_t *t)\n{\n\t(void)t;'),
    ('a-take-does-not-advance', '\ts->head = (s->head + 1) % MEDIA_KEYS_RING;\n\ts->count--;\n\ts->delivered++;',
     '\ts->count--;\n\ts->delivered++;'),
    ('an-empty-ring-gives-a-press', '\tif (s->count == 0)\n\t\treturn false;\n\t*out', '\t*out'),
    ('undelivered-is-not-lost', '\ts->delivered--;\n\ts->lost++;', '\ts->delivered--;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / STREAM
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {STREAM}', file=sys.stderr)
        unbuilt.append(name)
        continue
    log = args.logs / f'{name}.log'
    try:
        path.write_text(text.replace(old, new))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=120)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        if path.read_bytes() != original:
            print(f'{name}: {STREAM} WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    # A compiler's error, not a sanitizer's "runtime error:", which is a catch.
    built = re.search(r'\.(c|h):\d+:\d+: error:', body) is None and 'undefined reference' not in body
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

print(f'\nmedia_keys mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
