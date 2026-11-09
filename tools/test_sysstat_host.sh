#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
. tools/host_build.sh
host_flags=(-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
    -I userland/libos64/include -I abi/include)
host_sources=(tools/test_sysstat_host.c userland/apps/sysstat/output.c
    userland/libos64/args.c userland/libos64/fmt.c userland/libos64/str.c)
host_objects "$work"
cc "${host_objs[@]}" -fsanitize=address,undefined -o "$work/test_sysstat"
"$work/test_sysstat" "$work"
python3 tools/test_sysstat_json.py "$work"
