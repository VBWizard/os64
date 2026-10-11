"""Build the pinned decoder closure for host probes or freestanding audits."""
from pathlib import Path
import subprocess
from webp_generate import generate

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'userland/libwebp'

def sources(scalar=False):
    text = (BASE / 'sources.mk').read_text().replace('\\\n', ' ')
    groups = dict(line.split(' := ', 1) for line in text.splitlines())
    paths = groups['WEBP_CORE_SRCS'].split()
    if not scalar:
        paths += groups['WEBP_SSE2_SRCS'].split()
    return [ROOT / 'userland' / p for p in paths] + [BASE / 'port/cpu.c']

def build(work, scalar=False, guest=False, sanitize=False, probe=False):
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    generate(work)
    flags = ['-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-fno-builtin', '-fPIC', '-ffunction-sections', '-fdata-sections',
             '-fvisibility=hidden', '-msse2', '-mno-avx', '-DHAVE_CONFIG_H']
    if scalar:
        flags += ['-DOS64_WEBP_SCALAR']
    if guest:
        flags += ['-ffreestanding', '-mno-red-zone']
    else:
        flags += ['-DWEBP_HOST_ASSERT']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    includes = ['-I' + str(p) for p in [BASE / 'port/compat', BASE / 'port',
                BASE / 'upstream', ROOT / 'userland/libos64/include', ROOT / 'abi/include']]
    compiler = 'x86_64-elf-gcc' if guest else 'cc'
    objects = []
    for src in sources(scalar):
        obj = work / (src.stem + '.o')
        if src.name == 'alpha_dec.c':
            src = work / 'alpha_dec.c'
        # Probe-only instrumentation is generated outside the pristine import.
        if probe and src.name == 'vp8l_dec.c':
            text = src.read_text()
            for value in (0, 1):
                old = f'alph_dec->use_8b_decode = {value};'
                assert text.count(old) == 1
                text = text.replace(old, old + f' webp_probe_alpha({value});')
            old = 'transform->type = type;'
            assert text.count(old) == 1
            text = text.replace(old, old + ' webp_probe_transform(type);')
            src = work / src.name
            src.write_text('void webp_probe_alpha(int);\nvoid webp_probe_transform(int);\n' + text)
        subprocess.run([compiler, *flags, *includes, '-c', str(src), '-o', str(obj)], check=True)
        objects.append(obj)
    return objects
