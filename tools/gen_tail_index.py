#!/usr/bin/env python3
"""Write tools/pages/tail-index.mp4: an MP4 NOT written for the web, its
index (`moov`) after its data, for opening a file whose index is at its end
(VIDEO.md § 5; BRIEF_VIDEO_V6.md, V.6.1).

    tools/gen_tail_index.py

Eight seconds of FFmpeg's `testsrc` at 640x360, 30 pictures a second, with
moving noise over it (the test pattern alone compresses to a fraction of
the size wanted) and a 440 Hz tone in AAC 48 kHz stereo beside it, some
550 KB: more than the
window's jump gap (YONDER_MEDIA_JUMP_GAP), so a fetch served slowly reaches
the tail by a Range request rather than by waiting. FFmpeg's MP4
writer puts `moov` last unless told `+faststart`, which is what a camera or
an editor that never meant the file for a page does; this script asks for
neither and then checks the order, so a change of FFmpeg's default cannot
quietly give a fixture that proves nothing.

Made from FFmpeg's own test sources (lavfi, LGPL) by the host's FFmpeg; the
picture is encoded with libx264, whose output is the clip's data and carries
no licence of the encoder's. The bytes are committed, so the build never
needs the host's ffmpeg; this script is how they were made, and it prints
what it wrote.
"""
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tools/pages/tail-index.mp4'
SECONDS = '8'

subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'lavfi', '-i', f'testsrc=size=640x360:rate=30:duration={SECONDS}',
                '-f', 'lavfi', '-i', f'sine=frequency=440:sample_rate=48000:duration={SECONDS}',
                '-vf', 'noise=alls=18:allf=t',
                '-c:v', 'libx264', '-preset', 'veryslow', '-crf', '30', '-g', '30', '-pix_fmt', 'yuv420p',
                '-c:a', 'aac', '-b:a', '64k', '-ar', '48000', '-ac', '2',
                '-bitexact', '-fflags', '+bitexact',
                str(OUT)], check=True)

# The top-level boxes, in order: `moov` must come after `mdat`.
data = OUT.read_bytes()
order, at = [], 0
while at + 8 <= len(data):
    size, kind = struct.unpack('>I4s', data[at:at + 8])
    if size == 1:
        size = struct.unpack('>Q', data[at + 8:at + 16])[0]
    order.append(kind.decode('latin1'))
    at += size if size else len(data) - at
if 'moov' not in order or 'mdat' not in order or order.index('moov') < order.index('mdat'):
    raise SystemExit(f'{OUT.name}: boxes {order}: moov is not after mdat')
probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                        'stream=index,codec_name,duration,nb_frames,sample_rate,channels',
                        '-of', 'compact', str(OUT)], check=True, capture_output=True, text=True)
print(f'wrote {OUT.relative_to(ROOT)}: {len(data)} bytes, boxes {" ".join(order)}')
print(probe.stdout.strip())
