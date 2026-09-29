#!/usr/bin/env python3
"""Run production allocator and paging refusal tests without privileged instructions."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent.parent
includes=['-iquote'+str(p) for p,_,_ in os.walk(root/'kernel/include')]
includes+=['-I'+str(root/'abi/include'),'-I'+str(root/'kernel/src')]
with tempfile.TemporaryDirectory(prefix='os64-memory-try-') as work:
    for name in ('memory','paging'):
        binary=Path(work)/name
        subprocess.run(['cc','-std=gnu11','-O1','-g','-masm=intel','-ffunction-sections','-fdata-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread',*includes,
            str(root/f'tools/test_{name}_try_host.c'),'-Wl,--gc-sections','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
