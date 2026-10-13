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

def held(k,down):
    cmd('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':k}}}]})
    time.sleep(.5)
key('ctrl-alt-f2');type_('/bin/scribe &')
key('ctrl-alt-f1');type_('/tests/actiongallery --trace > /home/e1b-r4-held-'+sys.argv[1]+'.txt');key('ctrl-alt-f8')
# A mode down without its release at the owner strands F10 unless external
# loss resets guards. Release it at Scribe, return, and use the first new press.
held('f10',True);shot('r4-'+sys.argv[1]+'-f10-held')
key('alt-tab');held('f10',False);shot('r4-'+sys.argv[1]+'-outside')
key('alt-tab');key('f10');shot('r4-'+sys.argv[1]+'-f10-recovered')
a=Image.open(root/('r4-'+sys.argv[1]+'-f10-held.png')).crop((0,32,1024,80))
b=Image.open(root/('r4-'+sys.argv[1]+'-f10-recovered.png')).crop((0,32,1024,80))
assert a.tobytes()==b.tobytes(), 'first fresh F10 did not restore bar mode'
key('ret');held('ctrl',True);held('d',True);held('ctrl',False)
shot('r4-'+sys.argv[1]+'-shortcut-closed')
key('alt-tab');held('d',False);key('alt-tab')
key('alt-a');key('ctrl-d');shot('r4-'+sys.argv[1]+'-shortcut-recovered')
key('ctrl-q');key('ctrl-alt-f1');type_('sync')
key('ctrl-alt-f8');key('ctrl-q');key('ctrl-alt-f1');type_('sync')
