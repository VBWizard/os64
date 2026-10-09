#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/memory \
    tools/test_bt_multi_host.c kernel/src/driver/system/hid_keyboard_map.c \
    kernel/src/driver/system/hid_mouse.c -lcrypto -o "$work/bt-host"
"$work/bt-host"
