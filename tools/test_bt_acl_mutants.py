#!/usr/bin/env python3
"""One deliberate break at a time to the controller's ACL buffer accounting
(kernel/src/driver/system/usb/bt_acl.c: the two pool layouts, the reserve
kept for LE in a shared pool, completions by handle, a link going down),
against tools/test_bt_acl_host.sh: a mutant is CAUGHT when the suite fails,
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
parser.add_argument('--logs', type=pathlib.Path, default=pathlib.Path('/tmp/bt-acl-mutants'))
parser.add_argument('--only', help='run the mutants whose name contains this')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
args.logs.mkdir(parents=True, exist_ok=True)

ACL = 'kernel/src/driver/system/usb/bt_acl.c'
CASES = 'bash tools/test_bt_acl_host.sh 300'

mutants = [
    ('half-an-le-pool-is-a-pool', '    bool own=le_size && le_total;',
     '    bool own=le_size || le_total;'),
    ('a-length-alone-is-an-le-pool', '    bool own=le_size && le_total;',
     '    bool own=le_size;'),
    ('a-count-alone-is-an-le-pool', '    bool own=le_size && le_total;',
     '    bool own=le_total;'),
    ('the-sizes-are-never-known', '    a->known=true;\n',
     ''),
    ('the-classic-count-is-the-size', '    a->classic_size=classic_size; a->classic_total=classic_total;',
     '    a->classic_size=classic_size; a->classic_total=classic_size+classic_total*0;'),
    ('shared-is-never', 'bool bt_acl_shared(const bt_acl_t *a) { return a->le_total==0; }',
     'bool bt_acl_shared(const bt_acl_t *a) { return a->le_total==0 && a->classic_total==0xffff; }'),
    ('a-dead-link-is-found', '        if(a->link[i].live && a->link[i].handle==handle) return &a->link[i];\n    return NULL;\n}\nstatic const',
     '        if(a->link[i].handle==handle) return &a->link[i];\n    return NULL;\n}\nstatic const'),
    ('a-handle-may-be-there-twice', '    if(acl_find(a,handle)) return false;\n',
     ''),
    ('a-new-link-is-classic', '(bt_acl_link_t){.handle=handle,.le=le,.live=true};',
     '(bt_acl_link_t){.handle=handle,.le=le&&0,.live=true};'),
    ('a-full-table-takes-a-link', '        return true;\n    }\n    return false;\n}\n\nvoid bt_acl_link_down',
     '        return true;\n    }\n    return true;\n}\n\nvoid bt_acl_link_down'),
    ('a-link-going-keeps-its-packets', '    if(l) *l=(bt_acl_link_t){0};',
     '    if(l) l->live=false;'),
    ('a-link-never-goes', '    if(l) *l=(bt_acl_link_t){0};',
     '    (void)l;'),
    ('le-is-given-the-le-size-in-a-shared-pool', '    return le && !bt_acl_shared(a) ? a->le_size : a->classic_size;',
     '    return le ? a->le_size : a->classic_size;'),
    ('classic-is-given-the-le-size', '    return le && !bt_acl_shared(a) ? a->le_size : a->classic_size;',
     '    return (le||1) && !bt_acl_shared(a) ? a->le_size : a->classic_size;'),
    ('a-reserve-in-two-pools', '    if(!bt_acl_shared(a) || !a->classic_total) return 0;',
     '    if(!a->classic_total) return 0;'),
    ('a-reserve-out-of-nothing', '    if(!bt_acl_shared(a) || !a->classic_total) return 0;',
     '    if(!bt_acl_shared(a)) return 0;'),
    ('out-counts-links-not-packets', '        if(l->le?want_le:want_classic) n+=l->out;',
     '        if(l->le?want_le:want_classic) n+=l->live;'),
    ('the-reserve-counts-classic-links', 'if(a->link[i].live && a->link[i].le) le++;',
     'if(a->link[i].live) le++;'),
    ('the-reserve-counts-dead-links', 'if(a->link[i].live && a->link[i].le) le++;',
     'if(a->link[i].le || a->link[i].live) le++;'),
    ('the-reserve-may-take-everything', '    return (uint16_t)(le<a->classic_total ? le : a->classic_total-1u);',
     '    return (uint16_t)(le<a->classic_total ? le : a->classic_total);'),
    ('the-reserve-is-one-short', '    return (uint16_t)(le<a->classic_total ? le : a->classic_total-1u);',
     '    return (uint16_t)(le<=a->classic_total ? le : a->classic_total-1u);'),
    ('le-overruns-its-own-pool', '        return l->le ? acl_out(a,true,false)<a->le_total :',
     '        return l->le ? acl_out(a,true,false)<=a->le_total :'),
    ('classic-overruns-its-own-pool', 'acl_out(a,false,true)<a->classic_total;\n    if(l->le)',
     'acl_out(a,false,true)<=a->classic_total;\n    if(l->le)'),
    ('le-is-counted-against-classic-in-two-pools', '        return l->le ? acl_out(a,true,false)<a->le_total : acl_out(a,false,true)<a->classic_total;',
     '        return l->le ? acl_out(a,true,false)<a->le_total : acl_out(a,true,true)<a->classic_total;'),
    ('le-ignores-classic-in-a-shared-pool', '    if(l->le) return acl_out(a,true,true)<a->classic_total;',
     '    if(l->le) return acl_out(a,true,false)<a->classic_total;'),
    ('le-overruns-a-shared-pool', '    if(l->le) return acl_out(a,true,true)<a->classic_total;',
     '    if(l->le) return acl_out(a,true,true)<=a->classic_total;'),
    ('classic-takes-the-reserve', '    return acl_out(a,false,true)+bt_acl_reserved_for_le(a)<a->classic_total &&',
     '    return acl_out(a,false,true)<a->classic_total &&'),
    ('classic-overruns-a-shared-pool', '           acl_out(a,true,true)<a->classic_total;',
     '           acl_out(a,false,true)<a->classic_total;'),
    ('the-reserve-is-charged-for-le-packets-too', '    return acl_out(a,false,true)+bt_acl_reserved_for_le(a)<a->classic_total &&',
     '    return acl_out(a,true,true)+bt_acl_reserved_for_le(a)<a->classic_total &&'),
    ('a-send-is-not-counted', '    l->out++;\n    return true;',
     '    return true;'),
    ('a-send-on-no-link-is-accepted', '    if(!l) return false;\n    l->out++;',
     '    if(!l) return true;\n    l->out++;'),
    ('any-event-is-a-completion', '    if(n<3 || e[0]!=0x13 || n!=2u+e[1] || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;',
     '    if(n<3 || n!=2u+e[1] || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;'),
    ('the-length-byte-is-not-checked', '    if(n<3 || e[0]!=0x13 || n!=2u+e[1] || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;',
     '    if(n<3 || e[0]!=0x13 || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;'),
    ('the-entry-count-is-not-checked', '    if(n<3 || e[0]!=0x13 || n!=2u+e[1] || n!=3u+4u*e[2]) return BT_ACL_MALFORMED;',
     '    if(n<3 || e[0]!=0x13 || n!=2u+e[1] || n<3u+4u*e[2]) return BT_ACL_MALFORMED;'),
    ('the-flag-bits-are-handle', '        uint16_t handle=(uint16_t)((p[0]|p[1]<<8)&0xfff);',
     '        uint16_t handle=(uint16_t)(p[0]|p[1]<<8);'),
    ('the-handle-is-one-byte', '        uint16_t handle=(uint16_t)((p[0]|p[1]<<8)&0xfff);',
     '        uint16_t handle=(uint16_t)(p[0]&0xfff);'),
    ('the-count-is-one-byte', '        uint16_t count=(uint16_t)(p[2]|p[3]<<8);',
     '        uint16_t count=(uint16_t)(p[2]);'),
    ('every-entry-is-the-first', '        const uint8_t *p=e+3+4*i;',
     '        const uint8_t *p=e+3+4*i*0;'),
    ('a-stray-is-not-counted', '        if(!l || !count) { a->strays++; continue; }',
     '        if(!l || !count) { continue; }'),
    ('a-zero-count-is-not-a-stray', '        if(!l || !count) { a->strays++; continue; }',
     '        if(!l) { a->strays++; continue; }'),
    ('exactly-what-is-out-is-an-overcount', '        if(count>l->out) {',
     '        if(count>=l->out) {'),
    ('an-overcount-wraps', '        if(count>l->out) {\n',
     '        if(count>l->out && 0) {\n'),
    ('an-overcount-names-the-last', '            if(verdict==BT_ACL_COUNTED) *link=handle;',
     '            *link=handle;'),
    ('an-overcount-names-nobody', '            if(verdict==BT_ACL_COUNTED) *link=handle;',
     '            if(verdict==BT_ACL_COUNTED) (void)link;'),
    ('an-overcount-is-not-said', '            verdict=BT_ACL_OVERCOUNT; l->out=0;',
     '            l->out=0;'),
    ('an-overcount-keeps-the-count', '            verdict=BT_ACL_OVERCOUNT; l->out=0;',
     '            verdict=BT_ACL_OVERCOUNT;'),
    ('an-overcount-ends-the-event', '            verdict=BT_ACL_OVERCOUNT; l->out=0;\n            continue;',
     '            verdict=BT_ACL_OVERCOUNT; l->out=0;\n            break;'),
    ('a-completion-gives-one-back', '        l->out-=count;',
     '        l->out-=1;'),
    ('in-use-ignores-le-in-a-shared-pool', '    if(bt_acl_shared(a)) return (uint16_t)acl_out(a,true,true);',
     '    if(bt_acl_shared(a)) return (uint16_t)acl_out(a,false,true);'),
    ('in-use-answers-the-other-pool', '    return (uint16_t)(le_pool?acl_out(a,true,false):acl_out(a,false,true));',
     '    return (uint16_t)(le_pool?acl_out(a,false,true):acl_out(a,true,false));'),
]

caught, missed, unbuilt = [], [], []
for name, old, new in mutants:
    if args.only and args.only not in name:
        continue
    path = root / ACL
    original = path.read_bytes()
    text = original.decode()
    if text.count(old) != 1:
        print(f'{name}: the text to break is not found once in {ACL}', file=sys.stderr)
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
            print(f'{name}: {ACL} WAS NOT RESTORED', file=sys.stderr)
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

print(f'\nbt_acl mutants: {len(caught)} caught, {len(missed)} missed, {len(unbuilt)} did not build')
if missed:
    print('missed: ' + ', '.join(missed))
if unbuilt:
    print('did not build: ' + ', '.join(unbuilt))
sys.exit(1 if missed or unbuilt else 0)
