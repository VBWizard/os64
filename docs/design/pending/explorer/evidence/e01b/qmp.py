import socket,json,sys,time
from pathlib import Path
s=socket.socket(socket.AF_UNIX);s.connect('/tmp/e1b-vm/qmp.sock');f=s.makefile('rwb',buffering=0)
def recv():
    while True:
        v=json.loads(f.readline())
        if 'return' in v or 'error' in v:return v
def cmd(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode());return recv()
f.readline();cmd('qmp_capabilities')
kind=sys.argv[1]
if kind=='quit':
    print(cmd('quit'));sys.exit(0)
if kind=='hmp': print(cmd('human-monitor-command',{'command-line':' '.join(sys.argv[2:])}))
elif kind=='shot':
    time.sleep(.5);path=sys.argv[2];print(cmd('screendump',{'filename':path+'.ppm'}))
    from PIL import Image
    Image.open(path+'.ppm').save(path)
elif kind=='key':
    print(cmd('human-monitor-command',{'command-line':'sendkey '+sys.argv[2]}));time.sleep(.5)
elif kind=='type':
    keys={'&':'shift-7',' ':'spc','/':'slash','.':'dot','-':'minus','=':'equal','>':'shift-dot','_':'shift-minus','|':'shift-backslash',':':'shift-semicolon','$':'shift-4','?':'shift-slash'}
    for c in sys.argv[2]:
        key=keys.get(c,c if c.islower() or c.isdigit() else 'shift-'+c.lower())
        cmd('human-monitor-command',{'command-line':'sendkey '+key});time.sleep(.10)
    cmd('human-monitor-command',{'command-line':'sendkey ret'});time.sleep(1)

elif kind=='hold':
    key=sys.argv[2]
    def key_event(k,down):
        return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':k}}}
    print(cmd('input-send-event',{'events':[key_event('ctrl',True),key_event(key,True)]}))
    for _ in range(12):
        print(cmd('input-send-event',{'events':[key_event(key,True)]}));time.sleep(.04)
    print(cmd('input-send-event',{'events':[key_event(key,False),key_event('ctrl',False)]}))
elif kind=='cycle':
    for _ in range(int(sys.argv[2])):
        for key in ['ctrl-p','ctrl-p','ctrl-b','ctrl-b','ctrl-h']:
            cmd('human-monitor-command',{'command-line':'sendkey '+key});time.sleep(.16)
    time.sleep(.5)

elif kind=='click':
    cmd('human-monitor-command',{'command-line':'mouse_button 1'});time.sleep(.12)
    print(cmd('human-monitor-command',{'command-line':'mouse_button 0'}));time.sleep(.3)
elif kind=='keys':
    for key in sys.argv[2:]:
        print(cmd('human-monitor-command',{'command-line':'sendkey '+key}));time.sleep(.2)
