#!/usr/bin/env python3
"""Run target-compiled private runtime objects without a host libc substitute.

The Linux launcher supplies only process entry/exit; runtime_test.c also runs
inside os64's fonttest. --mutation verifies the build-policy refusal and reports the functional
effect of dropping the loop-pattern flag on the current target compiler.
"""
import argparse
from pathlib import Path
import resource
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
USER = ROOT / 'userland'
RUNTIME = USER / 'obj/freetype/libfreetype/port/runtime.c.o'
JUMP = USER / 'obj/freetype/libfreetype/port/nonlocal.S.o'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation', action='store_true')
    args = parser.parse_args()
    subprocess.run(['make', '-C', str(USER), str(RUNTIME), str(JUMP)], check=True)
    with tempfile.TemporaryDirectory(prefix='ft-target-runtime-') as tmp:
        tmp = Path(tmp)
        start = tmp / 'start.S'
        start.write_text('''.intel_syntax noprefix
.global _start
.text
_start:
    and rsp, -16
    call os64_font_runtime_test
    mov edi, eax
    mov eax, 60
    syscall
.section .note.GNU-stack,"",@progbits
''')
        flags = ['-O2', '-m64', '-ffreestanding', '-fno-pic', '-fno-pie',
                 '-mno-red-zone', '-fno-stack-protector', '-fcf-protection=none',
                 '-fno-builtin', '-Wall', '-Wextra', '-Werror']
        test_obj, start_obj = tmp / 'test.o', tmp / 'start.o'
        subprocess.run(['x86_64-elf-gcc', *flags, '-c', str(USER / 'tests/fonttest/runtime_test.c'), '-o', str(test_obj)], check=True)
        subprocess.run(['x86_64-elf-gcc', '-c', str(start), '-o', str(start_obj)], check=True)

        def run(runtime):
            binary = tmp / 'runtime-test'
            subprocess.run(['x86_64-elf-ld', '-z', 'noexecstack', '-e', '_start', '-o', str(binary),
                            str(start_obj), str(test_obj), str(runtime), str(JUMP)], check=True)
            try:
                return subprocess.run([str(binary)], timeout=3,
                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0))).returncode
            except subprocess.TimeoutExpired:
                return 'timeout'

        status = run(RUNTIME)
        if status != 0:
            raise SystemExit(f'FAIL: production target runtime: {status}')
        print('PASS: production target runtime, private aliases and nonlocal return (Linux entry/exit only)', flush=True)
        if args.mutation:
            dry = subprocess.check_output(['make', '-s', '-n', '-B', str(RUNTIME)], cwd=USER, text=True)
            command = next(shlex.split(line) for line in dry.splitlines()
                           if '-c libfreetype/port/runtime.c ' in line)
            policy = subprocess.run(['make', '-s', '-n', str(RUNTIME), 'FREETYPE_FLAGS=-O2'],
                                    cwd=USER, capture_output=True, text=True)
            if policy.returncode == 0 or 'private runtime requires' not in policy.stderr:
                raise SystemExit('FAIL: missing runtime flag was not refused by the build')
            print('PASS: build rejects removal of the loop-pattern policy flag')
            command.remove('-fno-tree-loop-distribute-patterns')
            mutant = tmp / 'mutant.o'
            command[command.index('-o') + 1] = str(mutant)
            subprocess.run(command, cwd=USER, check=True)
            status = run(mutant)
            print(f'OBSERVED: flag-removal runtime variant returned {status}; '
                  'this observation is separate from the build-policy check')


if __name__ == '__main__':
    main()
