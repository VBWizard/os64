#!/usr/bin/env python3
"""grid_probe.py DRIVER [NAME...] -- libflow's grid against Chrome's.

Each case is one page: a grid and its items, the items' content of fixed
size so that no font decides anything. Headless Chrome prints every item's
rectangle, libflow's driver lays the same page out, and the two are
compared. The fixtures (tools/test_libflow_grid.inc) are worked by hand
from the specification; this is the other yardstick (GRID.md § Decisions).

DRIVER is libflow's host driver, kept by
    LIBFLOW_DRIVER=/tmp/libflow_driver tools/test_libflow_host.sh
CHROME, in the environment, is Chrome's path: the Windows one, from WSL, by
default. A NAME picks the cases whose names hold it.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

CHROME = os.environ.get('CHROME', '/mnt/c/Program Files/Google/Chrome/Application/chrome.exe')


def B(w=None, h=None, style=''):
    st = ''
    if w is not None:
        st += f'width:{w}px;'
    if h is not None:
        st += f'height:{h}px;'
    return f'<div style="{st}{style}"></div>'


def item(style='', inner=''):
    return f'<div style="{style}">{inner}</div>'


# (name, the grid's own style, its items)
CASES = [
  ('danlegt 1200', 'width:1200px;grid-template-columns:repeat(6,1fr);grid-template-rows:repeat(3,150px);gap:10px',
   [item('grid-column:span 4;grid-row:span 2'), item('grid-column:span 2;grid-row:span 2'), item('grid-column:span 3'), item('grid-column:span 3')]),
  ('danlegt 400', 'width:400px;grid-template-columns:repeat(6,1fr);grid-template-rows:repeat(3,150px);gap:10px',
   [item('grid-column:span 4;grid-row:span 2'), item('grid-column:span 2;grid-row:span 2'), item('grid-column:span 3'), item('grid-column:span 3')]),
  ('autofill 500', 'width:500px;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:8px',
   [item('', B(50, 30)) for _ in range(6)]),
  ('autofill 250', 'width:250px;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:8px',
   [item('', B(50, 30)) for _ in range(3)]),
  ('autofill 90', 'width:90px;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:8px',
   [item('', B(50, 30)) for _ in range(2)]),
  ('autofit 500', 'width:500px;grid-template-columns:repeat(auto-fit,minmax(104px,1fr));gap:8px',
   [item('', B(50, 30)) for _ in range(2)]),
  ('92px 1fr', 'width:300px;grid-template-columns:92px 1fr;row-gap:4px',
   [item('', B(20, 10)), item('', B(100, 25)), item('', B(20, 10)), item('', B(100, 5))]),
  ('minmax pct', 'width:500px;grid-template-columns:minmax(140px,42%) 1fr',
   [item('', B(20, 10)), item('', B(20, 10))]),
  ('minmax pct narrow', 'width:250px;grid-template-columns:minmax(140px,42%) 1fr',
   [item('', B(20, 10)), item('', B(20, 10))]),
  ('auto cols', 'width:500px;grid-template-columns:auto auto',
   [item('', B(50, 10)), item('', B(150, 10))]),
  ('auto cols start', 'width:500px;grid-template-columns:auto auto;justify-content:start',
   [item('', B(50, 10)), item('', B(150, 10))]),
  ('auto cols center', 'width:500px;grid-template-columns:auto auto;justify-content:center;column-gap:10px',
   [item('', B(50, 10)), item('', B(150, 10))]),
  ('space-between', 'width:500px;grid-template-columns:100px 100px 100px;justify-content:space-between',
   [item('', B(10, 10)) for _ in range(3)]),
  ('space-evenly', 'width:500px;grid-template-columns:100px 100px;justify-content:space-evenly',
   [item('', B(10, 10)) for _ in range(2)]),
  ('negative lines', 'width:400px;grid-template-columns:repeat(4,100px);grid-template-rows:50px 50px',
   [item('grid-column:1/-1'), item('grid-column:-2;grid-row:2'), item('grid-column:2/span 2')]),
  ('areas', 'width:400px;grid-template-columns:100px 1fr;grid-template-rows:40px 1fr 30px;height:300px;grid-template-areas:\'h h\' \'s m\' \'f f\'',
   [item('grid-area:m'), item('grid-area:h'), item('grid-area:f'), item('grid-area:s')]),
  ('sparse', 'width:300px;grid-template-columns:repeat(3,100px);grid-auto-rows:20px',
   [item(''), item('grid-column:span 2'), item('grid-column:span 2'), item(''), item('')]),
  ('dense', 'width:300px;grid-template-columns:repeat(3,100px);grid-auto-rows:20px;grid-auto-flow:row dense',
   [item(''), item('grid-column:span 2'), item('grid-column:span 2'), item(''), item('')]),
  ('column flow', 'width:300px;grid-template-rows:repeat(2,20px);grid-auto-columns:50px;grid-auto-flow:column',
   [item('') for _ in range(5)]),
  ('fixed row only', 'width:300px;grid-template-columns:repeat(3,100px);grid-auto-rows:20px',
   [item(''), item('grid-row:1'), item('grid-row:1'), item('grid-row:2;grid-column:span 2')]),
  ('auto rows content', 'width:300px;grid-template-columns:1fr 1fr',
   [item('', B(10, 30)), item('', B(10, 50)), item('grid-column:span 2', B(10, 10))]),
  ('fr rows height', 'width:300px;height:200px;grid-template-rows:1fr 3fr;row-gap:20px',
   [item('', B(10, 10)), item('', B(10, 10))]),
  ('fr rows no height', 'width:300px;grid-template-rows:1fr 2fr',
   [item('', B(10, 30)), item('', B(10, 20))]),
  ('justify-self', 'width:400px;grid-template-columns:repeat(4,100px);justify-items:center',
   [item('', B(20, 10)), item('justify-self:start', B(20, 10)), item('justify-self:end', B(20, 10)), item('justify-self:stretch', B(20, 10))]),
  ('align-self', 'width:400px;grid-template-columns:repeat(4,100px);grid-template-rows:60px',
   [item('', B(20, 10)), item('align-self:start', B(20, 10)), item('align-self:end', B(20, 10)), item('align-self:center', B(20, 10))]),
  ('align-content', 'width:200px;height:200px;grid-template-rows:30px 30px;align-content:end',
   [item('', B(10, 10)), item('', B(10, 10))]),
  ('spanning auto', 'width:500px;grid-template-columns:auto auto auto;justify-content:start',
   [item('', B(30, 10)), item('', B(40, 10)), item('', B(10, 10)), item('grid-column:1/3', B(200, 10))]),
  ('fr min content', 'width:200px;grid-template-columns:1fr 1fr',
   [item('', B(150, 10)), item('', B(10, 10))]),
  ('fr minmax0', 'width:200px;grid-template-columns:minmax(0,1fr) minmax(0,1fr)',
   [item('', B(150, 10)), item('', B(10, 10))]),
  ('fit-content', 'width:500px;grid-template-columns:fit-content(100px) fit-content(300px) 1fr',
   [item('', B(150, 10)), item('', B(80, 10)), item('', B(10, 10))]),
  ('inline-grid', 'display:inline-grid;grid-template-columns:auto 1fr 2fr;gap:5px',
   [item('', B(30, 10)), item('', B(40, 10)), item('', B(50, 10))]),
  ('margins', 'width:400px;grid-template-columns:200px 200px;grid-template-rows:100px',
   [item('margin:10px 20px', B(10, 10)), item('margin:auto;width:50px;height:20px')]),
  ('min-height stretch', 'width:200px;min-height:150px;grid-template-rows:auto auto',
   [item('', B(10, 20)), item('', B(10, 20))]),
  ('max-content min', 'width:100px;grid-template-columns:max-content 1fr',
   [item('', B(150, 10)), item('', B(10, 10))]),
  ('implicit past', 'width:300px;grid-template-columns:100px 100px;grid-auto-columns:30px',
   [item('grid-column:4'), item('grid-row:2;grid-column:3/5')]),
  ('span over fixed+auto', 'width:500px;grid-template-columns:50px auto auto;justify-content:start',
   [item('grid-column:1/4', B(300, 10)), item('', B(10,10)), item('', B(20,10)), item('', B(30,10))]),
  ('span unequal', 'width:600px;grid-template-columns:auto auto;justify-content:start',
   [item('', B(10, 10)), item('', B(100, 10)), item('grid-column:span 2', B(300, 10))]),
  ('span min-content max', 'width:600px;grid-template-columns:min-content max-content;justify-content:start',
   [item('grid-column:span 2', B(200, 10))]),
  ('frames', 'width:400px;padding:10px;border:5px solid;grid-template-columns:1fr 2fr;gap:6px',
   [item('padding:4px;border:3px solid', B(10, 10)), item('padding:0 7px', B(10, 20))]),
  ('autofit gap', 'width:600px;grid-template-columns:repeat(auto-fit,100px);column-gap:20px;justify-content:center',
   [item('', B(10, 10)), item('', B(10, 10))]),
  ('autofill gap center', 'width:600px;grid-template-columns:repeat(auto-fill,100px);column-gap:20px;justify-content:center',
   [item('', B(10, 10)), item('', B(10, 10))]),
  ('order', 'width:300px;grid-template-columns:repeat(3,100px)',
   [item('order:2', B(10,10)), item('', B(10,10)), item('order:-1', B(10,10))]),
  ('nested', 'width:400px;grid-template-columns:1fr 1fr',
   [item('display:grid;grid-template-columns:auto 1fr', B(30,10)+B(20,20)), item('', B(10,10))]),
  ('pct track', 'width:400px;grid-template-columns:25% 50%;gap:10px',
   [item('', B(10,10)), item('', B(10,10))]),
  ('pct gap', 'width:400px;grid-template-columns:1fr 1fr;column-gap:10%',
   [item('', B(10,10)), item('', B(10,10))]),
  ('overflow items', 'width:200px;grid-template-columns:1fr 1fr',
   [item('overflow:hidden', B(300,10)), item('', B(10,10))]),
  ('width item', 'width:300px;grid-template-columns:1fr 1fr 1fr',
   [item('width:40px', B(10,10)), item('width:50%', B(10,10)), item('max-width:30px', B(60,10))]),
  ('height item', 'width:300px;grid-template-columns:1fr 1fr;grid-auto-rows:80px',
   [item('height:30px', B(10,10)), item('min-height:100px', B(10,10))]),
  ('three spans dense', 'width:400px;grid-template-columns:repeat(4,100px);grid-auto-rows:10px;grid-auto-flow:dense',
   [item('grid-column:span 3'), item('grid-column:span 3'), item(''), item(''), item('grid-column:span 2')]),
  ('column dense', 'width:400px;grid-template-rows:repeat(3,10px);grid-auto-columns:40px;grid-auto-flow:column dense',
   [item('grid-row:span 2'), item('grid-row:span 2'), item(''), item('')]),
  ('line end only', 'width:400px;grid-template-columns:repeat(4,100px)',
   [item('grid-column-end:3', B(10,10)), item('grid-column-end:span 2', B(10,10)), item('grid-row-end:3;grid-column:4', B(10,10))]),
  ('auto rows list', 'width:100px;grid-auto-rows:10px 20px',
   [item('') for _ in range(4)]),
  ('empty template rows', 'width:100px;grid-template-rows:30px 40px',
   []),
  ('area name lines', 'width:300px;grid-template-columns:100px 100px 100px;grid-template-areas:\'a a b\'',
   [item('grid-column:a-start/b-end;grid-row:2'), item('grid-column:b'), item('grid-column:a')]),
  ('unknown name', 'width:300px;grid-template-columns:100px 100px 100px',
   [item('grid-column:foo', B(10,10)), item('', B(10,10))]),
  ('block content text-free', 'width:300px;grid-template-columns:auto 1fr;align-items:center',
   [item('', B(60,50)), item('', B(10,10))]),
  ('stretch replaced-like', 'width:300px;grid-template-columns:100px;grid-template-rows:100px',
   [item('display:flex', B(10,10))]),
]


def page(css, items):
    return ('<!doctype html><html><head><style>body{margin:0;font:16px sans-serif}'
            '.g{display:grid}.g>div{background:#ccd}</style></head><body>'
            f'<div class=g style="{css}">' + ''.join(items) + '</div>'
            '<pre id=out>(needs script)</pre><script>'
            'const g=document.querySelector(".g").getBoundingClientRect();const o=[];'
            'o.push("C "+[g.left,g.top,g.width,g.height].map(Math.round).join(" "));'
            'for(const c of document.querySelector(".g").children){const r=c.getBoundingClientRect();'
            'o.push([r.left,r.top,r.width,r.height].map(Math.round).join(" "));}'
            'document.getElementById("out").textContent=o.join("\\n");</script></body></html>')


def chrome(path, profile):
    win = subprocess.run(['wslpath', '-w', path], capture_output=True, text=True).stdout.strip()
    out = subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--no-first-run',
                          '--user-data-dir=' + profile, '--window-size=1400,900',
                          '--dump-dom', 'file:///' + win.replace('\\', '/')],
                         capture_output=True, text=True, timeout=60).stdout
    m = re.search(r'<pre id="out">(.*?)</pre>', out, re.S)
    return m.group(1).strip().split('\n') if m else ['(no answer)']


# The grid is the page's first div, and its items the blocks one level in.
def flow(driver, path):
    out = subprocess.run([driver, '--cascade', path, '1400'], capture_output=True,
                         text=True).stdout
    res, depth = [], None
    for ln in out.split('\n'):
        m = re.match(r'^( *)block (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)', ln)
        if not m:
            continue
        d = len(m.group(1))
        if depth is None and m.group(2) == 'div':
            depth = d
            res.append('C ' + ' '.join(m.group(3, 4, 5, 6)))
        elif depth is not None and d <= depth:
            break
        elif depth is not None and d == depth + 2:
            res.append(' '.join(m.group(3, 4, 5, 6)))
    return res


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    driver, only = sys.argv[1], sys.argv[2:]
    work = tempfile.mkdtemp(prefix='grid_probe')
    # A throwaway profile on the Windows side, never the person's own.
    profile_wsl = '/mnt/c/temp/grid-probe-profile'
    os.makedirs(profile_wsl, exist_ok=True)
    profile = subprocess.run(['wslpath', '-w', profile_wsl], capture_output=True,
                             text=True).stdout.strip()
    differ = 0
    try:
        for name, css, items in CASES:
            if only and not any(o in name for o in only):
                continue
            path = os.path.join(work, re.sub(r'\W+', '_', name) + '.html')
            with open(path, 'w') as f:
                f.write(page(css, items))
            c, fl = chrome(path, profile), flow(driver, path)
            differ += c != fl
            print(('ok   ' if c == fl else 'DIFF ') + name)
            if c != fl:
                for i in range(max(len(c), len(fl))):
                    a = c[i] if i < len(c) else '-'
                    b = fl[i] if i < len(fl) else '-'
                    print(f'      chrome {a:24} flow {b:24}' + ('' if a == b else '   <'))
    finally:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(profile_wsl, ignore_errors=True)
    print(f'{differ} differ')
    sys.exit(1 if differ else 0)


if __name__ == '__main__':
    main()
