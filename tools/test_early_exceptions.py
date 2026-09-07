#!/usr/bin/env python3
"""Check fatal reports before CLS exists, using disposable QEMU instances.

Run after make: python3 tools/test_early_exceptions.py OUTDIR
GDB injects #UD or a first-page #PF into guest RAM; the ISO is unchanged.
Both the default reporter and EXCOLD must finish without reading GS:0.
Injection happens after paging and video initialization, before CLS setup.
"""

import argparse
from pathlib import Path
import subprocess
import time


GDB_TEST = r'''
set pagination off
set confirm off
tbreak hardware_init
continue
set kUseOldExceptions = $legacy
tbreak logging_queueing_init
continue
python
assert not int(gdb.parse_and_eval('kCLSInitialized'))
assert int(gdb.parse_and_eval('$gs_base')) == 0
assert int(gdb.parse_and_eval('kKernelPML4v')) != 0
# Stop at the final outer unlock, after the complete report and log drain.
finished = False
class ReportEnd(gdb.Breakpoint):
    def stop(self):
        global finished
        finished = int(gdb.parse_and_eval('kWireDepth')) == 1
        return finished
class CLSRead(gdb.Breakpoint):
    def stop(self):
        print('FAIL: early exception read CLS')
        return True
ReportEnd('exception_wire_unlock')
CLSRead('get_core_local_storage')
# UD2, or mov rax, moffs64 at address 1. Neither changes the disk image.
code = bytes.fromhex('{code}')
gdb.selected_inferior().write_memory(int(gdb.parse_and_eval('$rip')), code)
end
continue
python
assert finished, 'report did not reach its final unlock'
print('PASS early exception report completed without CLS')
end
detach
quit
'''


def run_case(root, iso, kernel, legacy, pagefault):
    name = f"{'old' if legacy else 'new'}-{'pf' if pagefault else 'ud'}"
    run = root / name
    run.mkdir()
    sock = run / 'gdb.sock'
    serial = run / 'serial.log'
    script = run / 'test.gdb'
    script.write_text(GDB_TEST.format(
        code='48a10100000000000000' if pagefault else '0f0b',
    ))
    with (run / 'qemu.log').open('w') as qlog:
        guest = subprocess.Popen([
            'qemu-system-x86_64', '-machine', 'q35', '-m', '8g', '-smp', '8',
            '-no-reboot', '-display', 'none', '-serial', f'file:{serial}',
            '-gdb', f'unix:{sock},server=on,wait=off', '-S', '-cdrom', str(iso),
        ], stdout=qlog, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 5
            while not sock.exists():
                if guest.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f'{name}: QEMU failed to start; see {run}')
                time.sleep(.05)
            result = subprocess.run([
                'gdb', '-nx', '-q', '-batch', str(kernel),
                '-ex', f'target remote {sock}',
                '-ex', f'set $legacy = {int(legacy)}', '-x', str(script),
            ], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
            (run / 'gdb.log').write_text(result.stdout)
            if result.returncode or 'PASS early exception' not in result.stdout:
                raise RuntimeError(f'{name}: GDB regression failed; see {run}')
            report = serial.read_text()
            if pagefault:
                assert 'no task context (early boot?)' in report, report
                assert '0x0000000000000001' in report, report
            else:
                assert 'invalid opcode' in report.lower(), report
            assert 'No current task' in report, report
            if 'GS_BASE' in report:
                assert 'not initialized (early boot)' in report, report
                assert 'GS IS WRONG' not in report, report
            print(f'PASS {name}', flush=True)
        finally:
            if guest.poll() is None:
                guest.terminate()
            guest.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('outdir', type=Path)
    parser.add_argument('--iso', type=Path, default=Path('os64_kernel.iso'))
    parser.add_argument('--kernel', type=Path, default=Path('kernel/bin/os64_kernel'))
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    for legacy in (False, True):
        for pagefault in (False, True):
            run_case(args.outdir.resolve(), args.iso.resolve(), args.kernel.resolve(),
                     legacy, pagefault)


if __name__ == '__main__':
    main()
