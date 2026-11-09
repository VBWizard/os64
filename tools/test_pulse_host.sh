#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=all -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -Iuserland/apps/pulse -Iuserland/libos64/include -Iabi/include \
    tools/test_pulse_host.c userland/apps/pulse/pulse_data.c \
    userland/libos64/str.c userland/libos64/fmt.c userland/libos64/conf.c \
    -o "$work/test_pulse"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$work/test_pulse"
