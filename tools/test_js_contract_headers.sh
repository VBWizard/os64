#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
python3 - <<'PY'
from pathlib import Path
import hashlib, json
root = Path('userland/libjs')
manifest = json.loads((root / 'manifest.json').read_text())
for entry in manifest['retained_files'] + manifest['patches']:
    assert hashlib.sha256((root / entry['path']).read_bytes()).hexdigest() == entry['sha256'], entry['path']
print('QuickJS source manifest: PASS')
PY
for source in userland/libjs/examples/runner.c userland/libjs/examples/jobs.c; do
    x86_64-elf-gcc -std=c11 -ffreestanding -m64 -Wall -Wextra -Werror \
        -I userland/libjs/include -fsyntax-only "$source"
done
# Binding syntax uses upstream's host headers here; R1 target compatibility
# headers remain separate work. Do not describe this as an os64 binding build.
cc -std=c11 -Wall -Wextra -Werror -I userland/libjs/include \
    -isystem userland/libjs/upstream -fsyntax-only userland/libjs/examples/binding.c
printf 'Runtime cross-header and host-binding syntax: PASS\n'
