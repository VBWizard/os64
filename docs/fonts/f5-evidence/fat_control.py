import subprocess,time,argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('base',type=Path);p.add_argument('out',type=Path);a=p.parse_args()
base=a.base.resolve();out=a.out.resolve();out.mkdir(parents=True)
for name in ['os64.img','os64_data.img']:subprocess.run(['cp','--reflink=auto','--sparse=always',str(base/'disk'/name),str(out/name)],check=True)
with (out/'monitor.log').open('w') as log:
 q=subprocess.Popen(['qemu-system-x86_64','-machine','q35','-m','8G','-smp','8','-cpu','qemu64,+rdrand,+rdseed','-cdrom',str(base/'os64_kernel.iso'),'-boot','d','-drive',f'file={out}/os64.img,if=none,id=b,format=raw','-device','nvme,drive=b,serial=CONTROL','-drive',f'file={out}/os64_data.img,if=none,id=d,format=raw','-device','nvme,drive=d,serial=DATA','-display','none','-serial',f'file:{out}/serial.log','-monitor','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=log,stderr=log,text=True)
 def key(k):q.stdin.write('sendkey '+k+'\n');q.stdin.flush();time.sleep(.2)
 try:
  time.sleep(2);key('down');key('ret')
  end=time.monotonic()+60
  while time.monotonic()<end:
   if "created window 1 'desktop'" in (out/'serial.log').read_text(errors='replace'):break
   time.sleep(.3)
  else:raise RuntimeError('boot timeout')
  time.sleep(25);key('ctrl-alt-f2')
  for c in 'shutdown':key(c)
  key('ret');q.wait(timeout=40)
 finally:
  if q.poll() is None:q.terminate();q.wait()
subprocess.run(['dd',f'if={out}/os64.img',f'of={out}/fat.img','bs=1M','skip=1','count=64','status=none'],check=True)
r=subprocess.run(['fsck.fat','-n',str(out/'fat.img')],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(out/'fsck.txt').write_text(r.stdout);print('Baseline-only boot fsck status',r.returncode);print(r.stdout)
