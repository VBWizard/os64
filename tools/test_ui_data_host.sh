#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out=${1:-/tmp/os64-ui-data-host}
mkdir -p "$out"
cc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iuserland/libos64/include -Iabi/include tools/test_ui_data_host.c userland/libos64/ui_data.c userland/libos64/str.c -o "$out/test_ui_data"
ASAN_OPTIONS=detect_leaks=1 "$out/test_ui_data"
