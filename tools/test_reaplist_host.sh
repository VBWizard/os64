#!/bin/bash
# Drive the reap list's lifecycle on the host, under ASan and UBSan.
#
# The core is the list of ended threads whose stacks are still to be
# released (reaplist.h); tools/test_reaplist_host.c runs it beside a model.
#
#   tools/test_reaplist_host.sh [random rounds, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/reaplist.c tools/test_reaplist_host.c -o "$work/test_reaplist"

"$work/test_reaplist" "${1:-20000}"
