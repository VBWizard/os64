#!/bin/bash
# Run yonder's load timeline (userland/apps/yonder/load_timeline.c: where a
# navigation's first minutes went, step by step and slice by slice, with
# its longest steps and milestones) on the host under ASan and UBSan,
# against the cases in tools/test_load_timeline_host.c.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/yonder \
   tools/test_load_timeline_host.c userland/apps/yonder/load_timeline.c -o "$work/test_load_timeline"

"$work/test_load_timeline"
