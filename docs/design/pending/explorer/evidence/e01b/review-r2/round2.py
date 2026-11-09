import socket,json,time,sys,re
from pathlib import Path
from PIL import Image
root=Path('/tmp/e1b-vm')
s=socket.socket(socket.AF_UNIX);s.connect(str(root/'qmp.sock'));f=s.makefile('rwb',buffering=0)
f.readline()
def cmd(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
        r=json.loads(f.readline())
        if 'error' in r:raise RuntimeError(r)
        if 'return' in r:return r['return']
cmd('qmp_capabilities')
def hmp(c):return cmd('human-monitor-command',{'command-line':c})
def key(k):hmp('sendkey '+k);time.sleep(.5)
def type_(text):
    keys={' ':'spc','/':'slash','.':'dot','-':'minus','>':'shift-dot','_':'shift-minus','&':'shift-7'}
    for c in text:hmp('sendkey '+keys.get(c,c));time.sleep(.1)
    key('ret');time.sleep(1)
def shot(name):
    time.sleep(.3);p=root/(name+'.png');cmd('screendump',{'filename':str(p)+'.ppm'});Image.open(str(p)+'.ppm').save(p)
    print('shot',name,flush=True)
pos=[512,384]
def move(x,y):
    dx,dy=x-pos[0],y-pos[1]
    while dx or dy:
        sx=max(-60,min(60,dx));sy=max(-60,min(60,dy));hmp('mouse_move %d %d'%(sx,sy));dx-=sx;dy-=sy;time.sleep(.08)
    pos[:]=[x,y];time.sleep(.3)
def click(right=False):
    hmp('mouse_button '+('2' if right else '1'));time.sleep(.12);hmp('mouse_button 0');time.sleep(.5)
mode=sys.argv[1]
if mode in ('r2-hid','r2-ps2'):
    suffix=mode
    key('ctrl-alt-f2');type_('/tests/fontsettingstest 9');type_('sync')
    key('ctrl-alt-f1');type_('/tests/actiongallery --trace --narrow > /home/e1b-'+suffix+'.txt');key('ctrl-alt-f8')
    for _ in range(25):hmp('mouse_move -60 -60');time.sleep(.08)
    pos[:]=[0,0]
    key('alt-f');shot(suffix+'-hidden-mnemonic')
    move(200,5);move(200,60);shot(suffix+'-overflow-hover')
    before=Image.open(root/(suffix+'-hidden-mnemonic.png')).crop((0,90,1024,768))
    after=Image.open(root/(suffix+'-overflow-hover.png')).crop((0,90,1024,768))
    assert before.tobytes()==after.tobytes(), 'overflow hover replaced the direct mnemonic menu'
    key('left');shot(suffix+'-left-actions');key('esc')
    key('alt-l');move(250,70);hmp('mouse_move 0 0 -3');time.sleep(.7);shot(suffix+'-wheel-no-selection')
    key('ret');shot(suffix+'-enter-no-invocation')
    key('down');shot(suffix+'-visible-selection');key('ret');shot(suffix+'-visible-invocation')
    key('ctrl-q');key('ctrl-alt-f1');type_('sync')
    key('ctrl-alt-f2');type_('/tests/fontsettingstest 6');type_('sync')
