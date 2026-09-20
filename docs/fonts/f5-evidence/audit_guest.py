#!/usr/bin/env python3
"""Audit a stopped private F5 guest; optionally refresh named userland binaries.
Never run against a live disk. Arguments: REPO OUTPUT [--refresh appearance gterm].
"""
import argparse, hashlib, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('repo',type=Path);p.add_argument('out',type=Path)
p.add_argument('--refresh',nargs='*',choices=['appearance','gterm'],default=[])
a=p.parse_args();repo=a.repo.resolve();out=a.out.resolve()
with (out/'audit.log').open('a') as log:
 def run(args):subprocess.run(list(map(str,args)),check=True,stdout=log,stderr=log)
 run(['dd',f'if={out}/data.img',f'of={out}/home-audit.img','bs=1M','skip=1','count=1024','status=none'])
 for name in ['f5-document.txt','fonts.conf','f5-api.log','f5-reboot.log']:
  run(['debugfs','-R',f'dump /{name} {out/name}',out/'home-audit.img'])
 expected=(out/'document.txt').read_bytes().replace(b'\n',b'UNSAVED font switch proof\n\n',1)
 assert (out/'f5-document.txt').read_bytes()==expected
 for c in ['0','3','7','8','4']:
  assert f'F5 PASS: command {c}' in (out/'f5-api.log').read_text()
 assert 'FAIL' not in (out/'f5-api.log').read_text()
 if (out/'f5-reboot.log').exists():
  assert 'F5 PASS: command 5 serial 0 ui=16 terminal=24 document=28' in (out/'f5-reboot.log').read_text()
 run(['debugfs','-R',f'dump /fonts/SourceSans3-Regular.otf {out}/installed-font.otf',out/'home-audit.img'])
 assert (out/'installed-font.otf').read_bytes()==(repo/'userland/libfreetype/fixtures/SourceSans3-Regular.otf').read_bytes()
 run(['dd',f'if={out}/os64.img',f'of={out}/root-audit.img','bs=1M','skip=65','count=256','status=none'])
 with (out/'payload-updates-sha256.txt').open('a') as hashes:
  for name in a.refresh:
   source=repo/'userland/bin'/name;dest='/bin/'+name
   run(['mcopy','-o','-i',f'{out}/os64.img@@1048576',source,'::'+dest])
   run(['debugfs','-w','-R','rm '+dest,out/'root-audit.img'])
   run(['debugfs','-w','-R',f'write {source} {dest}',out/'root-audit.img'])
   run(['mcopy','-o','-i',f'{out}/os64.img@@1048576','::'+dest,out/'verify-update'])
   assert (out/'verify-update').read_bytes()==source.read_bytes()
   run(['debugfs','-R',f'dump {dest} {out}/verify-ext2-update',out/'root-audit.img'])
   assert (out/'verify-ext2-update').read_bytes()==source.read_bytes()
   hashes.write(hashlib.sha256(source.read_bytes()).hexdigest()+'  '+dest+'\n')
 if a.refresh:run(['dd',f'if={out}/root-audit.img',f'of={out}/os64.img','bs=1M','seek=65','conv=notrunc','status=none'])
 run(['dd',f'if={out}/os64.img',f'of={out}/fat-audit.img','bs=1M','skip=1','count=64','status=none'])
 for cmd in [['e2fsck','-fn',out/'home-audit.img'],['e2fsck','-fn',out/'root-audit.img']]:run(cmd)
 fat=subprocess.run(['fsck.fat','-n',str(out/'fat-audit.img')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/'fat-check.txt').write_text(fat.stdout)
 print('FAT check exit:',fat.returncode,flush=True)
 print(fat.stdout,end='')
 # Retain the complete diagnostic; a nonzero FAT check is not a clean result.
 if fat.returncode:print('FAT WARNING: inspect fat-check.txt and the baseline control receipt')
print('PASS: exact saved edit bytes; copied CFF bytes; API refusal/serial/Save checks; available reboot receipt; private ext2 checks clean (FAT status reported separately)')
