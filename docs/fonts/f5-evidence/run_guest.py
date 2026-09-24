#!/usr/bin/env python3
"""Private-image F5 guest harness. Append JSON action arrays to commands.jsonl.
Example: [["enter", "appearance"], ["key", "alt-f8"], ["snap", "fonts"]]
Uses the accepted F4 ISO/kernel; refreshes both roots before the first boot.
"""
import argparse, hashlib, json, subprocess, time
from pathlib import Path
from PIL import Image
p = argparse.ArgumentParser()
p.add_argument('repo', type=Path); p.add_argument('base', type=Path); p.add_argument('out', type=Path)
p.add_argument('--resume', action='store_true')
p.add_argument('--label', default='reboot', help='Receipt suffix for an additional cold boot')
a=p.parse_args(); repo=a.repo.resolve(); base=a.base.resolve(); out=a.out.resolve(); out.mkdir(parents=True,exist_ok=a.resume)
log=(out/'install.log').open('a')
def run(args): subprocess.run(list(map(str,args)),check=True,stdout=log,stderr=log)
if not a.resume:
    for src,dst in [('os64.img','os64.img'),('os64_data.img','data.img')]:
        run(['cp','--reflink=auto','--sparse=always',base/'disk'/src,out/dst])
    run(['dd',f'if={out}/os64.img',f'of={out}/root.img','bs=1M','skip=65','count=256','status=none'])
    files=[]
    for f in sorted((repo/'userland/bin').iterdir()):
        if f.is_file() and f.suffix not in ['.gdb']:
            files.append((f,('/lib/' if f.suffix == '.so' else '/bin/')+f.name))
    files.append((repo/'userland/bin/tests/fontsettingstest','/tests/fontsettingstest'))
    files.append((repo/'etc/fonts.conf','/etc/fonts.conf'))
    for name in ['DejaVuSans.ttf','DejaVuSansMono.ttf']:
        files.append((repo/'userland/libfreetype/fixtures'/name,'/etc/fonts/'+name))
    files.append((repo/'userland/libfreetype/fixtures/LICENSE-DejaVu.txt','/etc/licenses/DejaVu.txt'))
    run(['debugfs','-w','-R','mkdir /etc/fonts',out/'root.img'])
    subprocess.run(['mmd','-i',f'{out}/os64.img@@1048576','::/etc/fonts'],stdout=log,stderr=log)
    with (out/'payload-sha256.txt').open('w') as hashes:
        for source,dest in files:
            run(['mcopy','-o','-i',f'{out}/os64.img@@1048576',source,'::'+dest])
            run(['debugfs','-w','-R','rm '+dest,out/'root.img'])
            run(['debugfs','-w','-R',f'write {source} {dest}',out/'root.img'])
            check=out/'verify.bin'
            run(['debugfs','-R',f'dump {dest} {check}',out/'root.img'])
            assert check.read_bytes()==source.read_bytes(),dest
            hashes.write(hashlib.sha256(source.read_bytes()).hexdigest()+'  '+dest+'\n')
    run(['dd',f'if={out}/root.img',f'of={out}/os64.img','bs=1M','seek=65','conv=notrunc','status=none'])
    run(['dd',f'if={out}/data.img',f'of={out}/home.img','bs=1M','skip=1','count=1024','status=none'])
    for path in ['/fonts.conf','/fonts/SourceSans3-Regular.otf']:
        run(['debugfs','-w','-R','rm '+path,out/'home.img'])
    document=out/'document.txt'
    document.write_text('F5 live document: café, naïve, résumé.\n'+('A wider line with editable words and symbols: α β → € — '*5)+'\nLast line.\n')
    run(['debugfs','-w','-R',f'write {document} /f5-document.txt',out/'home.img'])
    run(['dd',f'if={out}/home.img',f'of={out}/data.img','bs=1M','seek=1','conv=notrunc','status=none'])
log.close()
suffix='-'+a.label if a.resume else ''
serial=out/('serial'+suffix+'.log'); monitor=(out/('monitor'+suffix+'.log')).open('w'); actions=(out/('actions'+suffix+'.log')).open('w')
q=subprocess.Popen(['qemu-system-x86_64','-machine','q35','-m','8G','-smp','8','-cpu','qemu64,+rdrand,+rdseed','-cdrom',str(base/'os64_kernel.iso'),'-boot','d','-drive',f'file={out}/os64.img,if=none,id=bootdisk,format=raw','-device','nvme,drive=bootdisk,serial=F5BOOT','-drive',f'file={out}/data.img,if=none,id=datadisk,format=raw','-device','nvme,drive=datadisk,serial=F5DATA','-display','none','-serial',f'file:{serial}','-monitor','stdio','-no-reboot'],stdin=subprocess.PIPE,stdout=monitor,stderr=monitor,text=True)
def send(s):
    actions.write(s+'\n');actions.flush();q.stdin.write(s+'\n');q.stdin.flush()
def key(s): send('sendkey '+s);time.sleep(.18)
def enter(s):
    table={' ':'spc','/':'slash','-':'minus','.':'dot','>':'shift-dot','_':'shift-minus',':':'shift-semicolon',';':'semicolon','=':'equal','&':'shift-7','!':'shift-1','"':'shift-apostrophe'}
    for c in s:
        key(table.get(c,'shift-'+c.lower() if c.isupper() else c))
    key('ret');time.sleep(1)
def move(x,y):
    for _ in range(12): send('mouse_move -100 -100');time.sleep(.02)
    while x or y:
        dx=min(x,100);dy=min(y,100);send(f'mouse_move {dx} {dy}');x-=dx;y-=dy;time.sleep(.03)
def snap(name):
    time.sleep(.5);send('screendump '+str(out/(name+'.ppm')));time.sleep(.5)
    Image.open(out/(name+'.ppm')).save(out/(name+'.png'))
try:
    time.sleep(2);key('down');key('ret')
    end=time.monotonic()+55
    while time.monotonic()<end:
        if serial.exists() and "created window 1 'desktop'" in serial.read_text(errors='replace'):break
        time.sleep(.3)
    else:raise RuntimeError('Desktop did not boot')
    commands=out/('commands'+suffix+'.jsonl');commands.touch();pos=0
    print('READY',commands,flush=True)
    while q.poll() is None:
        with commands.open() as f:
            f.seek(pos);line=f.readline();next_pos=f.tell()
        if not line.endswith('\n'):time.sleep(.1);continue
        pos=next_pos
        for action in json.loads(line):
            op,*args=action
            if op=='enter':enter(*args)
            elif op=='key':key(*args)
            elif op=='snap':snap(*args)
            elif op=='move':move(*args)
            elif op=='click':
                move(*args);send('mouse_button 1');time.sleep(.1);send('mouse_button 0');time.sleep(.3)
            elif op=='wait':time.sleep(min(float(args[0]),30))
            elif op=='hmp':send(*args)
            else:raise ValueError(op)
        print('DONE',line.strip(),flush=True)
    print('QEMU exit',q.returncode,flush=True)
finally:
    if q.poll() is None:q.terminate();q.wait(timeout=10)
    monitor.close();actions.close()
