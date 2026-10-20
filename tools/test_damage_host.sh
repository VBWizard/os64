#!/usr/bin/env bash
# The damage list's rule (shared/damage.c) under ASan and UBSan: the named
# cases, the compositor's former code as the oracle, and the pixel oracle
# (tools/test_damage_host.c says which is which).
set -eu
cd "$(git rev-parse --show-toplevel)"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I abi/include -o "$dir/damage" tools/test_damage_host.c shared/damage.c
"$dir/damage"
