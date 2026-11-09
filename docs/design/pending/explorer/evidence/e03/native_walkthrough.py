import socket,json,time,sys
from pathlib import Path
from PIL import Image
root=Path('/tmp/e3-native')
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
if mode=='launch':
 key('ctrl-alt-f1');type_('/tests/actiongallery --details-page > /home/e3-native.txt');key('ctrl-alt-f8');shot('details-initial')
elif mode in ('empty','one','max','tenk'):
 key('alt-d');key('home')
 for _ in range({'empty':0,'one':1,'tenk':2,'max':3}[mode]):key('down')
 key('ret');time.sleep(3);shot('details-'+mode)
elif mode=='sort':
 move(345,102);click();shot('details-size-ascending');click();shot('details-size-descending')
elif mode=='columns':
 move(291,102);hmp('mouse_button 1');time.sleep(.1);move(375,102);hmp('mouse_button 0');time.sleep(.5);shot('details-column-resized')
 move(425,102);click(True);shot('details-header-context');key('esc')
 key('alt-v');key('c');key('down');key('down');key('ret');shot('details-type-hidden')
 key('alt-v');key('c');key('ctrl-a');key('spc');shot('details-popup-consumes-keys');key('esc');key('esc')
elif mode=='resize':
 move(950,650)
 cmd('input-send-event',{'events':[{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'ctrl'}}},{'type':'key','data':{'down':True,'key':{'type':'qcode','data':'alt'}}}]})
 hmp('mouse_button 2');time.sleep(.15);move(600,380);hmp('mouse_button 0')
 cmd('input-send-event',{'events':[{'type':'key','data':{'down':False,'key':{'type':'qcode','data':'ctrl'}}},{'type':'key','data':{'down':False,'key':{'type':'qcode','data':'alt'}}}]})
 time.sleep(.6);shot('details-small-window')
 key('home');key('end');shot('details-small-last-row')
elif mode=='selection':
 key('home');key('down');key('ctrl-down');key('ctrl-spc');key('shift-down');key('ctrl-shift-down');shot('details-selection')
 key('ctrl-a');key('end');key('ret');shot('details-end-activation')
 key('home');key('a');key('a');key('l');shot('details-typeahead')
elif mode=='filter':
 key('alt-d');key('f');shot('details-filtered');key('alt-d');key('f');shot('details-filter-cleared')
 key('alt-d');key('r');shot('details-refreshed');key('alt-d');key('n');shot('details-domain-reset')
elif mode=='second':
 key('alt-v');key('s');shot('details-second-view');key('tab');key('end');shot('details-second-independent');key('alt-v');key('s');shot('details-second-closed')
elif mode=='font':
 key('ctrl-alt-f2');type_('/tests/fontsettingstest 9 > /home/e3-font32.txt');type_('sync');key('ctrl-alt-f8');shot('details-font32')
 key('down');key('shift-down');key('ctrl-alt-f2');type_('/tests/fontsettingstest 7 > /home/e3-palette.txt');type_('sync');key('ctrl-alt-f8');shot('details-appearance')
elif mode=='report':key('alt-v');key('d');shot('details-diagnostics')
elif mode=='close':
 key('ctrl-q');key('ctrl-alt-f1');type_('sync');shot('details-exit-console')
elif mode=='reset-font':
 key('ctrl-alt-f2');type_('/tests/fontsettingstest 6 > /home/e3-font-reset.txt');type_('sync');key('ctrl-alt-f8')
elif mode=='benchmark':
 key('home')
 for i in range(110):key('down',.12)
 key('alt-v');key('d');shot('details-benchmark')
elif mode=='launch-dark':
 key('ctrl-alt-f1');type_('/tests/actiongallery --details-page --dark > /home/e3-native-dark.txt');key('ctrl-alt-f8');shot('details-dark')
elif mode=='legacy':
 key('ctrl-alt-f1');type_('/tests/actiongallery > /home/e3-legacy.txt');key('ctrl-alt-f8');key('alt-a');shot('legacy-menu');key('esc');key('ctrl-q');key('ctrl-alt-f1');type_('sync')
elif mode=='icons':
 key('ctrl-alt-f1');type_('/tests/actiongallery --icons-page > /home/e3-icons-page.txt');key('ctrl-alt-f8');shot('legacy-icons');key('alt-f4');key('ctrl-alt-f1');type_('sync')
elif mode=='shot':shot(sys.argv[2])
elif mode=='type':key('ctrl-alt-f1');type_(sys.argv[2]);shot('console-'+sys.argv[3])
elif mode=='keys':
 for k in sys.argv[2:]:key(k)
else:raise ValueError(mode)
