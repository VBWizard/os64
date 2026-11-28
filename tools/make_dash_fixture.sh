#!/bin/bash
# Make the fed media's fixture (MEDIA_SOURCE.md § 5): tools/pages/sync-720p60.mp4,
# the flash-and-beep clip, cut into fragmented-MP4 segments as a DASH
# player fetches them, with the host's ffmpeg. Two picture renditions of
# different sizes, so a quality switch changes the picture's size, and one
# sound at 44.1 kHz, so the sound's count is checked at a rate 48 kHz output
# frames do not divide:
#
#   sync-dash-0-init.m4s, sync-dash-0-<n>.m4s   1280x720, H.264 High with B-frames
#   sync-dash-1-init.m4s, sync-dash-1-<n>.m4s   640x360, the same
#   sync-dash-2-init.m4s, sync-dash-2-<n>.m4s   AAC-LC, 44.1 kHz stereo
#
# A keyframe every 2 s and 2 s segments, so the renditions' segments line up
# and a switch can land at any segment. The pictures are tagged BT.709:
# libmedia guesses an untagged picture's matrix from its width, which would
# shift the hue at a switch between the two sizes. The segments are written
# into tools/pages, where PAGE_FIXTURES ships *.m4s to /tests/pages.
#
#     tools/make_dash_fixture.sh
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ffmpeg -hide_banner -loglevel error -i "$top/tools/pages/sync-720p60.mp4" \
    -map 0:v -map 0:v -map 0:a \
    -c:v libx264 -profile:v high -bf 2 -g 120 -keyint_min 120 -sc_threshold 0 \
    -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
    -s:v:0 1280x720 -b:v:0 600k -s:v:1 640x360 -b:v:1 200k \
    -c:a aac -ar 44100 -ac 2 -b:a 96k \
    -f dash -seg_duration 2 -use_template 1 -use_timeline 0 \
    -adaptation_sets "id=0,streams=0,1 id=1,streams=2" \
    -init_seg_name 'sync-dash-$RepresentationID$-init.$ext$' \
    -media_seg_name 'sync-dash-$RepresentationID$-$Number$.$ext$' \
    "$work/sync-dash.mpd"
# The manifest is the page's business, not os64's: the names say it all.
for f in "$work"/sync-dash-*.m4s; do
    cp "$f" "$top/tools/pages/$(basename "$f")"
done
ls -l "$top"/tools/pages/sync-dash-*.m4s | awk '{ s += $5 } END { print NR " files, " s " bytes" }'
