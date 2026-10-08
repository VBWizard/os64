#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I kernel/include \
    tools/test_hid_keyboard_map_host.c kernel/src/driver/system/hid_keyboard_map.c -o "$work/keyboard-map"
"$work/keyboard-map"
