#!/bin/bash
# Drive the waited invalidation's rounds on the host, under ASan and UBSan.
#
# The core numbers the rounds and judges their answers (shootdown.h);
# tools/test_shootdown_host.c runs it beside a model of page tables and TLBs.
#
#   tools/test_shootdown_host.sh [random runs, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/shootdown.c tools/test_shootdown_host.c -o "$work/test_shootdown"

"$work/test_shootdown" "${1:-20000}"
