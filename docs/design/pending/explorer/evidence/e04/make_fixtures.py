#!/usr/bin/env python3
"""Add Roam fixtures to an offline scratch ext2 image and scratch FAT partition."""
import argparse
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('scratch', type=Path)
args = parser.parse_args()
root = args.scratch.resolve()
fixtures = root / 'fixtures'
fixtures.mkdir(exist_ok=True)
(fixtures / 'notes.txt').write_text('Roam directory fixture.\n')
(fixtures / 'empty-file').write_bytes(b'')
commands = ['mkdir /roam-test']
commands += ['mkdir /roam-test/' + name for name in ('empty', 'Alpha', 'Beta', 'large')]
for name in ('notes.txt', 'two words.txt', '.hidden', 'café.txt', 'long_' + 'x' * 230 + '.txt'):
    commands.append(f'write "{fixtures}/notes.txt" "/roam-test/{name}"')
commands += [f'write "{fixtures}/empty-file" /roam-test/large/item_{i:05d}.txt'
             for i in range(10001)]
script = root / 'fixture.debugfs'
script.write_text('\n'.join(commands) + '\n')
with (root / 'fixture-ext2.txt').open('w') as log:
    subprocess.run(['debugfs', '-w', '-f', str(script), str(root / 'ext2.img')],
                   stdout=log, stderr=log, check=True)
fat = str(root / 'os64.img') + '@@1048576'
for directory in ('/roam-test', '/roam-test/empty', '/roam-test/Alpha'):
    subprocess.run(['mmd', '-i', fat, '::' + directory], check=True)
for name in ('notes.txt', 'two words.txt'):
    subprocess.run(['mcopy', '-i', fat, str(fixtures / 'notes.txt'),
                    '::/roam-test/' + name], check=True)
