#!/usr/bin/env python3
"""One deliberate break at a time to the xHCI service rules
(kernel/src/driver/system/usb/xhci_service.c: the drain's deadline and wrap,
what ERDP is owed, the wait bound, the visit's order), against
tools/test_xhci_service_host.sh: a mutant is CAUGHT when the suite fails,
MISSED when it passes, which means a test is missing and gets written. A
build failure is neither. A mutant that makes the drain loop forever is
caught by the timeout.

The shape is test_damage_mutants.py's: each mutant is applied to the tree's
copy of the file, the suite is run, and the file is put back from the bytes
read before the change, checked byte for byte, whatever happens. Run it from
a clean tree, and never beside another harness that edits the same tree; the
per-mutant logs are in --logs. Under tools/mutants_parallel.py it needs
`--with kernel`: the runner's copies do not carry the kernel's sources
unless asked.
"""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/xhci-service-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

SERVICE = 'kernel/src/driver/system/usb/xhci_service.c'
CASES = 'bash tools/test_xhci_service_host.sh 400'

mutants = [
    ('the-deadline-is-never-read', '		if (clock != NULL && clock(clock_ctx) >= deadline) {',
     '		if (clock != NULL && clock(clock_ctx) >= deadline && 0) {'),
    ('the-deadline-is-read-after-the-event', '		if (clock != NULL && clock(clock_ctx) >= deadline) {\n			result.work_left = true;\n			return result;\n		}\n\n		handle(ctx, ev);',
     '		handle(ctx, ev);\n		if (clock != NULL && clock(clock_ctx) >= deadline) {\n			result.work_left = true;\n			return result;\n		}\n'),
    ('a-deadline-stop-is-not-said', '		if (clock != NULL && clock(clock_ctx) >= deadline) {\n			result.work_left = true;',
     '		if (clock != NULL && clock(clock_ctx) >= deadline) {'),
    ('the-deadline-is-one-late', '		if (clock != NULL && clock(clock_ctx) >= deadline) {',
     '		if (clock != NULL && clock(clock_ctx) > deadline) {'),
    ('the-cycle-does-not-flip-at-the-wrap', '			r->cycle ^= 1;\n', ''),
    ('the-cursor-does-not-wrap', '		if (r->dequeue == r->size) {', '		if (r->dequeue == r->size + 1) {'),
    ('an-invalid-slot-is-handled', "		if ((ev->control & XHCI_TRB_CYCLE) != (r->cycle ? XHCI_TRB_CYCLE : 0))\n			return result;",
     "		if ((ev->control & XHCI_TRB_CYCLE) != (r->cycle ? XHCI_TRB_CYCLE : 0) && result.handled > 0)\n			return result;"),
    ('an-early-stop-names-the-next-slot', '			slot = (slot == 0 ? r->size : slot) - 1;', '			slot = slot;'),
    ('an-early-stop-at-the-wrap-names-slot-zero', '			slot = (slot == 0 ? r->size : slot) - 1;',
     '			slot = slot == 0 ? 0 : slot - 1;'),
    ('an-empty-stop-names-the-last-handled', '		if (drain.work_left)\n			slot = ', '		if (1)\n			slot = '),
    ('every-pass-writes', '	if (drain.handled > 0) {\n		// Stopped with work left', '	if (1) {\n		// Stopped with work left'),
    ('progress-is-not-written', '		erdp.write = !(drain.work_left && slot == r->published);', '		erdp.write = false;'),
    ('an-unchanged-pointer-is-rewritten', '		erdp.write = !(drain.work_left && slot == r->published);', '		erdp.write = true;'),
    ('an-empty-stop-at-the-published-slot-is-skipped', '		erdp.write = !(drain.work_left && slot == r->published);',
     '		erdp.write = !(slot == r->published);'),
    ('the-published-slot-is-not-remembered', '	r->published = (uint32_t)(((erdp.value & ~0xFull) - r->phys) / sizeof(xhci_trb_t));', '	(void)r; (void)erdp;'),
    ('the-busy-bit-is-retired-unread', '	} else if (ehb_owed && ehb_reads_set) {', '	} else if (ehb_owed && (ehb_reads_set || 1)) {'),
    ('the-busy-bit-is-retired-unowed', '	} else if (ehb_owed && ehb_reads_set) {', '	} else if ((ehb_owed || 1) && ehb_reads_set) {'),
    ('the-busy-bit-is-never-retired', '	} else if (ehb_owed && ehb_reads_set) {\n		erdp.write = true;',
     '	} else if (ehb_owed && ehb_reads_set) {\n		erdp.write = false;'),
    ('the-write-lacks-the-busy-bit', ' | XHCI_ERDP_EHB;\n	return erdp;', ';\n	return erdp;'),
    ('a-read-is-wanted-after-progress', '	return drain.handled == 0 && ehb_owed;', '	return (drain.handled == 0 || 1) && ehb_owed;'),
    ('a-read-is-wanted-unowed', '	return drain.handled == 0 && ehb_owed;', '	return drain.handled == 0 && (ehb_owed || 1);'),
    ('the-bound-stops-short-of-the-drain', '		uint64_t bound = ended - w->seen_empty;', '		uint64_t bound = ended - w->seen_empty - (drain.handled > 1);'),
    ('the-bound-is-never-a-maximum', '		if (bound > w->max)\n			w->max = bound;\n', ''),
    ('the-recent-maximum-is-not-kept', '		if (bound > w->recent_max)\n			w->recent_max = bound;\n', ''),
    ('the-first-bucket-takes-everything', '		if (bound <= edge[0]) w->within[0]++;', '		if (bound <= edge[2]) w->within[0]++;'),
    ('a-deadline-stop-moves-the-stamp', '	if (!drain.work_left)\n		w->seen_empty = ended;', '	w->seen_empty = ended;'),
    ('a-never-seen-empty-ring-is-bounded', '	if (drain.handled > 0 && w->seen_empty != 0) {', '	if (drain.handled > 0) {'),
    ('the-cursor-does-not-rotate', '			v->cursor = (index + 1) % v->count;\n', ''),
    ('the-cursor-stays-on-the-busy-controller', '			v->cursor = (index + 1) % v->count;', '			v->cursor = index;'),
    ('the-visit-goes-on-past-the-deadline', '			result.work_left = true;\n			return result;\n		}\n	}\n	return result;',
     '			result.work_left = true;\n		}\n	}\n	return result;'),
    ('a-stopped-visit-is-not-said', '			v->cursor = (index + 1) % v->count;\n			result.work_left = true;',
     '			v->cursor = (index + 1) % v->count;'),
    ('the-visit-always-starts-at-zero', '	uint32_t first = v->cursor;', '	uint32_t first = 0;'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / SERVICE
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {SERVICE}', file=sys.stderr)
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
            print(f'{name}: {SERVICE} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nxhci_service mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
