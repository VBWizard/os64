#!/usr/bin/env python3
"""Write tools/pages/bbb-480p-3s-av.mp4: the three seconds of Big Buck Bunny
that bbb-480p-3s.mp4 holds, with the film's own sound beside the picture.

    tools/gen_bbb_av.py <bbb_sunflower_1080p_60fps_normal.mp4>

The picture is bbb-480p-3s.mp4's H.264 stream, COPIED (not encoded again),
so every picture decodes to what avbench's CRCs say. The sound is the
master's stereo track over the same stretch of the film (6:55, the cut
bbb-480p-3s.mp4 was made from), encoded to AAC-LC, 48 kHz stereo,
128 kb/s by the host's FFmpeg (its own AAC encoder, LGPL). It is the
fixture for libmedia's picture side (/tests/mediatest): a file with both,
whose pictures and sound share one timeline. The picture-only clip stays
as the file with no sound.

The bytes are committed, so the build never needs the master or the host's
ffmpeg; this script is how they were made, and it prints what it wrote.
The master is the Blender Foundation's 60 fps "sunflower" release
(Creative Commons Attribution 3.0, like the clip; the notice is
bbb-480p-3s-av.txt beside it).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PICTURE = ROOT / 'tools/pages/bbb-480p-3s.mp4'
OUT = ROOT / 'tools/pages/bbb-480p-3s-av.mp4'
CUT_START = '415'           # 6:55, where bbb-480p-3s.mp4 was cut
CUT_LENGTH = '3.066667'     # the picture stream's own length (92 pictures at 30)

if len(sys.argv) != 2:
    raise SystemExit(__doc__)
master = sys.argv[1]
subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                '-i', str(PICTURE),
                '-ss', CUT_START, '-t', CUT_LENGTH, '-i', master,
                '-map', '0:v:0', '-map', '1:a:0',
                '-c:v', 'copy',
                '-c:a', 'aac', '-b:a', '128k', '-ar', '48000', '-ac', '2',
                '-bitexact', '-fflags', '+bitexact', '-movflags', '+faststart',
                str(OUT)], check=True)
probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                        'stream=index,codec_name,start_time,duration,nb_frames,sample_rate,channels',
                        '-of', 'compact', str(OUT)], check=True, capture_output=True, text=True)
print(f'wrote {OUT.relative_to(ROOT)}: {OUT.stat().st_size} bytes')
print(probe.stdout.strip())
