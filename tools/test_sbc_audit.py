#!/usr/bin/env python3
"""Audit the kernel's SBC encoder: the import is what manifest.json pins, and
the objects the kernel links meet the rule for code in ring 0.

The rule (BLUETOOTH_AUDIO.md § Where the encoder lives, ruled 2026-11-12):
integer-only, allocates nothing, and host-tested before the kernel links it.
tools/test_sbc_host.sh is the testing; this reads what was built. It builds
the port's objects itself (make -C kernel, those targets only), so what it
judges is what the sources in the tree build to, not an object left over.

Each check names the property it protects, so a failure says what was lost.
"""
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KERNEL = ROOT / 'kernel'
PORT = KERNEL / 'ports/sbc'
UPSTREAM = PORT / 'upstream'
LICENCE = ROOT / 'license/android-sbc-LICENSE'


def run(*args, cwd=None):
    return subprocess.check_output([str(a) for a in args], text=True, cwd=cwd)


def check(cond, what):
    if not cond:
        raise SystemExit(f'FAIL {what}')


def git_blob(data):
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def git_tree(directory):
    """The id git gives a directory of plain files: what the pinned commit's
    tree names, so a match ties these bytes to that commit."""
    body = b''
    for p in sorted(directory.iterdir(), key=lambda p: p.name):
        check(p.is_file(), f'{p} is not a plain file')
        body += b'100644 ' + p.name.encode() + b'\0' + bytes.fromhex(git_blob(p.read_bytes()))
    return hashlib.sha1(b'tree %d\0' % len(body) + body).hexdigest()


# ── The import ────────────────────────────────────────────────────────────
# Every vendored file is what manifest.json pins, by SHA-256 and by git blob
# id, and each directory is, as a whole, the tree the pinned commit names.
# Nothing is vendored that the manifest does not name, and nothing is patched.
manifest = json.loads((PORT / 'manifest.json').read_text())
present = {str(p.relative_to(UPSTREAM)) for p in UPSTREAM.rglob('*') if p.is_file()}
check(present == set(manifest['files']), f'upstream/ and manifest.json disagree: {sorted(present ^ set(manifest["files"]))}')
for name, pin in manifest['files'].items():
    data = (UPSTREAM / name).read_bytes()
    check(hashlib.sha256(data).hexdigest() == pin['sha256'], f'{name} differs from manifest.json (sha256)')
    check(git_blob(data) == pin['git_blob'], f'{name} differs from manifest.json (git blob)')
for directory, tree in manifest['trees'].items():
    check(git_tree(UPSTREAM / directory) == tree, f'upstream/{directory} is not the tree {tree} of commit {manifest["commit"]}')
check(re.fullmatch(r'[0-9a-f]{40}', manifest['commit']) is not None, 'manifest.json names no commit')
check(manifest['patches'] == [] and not (PORT / 'patches').exists(), 'the port is unpatched, and something says otherwise')

# Every vendored translation unit is built, and only vendored ones are.
sources = (PORT / 'sources.mk').read_text()
built = set(re.findall(r'ports/sbc/upstream/(\S+\.c)', sources))
vendored = {f for f in manifest['files'] if f.endswith('.c')}
check(built == vendored, f'sources.mk and the vendored sources disagree: {sorted(built ^ vendored)}')

# The configuration is a decision on this side: every switch upstream's
# headers default (`#ifndef NAME` that is not an include guard) is set in
# sbc_switches.h, so a newer header cannot change the build by defaulting a
# new one.
defaulted = set()
for name in manifest['files']:
    if name.endswith('.h'):
        defaulted |= {m for m in re.findall(r'^#ifndef\s+(\w+)', (UPSTREAM / name).read_text(), re.M)
                      if not m.endswith('_H')}
switches = {m for m in re.findall(r'^#define\s+(\w+)', (PORT / 'port/sbc_switches.h').read_text(), re.M)
            if m != 'SBC_SWITCHES_H'}
check(defaulted <= switches, f'sbc_switches.h does not set {sorted(defaulted - switches)}')
check(switches <= defaulted, f'sbc_switches.h sets {sorted(switches - defaulted)}, which upstream does not read')

# ── The objects ───────────────────────────────────────────────────────────
objects = [f'obj/{s[:-2]}.c.o' for s in re.findall(r'ports/sbc/\S+\.c', sources)] + ['obj/audio/sbc_frame.c.o']
subprocess.run(['make', '-s', '-C', str(KERNEL), *objects], check=True, stdout=subprocess.DEVNULL)
paths = [KERNEL / o for o in objects]

# Integer-only. The kernel is built -mno-sse, but nothing turns off the x87,
# so a float or a double would compile, to x87 code. Read every instruction:
# an x87 one (every x87 mnemonic begins with f), an MMX or SSE register, or
# an instruction that touches SSE state.
# Every SSE instruction names an xmm register except the few matched by name.
FPU_MNEMONIC = re.compile(r'^(f\w+|emms|ldmxcsr|stmxcsr|movnti)$')
FPU_OPERAND = re.compile(r'\b([xyz]mm\d+|mm\d|st|st\(\d\))\b')


def floating_point(obj):
    found = []
    for line in run('x86_64-elf-objdump', '-d', '-M', 'intel', '--no-show-raw-insn', obj).splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+(\S+)\s*(.*)$', line)
        if not m:
            continue
        # Symbol annotations (<name+0x10>) are names, not operands.
        mnemonic, operands = m.group(1), re.sub(r'<[^>]*>', '', m.group(2))
        if FPU_MNEMONIC.match(mnemonic) or FPU_OPERAND.search(operands):
            found.append(line.strip())
    return found


# The scan must be able to see what it looks for: arithmetic on a double
# behind an integer interface (what a switch flipped to a floating-point
# table would compile to), built by the same compiler with the kernel's
# flags (x87) and with SSE allowed (SSE).
with tempfile.TemporaryDirectory() as tmp:
    probe = Path(tmp) / 'probe.c'
    probe.write_text('int f(int a, int b) { double x = a; return (int)(x * b * 0.7071); }\n')
    for flags, kind in ((['-mno-sse', '-mno-sse2', '-mno-mmx'], 'x87'), (['-msse2'], 'SSE')):
        out = Path(tmp) / f'probe-{kind}.o'
        run('x86_64-elf-gcc', '-O2', '-ffreestanding', '-mcmodel=kernel', *flags, '-c', probe, '-o', out)
        check(floating_point(out), f'the instruction scan does not see {kind} code')

for obj in paths:
    found = floating_point(obj)
    check(not found, f'{obj.relative_to(ROOT)} has floating-point or SIMD instructions: {found[:5]}')

# Allocates nothing, and leans on nothing: across the port's objects (and
# sbc_frame.c's, which the boundary calls) the one name left undefined is
# memset.
defined, undefined = set(), set()
for obj in paths:
    defined |= {l.split()[-1] for l in run('x86_64-elf-nm', '--defined-only', '-g', obj).splitlines()}
    undefined |= {l.split()[-1] for l in run('x86_64-elf-nm', '-u', obj).splitlines()}
check(undefined - defined == {'memset'}, f'the port reaches outside itself for {sorted(undefined - defined)}')

# The boundary is the kernel's only way in: its object exports the two
# functions of sbc_port.h and nothing else (no test hook in the kernel's
# build), and nothing in the kernel outside ports/sbc names upstream's
# header or its functions.
port_obj = KERNEL / 'obj/ports/sbc/port/sbc_port.c.o'
exported = {l.split()[-1] for l in run('x86_64-elf-nm', '--defined-only', '-g', port_obj).splitlines()}
check(exported == {'sbc_port_begin', 'sbc_port_encode'}, f'sbc_port.c.o exports {sorted(exported)}')
reach = re.compile(r'sbc_encoder\.h|sbc_enc_func_declare\.h|\bSBC_Encode\b|\bSBC_Encoder_Init\b|SBC_ENC_PARAMS')
for tree in ('src', 'include', 'test'):
    for p in (KERNEL / tree).rglob('*'):
        if p.suffix in ('.c', '.h', '.S') and p.is_file():
            check(not reach.search(p.read_text(errors='replace')), f'{p.relative_to(ROOT)} reaches past the boundary')

# ── The notice ────────────────────────────────────────────────────────────
# Apache 2.0 asks that a redistribution carry the licence and the NOTICE, and
# keep the files' own notices. The licence file holds the module's NOTICE as
# fetched (pinned by its digest) and the leading comment of every vendored
# file, so every copyright holder they name is named there.
notice_text = LICENCE.read_text()
check('Apache License' in notice_text and 'Version 2.0, January 2004' in notice_text, 'the licence file has no Apache 2.0 text')
bar = '=' * 72
marker = f'{bar}\nNOTICE\n{bar}\n\n'
check(marker in notice_text, f'{LICENCE.name} has no NOTICE section')
begin = notice_text.index(marker) + len(marker)
carried = notice_text[begin:notice_text.index(f'\n{bar}\n', begin)].encode()
check(hashlib.sha256(carried).hexdigest() == manifest['licence']['sha256'],
      f'the NOTICE in {LICENCE.name} is not the one manifest.json pins')
holders = set()
for name in manifest['files']:
    text = (UPSTREAM / name).read_text()
    head = text[:text.index('*/') + 2]
    check(head in notice_text, f'{LICENCE.name} does not carry the notice at the head of {name}')
    holders |= set(re.findall(r'Copyright\s+(.+)', text))
check(holders, 'no copyright line found in upstream')
for holder in holders:
    check(holder.strip() in notice_text, f'{LICENCE.name} does not name {holder.strip()}')

print(f'PASS sbc audit: {len(manifest["files"])} pinned files at {manifest["commit"][:12]}, '
      f'{len(paths)} objects integer-only, undefined {sorted(undefined - defined)}, '
      f'holders {sorted(h.strip() for h in holders)}')
