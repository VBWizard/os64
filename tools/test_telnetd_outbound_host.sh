#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
   -fsanitize=address,undefined -fno-sanitize-recover=all -Wl,--gc-sections \
   -I userland/libos64/include -I abi/include tools/test_telnetd_outbound_host.c \
   -o "$work/test"
"$work/test"
