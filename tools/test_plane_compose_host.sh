#!/bin/bash
# Drive the plane composer on the host, under ASan and UBSan, beside a model
# that decides every pixel on its own (kernel/src/gui/plane_compose.c;
# tools/test_plane_compose_host.c says what it checks).
#
#   tools/test_plane_compose_host.sh [random rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/gui/plane_compose.c tools/test_plane_compose_host.c -o "$work/test_plane_compose"

"$work/test_plane_compose" "${1:-3000}"
