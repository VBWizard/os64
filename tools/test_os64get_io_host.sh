#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all -fno-pie -no-pie \
   -I userland/libtls/include -I userland/libos64/include -I abi/include \
   tools/test_os64get_io_host.c userland/apps/os64get/url_io.c -o "$work/io"
ASAN_OPTIONS=detect_leaks=0 "$work/io"
