#!/bin/bash
# What a page's style sheets cost libgarb's cascade in anchored :has()
# compounds, and which are bare (tools/garb_anchors.c says what it prints).
# Usage: tools/garb_anchors.sh [-q] sheet.css...
set -eu
top=$(git rev-parse --show-toplevel)
cd "$top"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# The flags libgarb's other host builds use (tools/test_garb_host.sh says why
# -fno-builtin and -fno-tree-loop-distribute-patterns), without sanitizers
# and at -O2, as yonder is built, since `-p` prints times.
. tools/host_build.sh
host_flags=(-std=c11 -g -O2 -fno-builtin -fno-tree-loop-distribute-patterns -DGARB_MEDIA_COUNT
   -I userland/libgarb/include -I userland/libgarb -I userland/libhtml/include
   -I userland/libos64/include -I userland -I abi/include)
host_sources=(tools/garb_anchors.c
   userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c
   userland/libgarb/dump.c userland/libgarb/select.c
   userland/libgarb/values.c userland/libgarb/props.c
   userland/libgarb/media.c userland/libgarb/cascade.c userland/libgarb/timing.c
   userland/libhtml/core.c userland/libhtml/encoding.c
   userland/libhtml/tokenizer.c userland/libhtml/tree.c userland/libhtml/dom.c
   userland/libhtml/fragment.c userland/libhtml/serialize.c userland/libhtml/shadow.c userland/libhtml/range.c
   userland/libos64/str.c userland/libos64/fmt.c userland/libos64/arena.c)
host_objects "$work" >/dev/null
cc "${host_objs[@]}" -lm -o "$work/garb_anchors"
cd "$OLDPWD" 2>/dev/null || true
"$work/garb_anchors" "$@"
