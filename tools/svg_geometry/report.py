"""Create portable PNGs and a self-contained HTML gallery from proof masks."""
import base64
import html
from pathlib import Path
import struct
import zlib

PANELS = [
    ('scaled-cap', 'Correct: stroke, then scale(4,1)'),
    ('scaled-cap-reference', 'Independent analytic reference'),
    ('wrong-device-stroke', 'Negative control: stroke in device space'),
    ('sheared-cap', 'Reflection and shear'),
    ('mixed-closure', 'Open contour beside closed contour'),
    ('curves', 'Quadratic and cubic fill'),
    ('nonzero', 'Nonzero: same winding fills the center'),
    ('evenodd', 'Evenodd: the same path has a hole'),
    ('miter-below', 'Miter below the bevel threshold'),
    ('miter-above', 'Miter above the bevel threshold'),
    ('miter-clip', 'Clipped miter'),
]

def png(width, height, rgb):
    def chunk(tag, data):
        return struct.pack('>I', len(data))+tag+data+struct.pack('>I', zlib.crc32(tag+data))
    rows = b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            +chunk(b'IDAT', zlib.compress(rows))+chunk(b'IEND', b''))

def report(output: Path):
    figures = []
    # Light/dark backgrounds expose differences in alpha without changing masks.
    for stem, title in PANELS:
        raw = (output/(stem+'.pgm')).read_bytes()
        magic, dimensions, maximum, mask = raw.split(b'\n', 3)
        width, height = map(int, dimensions.split())
        assert magic == b'P5' and maximum == b'255' and len(mask) == width*height
        rgb = bytearray()
        for y in range(height):
            for background in ((242, 245, 250), (20, 26, 38)):
                for alpha in mask[y*width:(y+1)*width]:
                    for foreground, back in zip((32, 145, 222), background):
                        rgb.append((foreground*alpha+back*(255-alpha)+127)//255)
        encoded = png(width*2, height, rgb)
        (output/(stem+'.png')).write_bytes(encoded)
        image = base64.b64encode(encoded).decode()
        figures.append(f'<figure><img width="{width*2}" height="{height}" '
                       f'src="data:image/png;base64,{image}" alt="{html.escape(title)}">'
                       f'<figcaption>{html.escape(title)}</figcaption></figure>')
    log = html.escape((output/'results.txt').read_text())
    (output/'index.html').write_text('''<!doctype html><html lang="en"><meta charset="utf-8">
<title>SVG geometry proof</title><style>
body{font:16px system-ui;background:#101722;color:#e9eff8;margin:32px auto;max-width:1240px}
h1{font-size:28px}main{display:flex;flex-wrap:wrap;gap:20px}figure{margin:0;background:#1c2737;padding:12px;border-radius:8px}
figcaption{padding-top:10px;font-size:14px}pre{white-space:pre-wrap;background:#1c2737;padding:20px}
</style><h1>SVG geometry proof</h1><p>Os64's pinned FreeType port. Coverage on light and dark backgrounds.
These are native geometry fixtures, not browser SVG rendering.</p><main>'''+''.join(figures)+
        '</main><h2>Measured checks</h2><pre>'+log+'</pre></html>')
