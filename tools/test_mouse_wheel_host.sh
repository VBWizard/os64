#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -Dasm=__asm__ -O1 -g -fno-builtin -Wall -Wextra -Werror -masm=intel -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/memory -I kernel/include/driver/system -I abi/include \
    tools/test_mouse_wheel_host.c -Wl,--gc-sections -o "$work/mouse-wheel"
"$work/mouse-wheel"
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/memory \
    tools/test_hid_mouse_host.c -o "$work/hid-mouse"
"$work/hid-mouse"
