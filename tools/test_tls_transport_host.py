#!/usr/bin/env python3
"""Compile the real transport adapter with deterministic byte-engine/OS seams."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='tls-transport-') as directory:
    binary = Path(directory) / 'transport'
    subprocess.run(['cc', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'userland/libtls/include'),
                    '-I'+str(root/'userland/libos64/include'), '-I'+str(root/'abi/include'),
                    str(root/'userland/libtls/transport.c'), str(root/'tools/test_tls_transport_host.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
