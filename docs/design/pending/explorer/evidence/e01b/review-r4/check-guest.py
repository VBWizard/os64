"""Check archived final native traces, heap and first-F10 recovery pixels."""
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
    if 'held' in path.name:
        assert [int(command) for command,_,_ in calls]==[1,1,7],path
        assert 'hits=2,0' in trace,path
        scan=67 if 'hid' in path.name else 68
        down=trace.index(f'edge=1 ascii=0 scan={scan}')
        outside=trace.index('gained=0 other=0',down)
        assert f'edge=2 ascii=0 scan={scan}' not in trace[down:outside],path
    print(f'{path.name}: {len(calls)} callbacks closed; zero font/target ownership; {len(popups)} popup observations tightly pitched')
def heap(name):
    return dict(line.split('\t') for line in (root/name).read_text().splitlines() if '\t' in line)
a,b=heap('heap-warm.txt'),heap('heap-after.txt')
for field in ('mapped','live','blocks_live','high_water','audit'):
    assert a[field]==b[field],(field,a[field],b[field])
assert a['audit']=='ok'
print('heap: mapped/live/blocks/high-water/audit return to warm baseline')
for keyboard in ('hid','ps2'):
    a=Image.open(root/f'r4-{keyboard}-f10-held.png').crop((0,32,1024,80))
    b=Image.open(root/f'r4-{keyboard}-f10-recovered.png').crop((0,32,1024,80))
    assert a.tobytes()==b.tobytes(),keyboard
    print(f'{keyboard}: first fresh F10 returns to matching highlighted bar pixels')
