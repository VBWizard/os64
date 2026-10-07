#!/usr/bin/env python3
"""Audit the freestanding core and its binding header and symbol-only link."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
core = root / 'userland/obj/js/core.o'
math = set('acos acosh asin asinh atan atan2 atanh cbrt ceil cos cosh exp expm1 fabs floor fmax fmin fmod hypot log log10 log1p log2 lrint pow round sin sinh sqrt tan tanh trunc'.split())
services = set('os64_exit os64_free os64_hprintf os64_localtime os64_malloc os64_malloc_size os64_memchr os64_memcmp os64_memcpy os64_memmove os64_memset os64_realloc os64_strchr os64_strcmp os64_strlen os64_strrchr os64_time os64_write'.split())
runtime_services = services | {'os64_micros', 'os64_open', 'os64_read', 'os64_close'}
runtime_exports = {'os64_js_' + name for name in
                   'create context class_id eval run run_file install_output install_args drain_jobs cancel destroy check_budget create_with_teardown destroy_report'.split()}

def run(*args):
    return subprocess.check_output(args, cwd=root, text=True)

undefined = {line.split()[-1] for line in run('x86_64-elf-nm', '-u', str(core)).splitlines()}
assert undefined == math | services | {'_GLOBAL_OFFSET_TABLE_'}, sorted(undefined ^ (math | services | {'_GLOBAL_OFFSET_TABLE_'}))
symbols = run('x86_64-elf-readelf', '-sW', str(core))
for name in ['JS_NewRuntime', 'JS_NewRuntime2', 'JS_Eval', '__JS_FreeValue', '__JS_FreeValueRT', 'js_malloc', 'js_free']:
    assert re.search(r'GLOBAL\s+DEFAULT\s+\d+\s+' + name + r'$', symbols, re.M), name
for name in ['jsport_fatal', 'jsport_malloc_functions', 'memcpy', '__udivti3', '__udivmodti4']:
    assert re.search(r'GLOBAL\s+HIDDEN\s+\d+\s+' + name + r'$', symbols, re.M), name
run('x86_64-elf-gcc', '-std=c11', '-ffreestanding', '-m64', '-Wall', '-Wextra', '-Werror',
    '-I', str(root / 'userland/libmath/include'),
    '-I', str(root / 'userland/libjs/port/compat'), '-I', str(root / 'userland/libjs/port'),
    '-I', str(root / 'userland/libjs/include'), '-I', str(root / 'userland/libos64/include'),
    '-I', str(root / 'abi/include'), '-isystem', str(root / 'userland/obj/js/upstream'),
    '-fsyntax-only', str(root / 'userland/libjs/examples/binding.c'))

with tempfile.TemporaryDirectory() as work:
    work = Path(work)
    binding = work / 'binding.o'
    run('x86_64-elf-gcc', '-std=c11', '-ffreestanding', '-m64', '-Wall', '-Wextra', '-Werror',
        '-I', str(root / 'userland/libmath/include'),
        '-I', str(root / 'userland/libjs/port/compat'), '-I', str(root / 'userland/libjs/port'),
        '-I', str(root / 'userland/libjs/include'), '-I', str(root / 'userland/libos64/include'),
        '-I', str(root / 'abi/include'), '-isystem', str(root / 'userland/obj/js/upstream'),
        '-c', str(root / 'userland/libjs/examples/binding.c'), '-o', str(binding))
    # Trap-only libraries prove symbol resolution and ELF packaging. They have
    # no numerical/system behavior and cannot establish guest execution.
    for name, names in [('libmath', math), ('libos64', services)]:
        source = work / (name + '.S')
        source.write_text('.text\n' + ''.join(f'.global {n}\n.type {n},@function\n{n}: ud2\n' for n in sorted(names)))
        run('x86_64-elf-gcc', '-c', str(source), '-o', str(work / (name + '.o')))
        run('x86_64-elf-ld', '-shared', '--hash-style=sysv', '-z', 'noexecstack',
            '-soname', name + '.so', '-o', str(work / (name + '.so')), str(work / (name + '.o')))
    library = work / 'engine-link-probe.so'
    run('x86_64-elf-ld', '--defsym', 'LIB_BASE=0x00007f00c8000000', '--defsym', 'LIB_SLOT_SIZE=0x04000000',
        '-shared', '--hash-style=sysv', '-Bsymbolic-functions', '-z', 'max-page-size=0x1000',
        '-z', 'noexecstack', '-z', 'separate-code', '-T', str(root / 'userland/link/lib.ld'),
        '-soname', 'libjs.so', '--no-undefined', '--no-as-needed',
        '--version-script=' + str(root / 'userland/libjs/exports.map'), '-o', str(library),
        str(core), str(work / 'libmath.so'), str(work / 'libos64.so'))
    dynamic = run('x86_64-elf-readelf', '-dW', str(library))
    for name in ['libmath.so', 'libos64.so', 'libjs.so']:
        assert '[' + name + ']' in dynamic, name
    assert '(HASH)' in dynamic and '(TEXTREL)' not in dynamic
    exports = {line.split()[-1] for line in run('x86_64-elf-nm', '-D', '--defined-only', str(library)).splitlines()}
    assert 'JS_NewRuntime2' in exports and 'js_malloc' in exports
    binding_imports = {line.split()[-1] for line in run('x86_64-elf-nm', '-u', str(binding)).splitlines()}
    assert binding_imports - exports <= {'os64_js_class_id', 'os64_js_context'}, sorted(binding_imports - exports)
    assert '__JS_FreeValue' in exports and '__JS_FreeValueRT' in exports
    assert all(n.startswith(('JS_', '__JS_')) or re.match(r'js_(malloc|free|realloc|strdup)', n) for n in exports), sorted(exports)
    headers = run('x86_64-elf-readelf', '-lW', str(library))
    assert not re.search(r'LOAD.*RWE', headers)
    print(f'QuickJS target: 32 math / 18 os64 imports; binding header; {len(exports)} engine exports; ELF symbol-only link: PASS')

# Check the production dependency libraries separately from the trap fixture.
library = root / 'userland/bin/libjs.so'
dynamic = run('x86_64-elf-readelf', '-dW', str(library))
assert set(re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)) == {'libmath.so', 'libos64.so'}
assert '(HASH)' in dynamic and '(TEXTREL)' not in dynamic and '[libjs.so]' in dynamic
imports = {line.split()[-1] for line in run('x86_64-elf-nm', '-D', '-u', str(library)).splitlines()}
assert imports == math | runtime_services, sorted(imports ^ (math | runtime_services))
for dependency, names in [('libmath', math), ('libos64', runtime_services)]:
    provided = {line.split()[-1] for line in run('x86_64-elf-nm', '-D', '--defined-only',
                str(root / f'userland/bin/{dependency}.so')).splitlines()}
    assert names <= provided, sorted(names - provided)
real_exports = {line.split()[-1] for line in run('x86_64-elf-nm', '-D', '--defined-only', str(library)).splitlines()}
assert real_exports == exports | runtime_exports, sorted(real_exports ^ (exports | runtime_exports))
assert not re.search(r'LOAD.*RWE', run('x86_64-elf-readelf', '-lW', str(library)))
for name in ['quickjs.d', 'port/format.d']:
    dependencies = (root / 'userland/obj/js' / name).read_text()
    assert str(root / 'userland/libmath/include/math.h') in dependencies, name
    assert '/libjs/port/compat/math.h' not in dependencies, name

# Simulate either link input becoming newer without changing source timestamps.
# Freeze the binary inputs so a transitive dependency cannot mask a missing
# direct edge from the recipe or placement assigner to libjs.so.
idle = run('make', '-C', str(root / 'userland'), '-n', 'js-library')
assert '-soname libjs.so' not in idle, 'production library is not up to date'
for prerequisite in ['libjs/shared.mk', 'tools/app_bases.py']:
    recipe = run('make', '-C', str(root / 'userland'), '-n',
                 '-o', str(core), '-o', str(root / 'userland/bin/libos64.so'),
                 '-o', str(root / 'userland/obj/js/runtime/runtime.o'),
                 '-o', str(root / 'userland/bin/libmath.so'), '-W', prerequisite, 'js-library')
    assert '-soname libjs.so' in recipe, prerequisite
print(f'QuickJS production link: {len(runtime_exports)} runtime exports; 32 math / {len(runtime_services)} os64 imports; real dependencies, public maths header and relink triggers: PASS (no guest execution)')
