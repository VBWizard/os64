#!/bin/bash
# Drive the mapping list's core on the host, under ASan and UBSan.
#
# The core is a task's list of mappings with each mapping's holds
# (memory/maplist.h); tools/test_maplist_host.c runs it beside a model.
#
#   tools/test_maplist_host.sh [random rounds, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/memory/maplist.c tools/test_maplist_host.c -o "$work/test_maplist"

"$work/test_maplist" "${1:-20000}"
