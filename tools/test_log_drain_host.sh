#!/bin/bash
# Keep O0: the kernel uses it, and an optimizer could discard the disabled
# statistics output's formatting and hide a forbidden CLS read there.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
shopt -s globstar
includes=(-Iabi/include)
for dir in kernel/include/**/; do includes+=(-I"$dir"); done
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -O0 -g -Wall -Wextra -Werror \
    -fno-builtin -ffunction-sections -fdata-sections -masm=intel \
    "${includes[@]}" tools/test_log_drain_host.c -Wl,--gc-sections \
    -o "$work/test_log_drain"
for case in empty one backlog full emergency deferred; do
    echo "log drain: $case"
    "$work/test_log_drain" "$case"
done
