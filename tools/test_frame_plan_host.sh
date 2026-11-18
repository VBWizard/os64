#!/bin/bash
# Run yonder's animation-frame plan (userland/apps/yonder/frame_plan.c: when
# a frame is asked for, the frame's time, skips, the cost rule's rests, the
# next blank) on the host under ASan and UBSan, against the cases and the
# random walk in tools/test_frame_plan_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_frame_plan_host.c userland/apps/yonder/frame_plan.c -o "$work/test_frame_plan"

"$work/test_frame_plan"
