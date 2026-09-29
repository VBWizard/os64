#!/usr/bin/env python3
"""Exercise the production PTY budget and replacement paths under ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent.parent
includes=['-iquote'+str(p) for p,_,_ in os.walk(root/'kernel/include')]
includes+=['-I'+str(root/'abi/include'),'-I'+str(root/'kernel/src')]
with tempfile.TemporaryDirectory(prefix='os64-pty-budget-') as work:
    binary=Path(work)/'test'
    subprocess.run(['cc','-std=gnu11','-O1','-g','-masm=intel','-ffunction-sections','-fdata-sections',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',*includes,
        str(root/'tools/test_pty_budget_host.c'),'-Wl,--gc-sections','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
