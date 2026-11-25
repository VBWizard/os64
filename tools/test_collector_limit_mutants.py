#!/usr/bin/env python3
"""One deliberate break at a time to the collector under the memory limit:
the engine's schedule and trigger (userland/libjs/patches/0048-collect-under-limit.patch),
the port allocator's count of refusals and the runtime's reader of both,
each against the runtime host suite's collector and limit cases
(tools/test_js_ledger.inc). A mutant is CAUGHT when the suite fails, MISSED
when it passes, which means a test is missing and gets written. A build
failure (a patch that no longer applies included) is neither.

A patch is pinned by its hash in libjs/manifest.json, so a mutant of 0048
changes the patch and writes the manifest's hash for it; both files are put
back from the bytes read before the change, checked byte for byte, whatever
happens. A mutant of a patch replaces lines with as many lines, so its hunks
still apply. Run it from a clean tree, and never beside another harness that
edits the same tree; the per-mutant logs are in --logs.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/collector-limit-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

PATCH = 'userland/libjs/patches/0048-collect-under-limit.patch'
MANIFEST = 'userland/libjs/manifest.json'
ALLOCATOR = 'userland/libjs/port/allocator.c'
RUNTIME = 'userland/libjs/runtime/runtime.c'
CASES = 'bash tools/test_js_runtime_host.sh'
TIMEOUT = 2400

mutants = [
    # The schedule (gc_schedule): each rule of the threshold.
    ('the-threshold-not-held-under-the-margin', PATCH,
     '+        if (next > limit - margin) {', '+        if (0) {'),
    ('no-step-for-a-heap-inside-the-margin', PATCH,
     '+            if (next < size + step)\n+                next = size + step;',
     '+            if (0)\n+                next = size + step;'),
    ('the-threshold-let-reach-the-limit', PATCH,
     '+        if (next > limit - last) {', '+        if (0) {'),
    ('the-schedule-only-at-the-start', PATCH,
     '+    gc_schedule(rt);\n', '+    (void)gc_schedule;\n'),
    # The trigger: the wait for growth after a run the limit scheduled.
    ('no-wait-for-growth-at-the-limit', PATCH,
     '+    if (force_gc && rt->threshold_by_limit &&', '+    if (0 && force_gc && rt->threshold_by_limit &&'),
    ('growth-from-the-last-run-not-its-least', PATCH,
     '+        rt->malloc_low_since_gc = rt->malloc_ctx.malloc_state.malloc_size;\n+    if (force_gc',
     '+        (void)rt->malloc_low_since_gc;\n+    if (force_gc'),
    ('a-host-s-off-overwritten', PATCH,
     '+    if (rt->malloc_gc_threshold == (size_t)-1)\n+        return;',
     '+    if (0)\n+        return;'),
    ('limit-runs-not-counted', PATCH,
     '+            rt->collector_counts.runs_by_limit++;', '+            (void)rt->collector_counts.runs_by_limit;'),
    # The allocator's count and the reader.
    ('a-refusal-not-counted', ALLOCATOR,
     '    if (reason == JSPORT_ALLOC_LIMIT) owner->refused_by_limit++;', '    (void)reason;'),
    ('the-refusals-not-read', RUNTIME,
     '                                 runtime->allocator.refused_by_limit};', '                                 0};'),
    ('the-limit-not-read', RUNTIME,
     '                                 c.limit == UINT64_MAX ? 0 : c.limit, c.runs_by_limit,',
     '                                 0, c.runs_by_limit,'),
]


def pin(manifest_text, patch_bytes):
    """The manifest with 0048's hash made the given patch's, as written."""
    manifest = json.loads(manifest_text)
    for entry in manifest['patches']:
        if entry['path'] == 'patches/0048-collect-under-limit.patch':
            entry['sha256'] = hashlib.sha256(patch_bytes).hexdigest()
    return json.dumps(manifest, indent=2, ensure_ascii=False) + ('\n' if manifest_text.endswith('\n') else '')


caught, missed, unbuilt = [], [], []
manifest_path = root / MANIFEST
manifest_original = manifest_path.read_bytes()
for name, rel, old, new in mutants:
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
        broken = text.replace(old, new)
        path.write_text(broken)
        if rel == PATCH:
            manifest_path.write_text(pin(manifest_original.decode(), broken.encode()))
        with log.open('w') as out:
            try:
                result = subprocess.run(['bash', '-c', CASES], cwd=root, stdout=out, stderr=out, timeout=TIMEOUT)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
    finally:
        path.write_bytes(original)
        manifest_path.write_bytes(manifest_original)
        if path.read_bytes() != original or manifest_path.read_bytes() != manifest_original:
            print(f'{name}: {rel} or the manifest WAS NOT RESTORED', file=sys.stderr)
            sys.exit(2)
    body = log.read_text(errors='replace')
    built = (re.search(r'\.(c|h|inc):\d+:\d+: error:', body) is None and 'undefined reference' not in body
             and 'QuickJS patch failed' not in body and 'QuickJS source hash mismatch' not in body)
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

print(f'\ncollector limit mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
