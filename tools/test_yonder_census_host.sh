#!/usr/bin/env bash
# yonder's memory census (userland/apps/yonder/census.c; MEMORY_CENSUS.md) on
# the host, under the sanitizers: its formats, its byte budgets at their
# worst, the timeline's thinning and host matching
# (tools/test_yonder_census_host.c).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fno-builtin -masm=intel \
   -fsanitize=address,undefined -fno-sanitize-recover=all \
   -I abi/include -I userland/libos64/include -I userland/libjs/include -I userland/apps/yonder \
   tools/test_yonder_census_host.c userland/apps/yonder/census.c \
   userland/libos64/fmt.c userland/libos64/str.c -o "$work/census_driver"
"$work/census_driver"
