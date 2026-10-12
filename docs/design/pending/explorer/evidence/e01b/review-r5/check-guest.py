"""Check archived final native traces, heap and no-op wheel pixels."""
from pathlib import Path
import re
from PIL import Image
root=Path(__file__).resolve().parent
for path in sorted(root.glob('guest-*.txt')):
    trace=path.read_text()
    calls=re.findall(r'gallery invoke=(\d+) menus=(\d+) target=(\d+)',trace)
    assert calls or path.name=='guest-external.txt',path
    assert all(active=='0' for _,active,_ in calls),path
    assert 'gallery exit font-bytes=0 refs=0,0' in trace,path
    surfaces=re.findall(r'surface handle=(\d+) width=(\d+) height=\d+ pitch=(\d+)',trace)
    assert surfaces,path
    # The initial surface is the ordinary owner, which keeps padded capacity.
    owner=surfaces[0][0]
    popups=[(width,pitch) for handle,width,pitch in surfaces if handle!=owner]
    assert popups and all(width==pitch for width,pitch in popups),path
    if 'scroll' in path.name:
        assert [int(command) for command,_,_ in calls]==[118,7],path
    print(f'{path.name}: {len(calls)} callbacks closed; zero font/target ownership; {len(popups)} popup observations tightly pitched')
def heap(name):
    return dict(line.split('\t') for line in (root/name).read_text().splitlines() if '\t' in line)
a,b=heap('heap-warm.txt'),heap('heap-after.txt')
for field in ('mapped','live','blocks_live','high_water','audit'):
    assert a[field]==b[field],(field,a[field],b[field])
assert a['audit']=='ok'
print('heap: mapped/live/blocks/high-water/audit return to warm baseline')
for keyboard in ('hid','ps2'):
    a=Image.open(root/f'r5-{keyboard}-short-parent-before-wheel.png').crop((150,32,1024,400))
    b=Image.open(root/f'r5-{keyboard}-short-parent-after-wheel.png').crop((150,32,1024,400))
    assert a.tobytes()==b.tobytes(),keyboard
    print(f'{keyboard}: parent and child pixels preserved after no-op wheel')
