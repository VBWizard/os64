#!/bin/bash
# Make the fed media's fixture (MEDIA_SOURCE.md § 5), the one set its host
# test, gplay --fed and yonder's Media Source pages all use:
# tools/pages/sync-720p60.mp4, the flash-and-beep clip, as the separate
# picture and sound files a DASH player fetches, with the host's ffmpeg.
#
#   sync-fed-1080p.mp4  1920x1080  H.264 High with B-frames
#   sync-fed-720p.mp4   1280x720   the same
#   sync-fed-480p.mp4   854x480    the same
#   sync-fed-a48.mp4    AAC-LC, 48 kHz stereo
#   sync-fed-a44.mp4    AAC-LC, 44.1 kHz stereo (a rate 48 kHz output
#                       frames do not divide: the sound's count is checked)
#
# Each is a fragmented MP4, 10 s, with a fragment every 2 s: the pictures
# have a keyframe at every 2 s and nowhere else, so every picture fragment
# starts on one and the three sizes share keyframe times (a switch can land
# at any fragment), and the sound is cut on the same grid. The pictures are
# tagged BT.709: libmedia guesses an untagged picture's matrix from its
# width, which would shift the hue at a switch between sizes.
#
# Beside each, <name>.json: the initialization segment's [offset, length],
# then per fragment [offset, length, start, duration] (seconds), so a page
# or a test appends whole fragments by byte range.
#
#     tools/make_dash_fixture.sh
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
src=$top/tools/pages/sync-720p60.mp4
out=$top/tools/pages
frag='-movflags +frag_keyframe+empty_moov+default_base_moof'
picture() {   # size bitrate name
    # shellcheck disable=SC2086  # $frag is words
    ffmpeg -hide_banner -loglevel error -y -i "$src" -map 0:v -an \
        -c:v libx264 -profile:v high -bf 2 -g 120 -keyint_min 120 -sc_threshold 0 \
        -color_primaries bt709 -color_trc bt709 -colorspace bt709 -s "$1" -b:v "$2" \
        $frag "$out/$3"
}
sound() {     # rate name
    # shellcheck disable=SC2086
    ffmpeg -hide_banner -loglevel error -y -i "$src" -map 0:a -vn -c:a aac -ar "$1" -ac 2 -b:a 96k \
        -frag_duration 2000000 $frag "$out/$2"
}
picture 1920x1080 1200k sync-fed-1080p.mp4
picture 1280x720 600k sync-fed-720p.mp4
picture 854x480 300k sync-fed-480p.mp4
sound 48000 sync-fed-a48.mp4
sound 44100 sync-fed-a44.mp4
for f in sync-fed-1080p sync-fed-720p sync-fed-480p sync-fed-a48 sync-fed-a44; do
    python3 -I - "$out/$f.mp4" "$out/$f.json" <<'PY'
import json, struct, sys

data = open(sys.argv[1], 'rb').read()


def boxes(at, end):
    while at + 8 <= end:
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        head = 8
        if size == 1:
            size, head = struct.unpack('>Q', data[at + 8:at + 16])[0], 16
        yield kind.decode('latin-1'), at, at + head, at + size
        at += size


def child(at, end, kind):
    for k, s, b, e in boxes(at, end):
        if k == kind:
            return s, b, e
    return None


top = list(boxes(0, len(data)))
moov = next(t for t in top if t[0] == 'moov')
trak = child(moov[2], moov[3], 'trak')
mdia = child(trak[1], trak[2], 'mdia')
mdhd = child(mdia[1], mdia[2], 'mdhd')
v = data[mdhd[1]]
timescale = struct.unpack('>I', data[mdhd[1] + (20 if v == 1 else 12):][:4])[0]
mvex = child(moov[2], moov[3], 'mvex')
trex = child(mvex[1], mvex[2], 'trex')
default_duration = struct.unpack('>I', data[trex[1] + 12:trex[1] + 16])[0]
first = next(t for t in top if t[0] == 'moof')
index = {'init': [0, first[1]], 'timescale': timescale, 'fragments': []}
for i, (kind, start, body, end) in enumerate(top):
    if kind != 'moof':
        continue
    mdat = top[i + 1]
    traf = child(body, end, 'traf')
    tfhd = child(traf[1], traf[2], 'tfhd')
    flags = struct.unpack('>I', data[tfhd[1]:tfhd[1] + 4])[0] & 0xffffff
    at = tfhd[1] + 8 + (8 if flags & 1 else 0) + (4 if flags & 2 else 0)
    dur = struct.unpack('>I', data[at:at + 4])[0] if flags & 8 else default_duration
    tfdt = child(traf[1], traf[2], 'tfdt')
    base = struct.unpack('>Q' if data[tfdt[1]] == 1 else '>I',
                         data[tfdt[1] + 4:tfdt[1] + (12 if data[tfdt[1]] == 1 else 8)])[0]
    trun = child(traf[1], traf[2], 'trun')
    tflags = struct.unpack('>I', data[trun[1]:trun[1] + 4])[0] & 0xffffff
    count = struct.unpack('>I', data[trun[1] + 4:trun[1] + 8])[0]
    at = trun[1] + 8 + (4 if tflags & 1 else 0) + (4 if tflags & 4 else 0)
    total = 0
    for _ in range(count):
        d = dur
        if tflags & 0x100:
            d = struct.unpack('>I', data[at:at + 4])[0]
            at += 4
        at += (4 if tflags & 0x200 else 0) + (4 if tflags & 0x400 else 0) + (4 if tflags & 0x800 else 0)
        total += d
    index['fragments'].append([start, mdat[3] - start, round(base / timescale, 6), round(total / timescale, 6)])
json.dump(index, open(sys.argv[2], 'w'), indent=1)
PY
done
ls -l "$out"/sync-fed-* | awk '{ s += $5; print $5, $9 } END { print NR " files, " s " bytes" }'
