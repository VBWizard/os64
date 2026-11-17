#!/bin/bash
# Drive the frame clock's arithmetic on the host, under ASan and UBSan,
# beside a model display that sweeps and a copier that copies. See
# tools/test_frame_clock_host.c for what is asserted.
#
#   tools/test_frame_clock_host.sh [random rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/gui/frame_clock.c tools/test_frame_clock_host.c -o "$work/test_frame_clock"

"$work/test_frame_clock" "${1:-3000}"
