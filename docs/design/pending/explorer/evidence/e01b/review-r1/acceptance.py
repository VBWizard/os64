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
if mode=='hid':
    for _ in range(25):hmp('mouse_move -60 -60');time.sleep(.08)
    pos[:]=[0,0]
    key('ctrl-alt-f1');type_('/tests/actiongallery --trace > /home/e1b-final-hid.txt');key('ctrl-alt-f8');time.sleep(1)
    shot('gallery-final');key('f10');key('ret');shot('hid-actions-final');key('ret')
    key('alt-v');shot('checked-icon-final');key('ret');shot('hidden-final')
    key('f10');shot('f10-reveal-final');key('esc');key('alt');shot('alt-reveal-final');key('esc')
    move(900,700);click(True);shot('context-edge-final');key('ret');shot('restored-final')
    # Determine the Record A button from the actual owner rendering.
    im=Image.open(root/'restored-final.png');runs=[];start=None
    for y in range(10,650):
        r,g,b=im.getpixel((70,y))[:3];blue=b>r+35 and b>g+25
        if blue and start is None:start=y
        if not blue and start is not None:
            if y-start>10:runs.append((start,y))
            start=None
    print('blue runs',runs,flush=True);record_y=sum(runs[3])//2
    move(900,700);click(True);move(200,record_y);click();shot('dismiss-click-final');click();shot('later-click-final')
    key('alt-f');key('down');key('right');key('end');shot('tall-end-final');key('pgup');shot('tall-page-final');key('esc');key('esc')
    # Warm the same fonts/help, then exercise reused handles repeatedly.
    for i in range(12):key('alt-a');key('esc')
    pid=re.findall(r'task (\d+) \(actiongallery\) started',(root/'serial-acceptance.log').read_text())[-1]
    key('ctrl-alt-f2');type_('/bin/cat /proc/'+pid+'/heap > /home/e1b-heap-warm.txt');type_('sync');key('ctrl-alt-f8')
    for i in range(24):key('alt-a');key('esc')
    key('ctrl-alt-f2');type_('/bin/cat /proc/'+pid+'/heap > /home/e1b-heap-after.txt');type_('sync');key('ctrl-alt-f8')
    shot('after-cycles-final')
    key('alt-f');key('down');key('right');key('ctrl-alt-f1');time.sleep(.6);key('ctrl-alt-f8');shot('vt-preserved-final');key('esc');key('esc')
    key('ctrl-q');key('ctrl-alt-f1');type_('sync')
elif mode=='quit':cmd('quit')
elif mode=='ps2-font':
    cmd('device_del',{'id':'e1b_hidkbd'});time.sleep(2)
    key('ctrl-alt-f1');type_('/tests/actiongallery --trace --no-icons > /home/e1b-final-ps2.txt');key('ctrl-alt-f8');time.sleep(1)
    key('f10');key('ret');shot('ps2-actions-final');key('ret')
    key('alt-f');key('down');key('right');key('end');shot('ps2-tall-final');key('esc');key('esc')
    # Open a panel then publish a real Interface change from another VT.
    key('alt-a');key('ctrl-alt-f2');time.sleep(.7)
    type_('/tests/fontsettingstest 9 > /home/e1b-font32.txt');type_('sync');key('ctrl-alt-f8');shot('font-closed-final')
    key('alt-v');shot('font32-view-final');key('esc')
    # Color-only Apply does not change the font generation or reopen menus.
    key('alt-a');key('ctrl-alt-f2');type_('/tests/fontsettingstest 7 > /home/e1b-palette.txt');type_('sync');key('ctrl-alt-f8')
    shot('font32-palette-final');key('esc')
    key('ctrl-q');key('ctrl-alt-f1');type_('sync')
    type_('/tests/actiongallery --trace --narrow > /home/e1b-overflow.txt');key('ctrl-alt-f8')
    shot('overflow-owner-final');key('f10');key('right');key('ret');shot('overflow-menu-final');key('right');shot('overflow-child-final');assert (root/'overflow-child-final.png').read_bytes() != (root/'overflow-menu-final.png').read_bytes(), 'overflow child did not open';key('esc');key('esc');key('ctrl-q');key('ctrl-alt-f1');type_('sync')
    # Restore shared fonts for the next consumer check.
    key('ctrl-alt-f2');type_('/tests/fontsettingstest 6 > /home/e1b-font-reset.txt');type_('sync')
elif mode=='external':
    key('ctrl-alt-f2');type_('/bin/scribe &')
    key('ctrl-alt-f1');type_('/tests/actiongallery --trace > /home/e1b-external.txt');key('ctrl-alt-f8')
    for _ in range(25):hmp('mouse_move -60 -60');time.sleep(.08)
    pos[:]=[0,0];move(800,500)
    key('alt-f');key('down');key('right');key('end');shot('wheel-before-final')
    hmp('mouse_move 0 0 3');time.sleep(.7);shot('wheel-after-final')
    key('alt-tab');shot('external-focus-final')
    key('ctrl-g');shot('scribe-help-final');key('esc');key('alt-tab')
    key('alt-a');key('alt-f4');shot('popup-close-final')
    key('alt-f4');shot('owner-close-final')
    key('ctrl-q');key('ctrl-alt-f1');type_('sync')
