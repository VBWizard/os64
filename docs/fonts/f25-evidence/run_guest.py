import argparse,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('repo',type=Path);p.add_argument('images',type=Path);p.add_argument('name');p.add_argument('--gui',action='store_true');p.add_argument('--bench',action='store_true');a=p.parse_args()
out=a.images/a.name;out.mkdir(exist_ok=True)
for name in ['os64.img','data.img']:subprocess.run(['cp','--reflink=auto','--sparse=always',str(a.images/name),str(out/name)],check=True)
serial=out/'serial.log';monitor=(out/'monitor.log').open('w')
cmd=['qemu-system-x86_64','-machine','q35','-m','8G','-smp','8','-cpu','qemu64,+rdrand,+rdseed','-cdrom',str(a.repo/'os64_kernel.iso'),'-boot','d','-drive',f'file={out}/os64.img,if=none,id=bootdisk,format=raw','-device','nvme,drive=bootdisk,serial=F2BOOT','-drive',f'file={out}/data.img,if=none,id=datadisk,format=raw','-device','nvme,drive=datadisk,serial=F2DATA','-display','none','-serial',f'file:{serial}','-monitor','stdio','-no-reboot']
q=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=monitor,stderr=monitor,text=True)
def send(command):q.stdin.write(command+'\n');q.stdin.flush()
def enter(command):
 keys={' ':'spc','/':'slash','-':'minus','.':'dot','>':'shift-dot'}
 for c in command:send('sendkey '+keys.get(c,c))
 send('sendkey ret');time.sleep(1)
def wait_for(token,timeout=60):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  if serial.exists() and token in serial.read_text(errors='replace'):return
  if q.poll() is not None:raise RuntimeError('QEMU exited')
  time.sleep(.5)
 raise RuntimeError('timeout: '+token)
def snap(name):
 send('screendump '+str(out/(name+'.ppm')));time.sleep(.5)
 from PIL import Image
 Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
try:
 time.sleep(2)
 if a.gui:send('sendkey down');send('sendkey ret')
 wait_for("created window 1 'desktop'" if a.gui else '[pool] boot complete')
 time.sleep(1)
 if a.gui:send('sendkey ctrl-alt-f1');time.sleep(.4)
 if a.bench:
  enter('textspawn > /home/f2-spawn.log');time.sleep(35)
  enter('cat /home/f2-spawn.log');snap('spawn')
 enter('fontsettest > /home/f25-fontset.log')
 if a.gui:
  wait_for("'F2 text runs' at")
  send('sendkey alt-f8');time.sleep(3);snap('specimen')
  wait_for('(texttest) exited code 0')
  send('sendkey ctrl-alt-f1');time.sleep(.5)
 else:time.sleep(4)
 enter('cat /home/f25-fontset.log');snap('pass');enter('shutdown');q.wait(timeout=30)
 print('Guest completed:',out,flush=True)
finally:
 if q.poll() is None:
  q.terminate()
  try:q.wait(timeout=5)
  except subprocess.TimeoutExpired:q.kill();q.wait()
 monitor.close()
