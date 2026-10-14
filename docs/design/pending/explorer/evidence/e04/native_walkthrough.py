import socket,json,time,sys
from pathlib import Path
from PIL import Image
root=Path('/tmp/roam-e4-native')
s=socket.socket(socket.AF_UNIX);s.connect(str(root/'qmp.sock'));f=s.makefile('rwb',buffering=0);f.readline()
def cmd(name,args=None):
 f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
 while True:
  r=json.loads(f.readline())
  if 'error' in r:raise RuntimeError(r)
  if 'return' in r:return r['return']
cmd('qmp_capabilities')
def hmp(c):return cmd('human-monitor-command',{'command-line':c})
def key(k,delay=.25):hmp('sendkey '+k);time.sleep(delay)
def type_(text):
 keys={' ':'spc','/':'slash','.':'dot','-':'minus','>':'shift-dot','_':'shift-minus','&':'shift-7',':':'shift-semicolon','=':'equal'}
 for c in text:hmp('sendkey '+keys.get(c,c));time.sleep(.065)
 key('ret');time.sleep(.7)
def shot(name):
 time.sleep(.3);p=root/(name+'.png');cmd('screendump',{'filename':str(p)+'.ppm'});Image.open(str(p)+'.ppm').save(p);print('shot',name,flush=True)
def reset_mouse():
 for _ in range(25):hmp('mouse_move -60 -60');time.sleep(.03)
def move(x,y):
 reset_mouse()
 while x or y:
  dx=max(-60,min(60,x));dy=max(-60,min(60,y));hmp('mouse_move %d %d'%(dx,dy));x-=dx;y-=dy;time.sleep(.05)
 time.sleep(.2)
def click(right=False):hmp('mouse_button '+('2' if right else '1'));time.sleep(.1);hmp('mouse_button 0');time.sleep(.3)
mode=sys.argv[1]
if mode=='install':
 key('ctrl-alt-f1');type_('rm /bin/roam');type_('os64get -a -n -f 10.0.2.2 > /home/roam-delivery-final.txt');time.sleep(20);shot('roam-installed')
elif mode=='launch':
 key('ctrl-alt-f1');type_('/bin/roam /roam-test > /home/roam-e4.txt');key('ctrl-alt-f8');time.sleep(8);shot('roam-initial')
elif mode=='navigate':
 key('ctrl-l');key('ctrl-a');type_(sys.argv[2]);time.sleep(2);shot(sys.argv[3])
elif mode=='shot':shot(sys.argv[2])
elif mode=='type':key('ctrl-alt-f1');type_(sys.argv[2]);shot('console-'+sys.argv[3])
elif mode=='type2':key('ctrl-alt-f2');type_(sys.argv[2]);shot('console-'+sys.argv[3])
elif mode=='keys':
 for k in sys.argv[2:]:key(k)
elif mode=='click':
 move(int(sys.argv[2]),int(sys.argv[3]));click(len(sys.argv)>4)
elif mode=='walk':
 key('home');key('down');key('shift-down');shot('roam-selection')
 key('ctrl-l');key('ctrl-a');type_('/roam-test/empty');time.sleep(2)
 key('ctrl-bracket_left');time.sleep(2);shot('roam-restored-selection')
 key('ctrl-f');type_('notes');shot('roam-filtered')
 key('esc');key('ctrl-h');shot('roam-hidden-files')
 key('ctrl-l');key('ctrl-a');type_('/does-not-exist');time.sleep(2);shot('roam-failed-navigation')
 key('ctrl-l');key('ctrl-a');type_('/fat/roam-test');time.sleep(2);shot('roam-fat')
 key('ctrl-l');key('ctrl-a');type_('/roam-test/large');time.sleep(6);shot('roam-large')
elif mode=='history-scroll':
 key('end');shot('roam-large-end')
 key('ctrl-l');key('ctrl-a');type_('/roam-test/empty');time.sleep(2)
 key('ctrl-bracket_left');time.sleep(5);shot('roam-restored-scroll')
 key('f5');time.sleep(5);shot('roam-refreshed-scroll')
elif mode=='file':
 key('ctrl-l');key('ctrl-a');type_('/fat/roam-test');time.sleep(2)
 key('home');key('down');key('down');key('ret');shot('roam-file-message')
elif mode=='panes':
 for k in ('t','l','b','f','p','s','m'):
  key('alt-v');key(k)
 shot('roam-minimal');key('alt');shot('roam-menu-revealed');key('esc')
 move(400,400);click(True);shot('roam-context-recovery');key('m');shot('roam-menu-restored')
elif mode=='restore-panes':
 for k in ('t','l','b','f','p','s'):
  key('alt-v');key(k)
elif mode=='font':
 key('ctrl-alt-f2');type_('/tests/fontsettingstest 9 > /home/roam-font32.txt');key('ctrl-alt-f8');time.sleep(2);shot('roam-font32')
elif mode=='reset-font':
 key('ctrl-alt-f2');type_('/tests/fontsettingstest 6 > /home/roam-font-reset.txt');key('ctrl-alt-f8');time.sleep(2)
elif mode=='quit-vm':cmd('quit')
elif mode=='close-loading':
 key('ctrl-l');key('ctrl-a');type_('/roam-test/large');key('ctrl-q');time.sleep(2)
 key('ctrl-alt-f1');type_('sync');shot('roam-close-loading')
elif mode=='close':
 key('ctrl-q');key('ctrl-alt-f1');type_('sync');shot('roam-exit-console')
else:raise ValueError(mode)
