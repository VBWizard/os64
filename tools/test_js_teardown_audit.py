#!/usr/bin/env python3
"""Audit linked target allocation escape sites and process-global storage.

The callback flow and native-resource audit is recorded in libjs/TEARDOWN.md;
these checks keep its object-file premises from changing silently.
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
objects = root / 'userland/obj/js'

def run(*args):
    return subprocess.check_output(args, text=True)

for name in ('quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils'):
    obj = objects / (name + '.o')
    sections = {int(n): s for n, s in re.findall(r'\[\s*(\d+)\]\s+(\S+)\s+',
                run('x86_64-elf-readelf', '-SW', str(obj)))}
    mutable = []
    for line in run('x86_64-elf-readelf', '-sW', str(obj)).splitlines():
        fields = line.split()
        if len(fields) == 8 and fields[3] == 'OBJECT' and fields[6].isdigit():
            section = sections.get(int(fields[6]), '')
            if section.startswith(('.data', '.bss')) and not section.startswith('.data.rel.ro'):
                mutable.append((fields[7], int(fields[2])))
    assert mutable == ([('js_class_id_alloc', 4)] if name == 'quickjs' else []), (name, mutable)
    undefined = {line.split()[-1] for line in run('x86_64-elf-nm', '-u', str(obj)).splitlines()}
    direct = undefined & {'malloc', 'calloc', 'realloc', 'free', 'os64_malloc', 'os64_calloc', 'os64_realloc', 'os64_free'}
    assert direct == ({'os64_realloc'} if name in ('cutils', 'libunicode') else set()), (name, direct)
    if direct:
        fallback = 'dbuf_default_realloc' if name == 'cutils' else 'cr_default_realloc'
        nm = run('x86_64-elf-nm', '-S', str(obj))
        address, size = re.search(r'^([0-9a-f]+) ([0-9a-f]+) t ' + fallback + r'$', nm, re.M).groups()
        relocations = re.findall(r'^([0-9a-f]+).*\bos64_realloc\b',
                                run('x86_64-elf-readelf', '-rW', str(obj)), re.M)
        assert len(relocations) == 1 and int(address, 16) <= int(relocations[0], 16) < int(address, 16) + int(size, 16), name
    print(f'{name}: direct allocations and mutable globals match the audited inventory')

symbols = run('x86_64-elf-readelf', '-sW', str(objects / 'core.o'))
for name in ('jsport_free_runtime_report', 'jsport_allocator_reclaim', 'jsport_tracked_malloc_functions'):
    assert re.search(r'GLOBAL\s+HIDDEN\s+\d+\s+' + name + r'$', symbols, re.M), name
exports = run('x86_64-elf-nm', '-D', '--defined-only', str(root / 'userland/bin/libjs.so'))
assert not re.search(r'\bjsport_', exports)
print('Teardown target allocation/global/visibility audit: PASS')
