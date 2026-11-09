#!/usr/bin/env python3
"""Placement persistence/ownership and queue regressions, including queue mutants."""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path)
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--run-only', action='store_true')
args = parser.parse_args()
out = args.output or Path(tempfile.mkdtemp(prefix='window-placement-host-'))
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
queue = (root / 'kernel/src/gui/event_queue.c').read_text()
header = (root / 'kernel/include/gui/event_queue.h').read_text()
mutations = {
    'legacy_empty_ring_priority': ('if (q->doorbell_pending && q->head == q->tail && !q->placement_pending)', 'if (false)'),
    'snapshot_overwrite': ('q->placement.placement.changed |= mask;', 'q->placement = *ev;'),
    'stale_mask': ('q->placement = (input_event_t){0};', '/* mutant: retain previous mask */'),
    'placement_starves_ring': ('q->placement_pending = false;\n        q->service_next = 2;',
                              'q->placement_pending = false;\n        q->service_next = 0;'),
    'doorbell_starves_placement': ('&& !q->placement_pending)', ')'),
    'appearance_loses_priority': ('if (q->appearance_pending) {', 'if (q->appearance_pending && !q->doorbell_pending) {'),
    'pointer_precedes_drain': ('unsigned source = 3;', 'if (q->pointer_pending) { *out=q->pointer; q->pointer_pending=false; return true; }\n    unsigned source = 3;'),
    'loss_starves_control': ('!q->lost_yield_control || q->head == q->tail', 'q->lost_pending || q->head == q->tail'),
}
names = ['queue', 'placement', *mutations, 'pending_omits_placement']
if not args.run_only:
    includes = ['-I'+str(root / p) for p in ['abi/include', 'kernel/include', 'userland/libos64/include']]
    def compile_queue(name, source, include=None):
        directory = out / name
        directory.mkdir(exist_ok=True)
        (directory / 'queue.c').write_text(source)
        extra = []
        if include is not None:
            (directory / 'gui').mkdir(exist_ok=True)
            (directory / 'gui/event_queue.h').write_text(include)
            extra = ['-I'+str(directory)]
        subprocess.run(['cc', *flags, *extra, *includes,
                        str(root / 'tools/test_window_placement_queue_host.c'),
                        str(directory / 'queue.c'), '-o', str(out / (name+'.bin'))], check=True)
    compile_queue('queue', queue)
    for name, (before, after) in mutations.items():
        assert before in queue, name
        compile_queue(name, queue.replace(before, after, 1))
    needle = '|| q->placement_pending'
    assert needle in header
    compile_queue('pending_omits_placement', queue, header.replace(needle, '', 1))
    sources = ['tools/test_window_placement_host.c'] + [
        'userland/libos64/'+name+'.c' for name in
        ['window_placement', 'ui_window', 'conf', 'slurp', 'str', 'fmt']]
    subprocess.run(['cc', *flags, *includes, '-include', str(root / 'tools/test_file_dialog_syscalls.h'),
                    *[str(root / s) for s in sources], '-o', str(out / 'placement.bin')], check=True)
print('Artifacts:', out, flush=True)
if args.build_only:
    raise SystemExit(0)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1'}
for name in names:
    result = subprocess.run([str(out / (name+'.bin'))], env=env, capture_output=True, text=True)
    (out / (name+'.log')).write_text(result.stdout+result.stderr)
    if name in ['queue', 'placement']:
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        print(result.stdout.strip())
    else:
        assert result.returncode != 0 and 'Assertion' in result.stderr, (name, 'mutant survived or failed for the wrong reason', result.stderr)
        print('Killed queue mutant:', name)
