#!/bin/bash
# Run gplay's arithmetic that needs no machine (userland/apps/gplay/view.c)
# on the host under ASan and UBSan, against tools/test_gplay_host.c. The
# player itself is glue and is proven by /tests/gplaytest.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/apps/gplay \
   tools/test_gplay_host.c userland/apps/gplay/view.c -o "$work/test_gplay"

"$work/test_gplay"
