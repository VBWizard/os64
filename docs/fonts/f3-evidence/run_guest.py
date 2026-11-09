#!/usr/bin/env python3
"""Reproduce F3 on private copies of the F2/F2.5 test disks and accepted ISO.
Usage: python3 run_guest.py REPO BASE_IMAGES NEW_OUTPUT_DIR
BASE_IMAGES contains os64.img and data.img; they are never booted or modified.
Requires QEMU, mtools, e2fsprogs and Pillow. See ../F3-REPORT.md for disk layout.
"""
import argparse, runpy, subprocess, time
from pathlib import Path
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('repo',type=Path);p.add_argument('images',type=Path);p.add_argument('out',type=Path);a=p.parse_args()
a.repo=a.repo.resolve();a.images=a.images.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True)
out=a.out
install=(out/'install.log').open('w')
def run(args):subprocess.run([str(x) for x in args],check=True,stdout=install,stderr=install)
for name in ['os64.img','data.img']:run(['cp','--reflink=auto','--sparse=always',a.images/name,out/name])
runpy.run_path(str(a.repo/'tools/fonts/make_terminal_fixture.py'))['make_fixture'](a.repo/'userland/libfreetype/fixtures/DejaVuSansMono.ttf',out/'NoBoxes.ttf')
run(['dd',f'if={out}/os64.img',f'of={out}/root.img','bs=1M','skip=65','count=256','status=none'])
files=[(a.repo/'userland/bin/libos64.so','/lib/libos64.so'),(a.repo/'userland/bin/libfreetype.so','/lib/libfreetype.so'),(a.repo/'userland/bin/gterm','/bin/gterm'),(a.repo/'userland/bin/tests/gtermfonttest','/tests/gtermfonttest'),(out/'NoBoxes.ttf','/tests/fonts/NoBoxes.ttf'),(a.repo/'userland/libfreetype/fixtures/DejaVuSans.ttf','/tests/fonts/DejaVuSans.ttf')]
import hashlib
with (out/'payload-sha256.txt').open('w') as hashes:
 for source,dest in files:
  run(['mcopy','-o','-i',f'{out}/os64.img@@1048576',source,'::'+dest])
  run(['debugfs','-w','-R','rm '+dest,out/'root.img'])
  run(['debugfs','-w','-R',f'write {source} {dest}',out/'root.img'])
  verify=out/('verify-'+source.name)
  run(['mcopy','-o','-i',f'{out}/os64.img@@1048576','::'+dest,verify])
  assert verify.read_bytes()==source.read_bytes()
  hashes.write(hashlib.sha256(source.read_bytes()).hexdigest()+'  '+dest+'\n')
run(['dd',f'if={out}/root.img',f'of={out}/os64.img','bs=1M','seek=65','conv=notrunc','status=none'])
install.close()
serial=out/'serial.log';monitor=(out/'monitor.log').open('w');actions=(out/'actions.log').open('w')
q=subprocess.Popen(['qemu-system-x86_64','-machine','q35','-m','8G','-smp','8','-cpu','qemu64,+rdrand,+rdseed','-cdrom',str(a.repo/'os64_kernel.iso'),'-boot','d','-drive',f'file={out}/os64.img,if=none,id=bootdisk,format=raw','-device','nvme,drive=bootdisk,serial=F3BOOT','-drive',f'file={out}/data.img,if=none,id=datadisk,format=raw','-device','nvme,drive=datadisk,serial=F3DATA','-display','none','-serial',f'file:{serial}','-monitor','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=monitor,stderr=monitor,text=True)
def send(cmd):
 actions.write(cmd+'\n');actions.flush();q.stdin.write(cmd+'\n');q.stdin.flush()
def read():return serial.read_text(errors='replace') if serial.exists() else ''
def wait(token,offset=0):
 until=time.monotonic()+50
 while time.monotonic()<until:
  if token in read()[offset:]:return
  if q.poll() is not None:raise RuntimeError('QEMU exited before '+token)
  time.sleep(.25)
 raise RuntimeError('timeout: '+token)
def key(k,expect=None):
 offset=len(read());send('sendkey '+k);time.sleep(.2)
 if expect:wait(expect,offset)
 time.sleep(.5)
def enter(cmd):
 keys={' ':'spc','/':'slash','-':'minus','.':'dot','>':'shift-dot'}
 for c in cmd:send('sendkey '+keys.get(c,c));time.sleep(.14)
 key('ret');time.sleep(1)
def snap(name):
 time.sleep(.5);send('screendump '+str(out/(name+'.ppm')));time.sleep(.5)
 Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
def mouse(cmd):send(cmd);time.sleep(.15)
def move_to(x,y):
 # PS/2 deltas must stay within the signed-byte range.
 for _ in range(11):mouse('mouse_move -100 -100')
 while x or y:
  dx=min(x,100);dy=min(y,100);mouse(f'mouse_move {dx} {dy}');x-=dx;y-=dy
copy='F3 PASS: pointer selection copied exact bytes'
def select(delta,name):
 move_to(143,145);mouse('mouse_button 1');mouse(f'mouse_move {delta} 0');snap(name);mouse('mouse_button 0');key('6',copy)
try:
 time.sleep(2);key('down');key('ret');wait("created window 1 'desktop'")
 key('ctrl-alt-f1');enter('gtermfonttest > /home/f3-terminal.log');wait('F3 child startup 100x38')
 key('alt-f8');snap('builtin')
 key('1','F3 child SIGWINCH 114x40');snap('font12');select(42,'selected12')
 key('2','F3 child SIGWINCH 47x18');snap('font28');select(109,'selected28')
 key('3','F3 PASS: real PTY refusal preserved font/grid/selection');key('6',copy)
 key('4','F3 PASS: proportional terminal rejected');snap('refused28')
 key('ctrl-alt-m','F3 child SIGWINCH 60x22');snap('maximized28')
 key('ctrl-alt-m','F3 child SIGWINCH 47x18');snap('restored28')
 key('1','F3 child SIGWINCH 114x40')
 move_to(143,145);mouse('mouse_button 1');mouse('mouse_move 42 0')
 key('2','F3 child SIGWINCH 47x18');mouse('mouse_button 0');snap('switched-drag')
 key('6',copy) # cancelled drag must not publish a new clipboard value
 key('q');key('ret');wait('F3 PASS: fixture finished')
 key('ctrl-alt-f1');enter('cat /home/f3-terminal.log');snap('report')
 enter('gterm');wait("'husk' at");key('alt-f8');snap('production-builtin')
 enter('echo alive');snap('production-typing');enter('exit')
 key('ctrl-alt-f1');enter('shutdown');q.wait(timeout=30)
 assert 'F3 FAIL:' not in read()
 print('F3 guest PASS:',out,flush=True)
finally:
 if q.poll() is None:
  q.terminate()
  try:q.wait(timeout=5)
  except subprocess.TimeoutExpired:q.kill();q.wait()
 monitor.close();actions.close()
