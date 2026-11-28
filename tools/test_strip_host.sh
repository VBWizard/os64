#!/bin/bash
# Drive yonder's strip states (userland/apps/yonder/strip.c) on the host,
# under ASan and UBSan, beside a model. See tools/test_strip_host.c.
#
#   tools/test_strip_host.sh [random rounds, default 2000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   userland/apps/yonder/strip.c tools/test_strip_host.c -o "$work/test_strip"

"$work/test_strip" "${1:-2000}"
