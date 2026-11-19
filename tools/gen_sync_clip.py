#!/usr/bin/env python3
"""Write tools/pages/sync-720p60.mp4: a flash and a beep together, once a
second, for judging whether a player's sound and picture are in step.

    tools/gen_sync_clip.py

Ten seconds of black 1280x720 at 60 pictures a second; the first two
pictures of every second are white. Beside it, AAC 48 kHz stereo: a
1 kHz tone for the first 33 ms of every second (two pictures' worth),
silence otherwise. A player in step shows the flash as the beep is heard;
gplay's `[` and `]` move the sound against the picture until they meet,
and the value that makes them meet is the error in the sink's reported
delay (VIDEO.md § 4, BRIEF_VIDEO_V4.md question 1).

Made from FFmpeg's own test sources (lavfi `color` and `aevalsrc`, LGPL) by
the host's FFmpeg; the picture is encoded with libx264, whose output is
the clip's data and carries no licence of the encoder's. The bytes are
committed, so the build never needs the host's ffmpeg; this script is how
they were made, and it prints what it wrote.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tools/pages/sync-720p60.mp4'
SECONDS = '10'
# The flash by picture number, not by time: a time test on 60ths of a
# second rounds either way at the edge and flashed three pictures once.
FLASH = 'lt(mod(n,60),2)'
# The beep by sample (aevalsrc evaluates every sample; a gate on the
# filter's `enable` opens only at its 1024-sample frames, and drifted).
BEEP = 'if(lt(mod(n,48000),1600),0.5*sin(2*PI*1000*t),0)'   # 1600 samples = two pictures

subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
                '-f', 'lavfi', '-i', f'color=c=black:s=1280x720:r=60:d={SECONDS}',
                '-f', 'lavfi', '-i', f"aevalsrc=exprs='{BEEP}|{BEEP}':sample_rate=48000:duration={SECONDS}",
                '-vf', f"drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='{FLASH}'",
                '-c:v', 'libx264', '-preset', 'veryslow', '-crf', '18', '-g', '60', '-pix_fmt', 'yuv420p',
                '-c:a', 'aac', '-b:a', '96k', '-ar', '48000', '-ac', '2',
                '-bitexact', '-fflags', '+bitexact', '-movflags', '+faststart',
                str(OUT)], check=True)
probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries',
                        'stream=index,codec_name,start_time,duration,nb_frames,sample_rate,channels',
                        '-of', 'compact', str(OUT)], check=True, capture_output=True, text=True)
print(f'wrote {OUT.relative_to(ROOT)}: {OUT.stat().st_size} bytes')
print(probe.stdout.strip())
