#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -Dasm=__asm__ -O1 -g -fno-builtin -Wall -Wextra -Werror -masm=intel -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/memory -I kernel/include/driver/system -I abi/include \
    tools/test_mouse_settings_host.c -Wl,--gc-sections -o "$work/mouse-settings"
"$work/mouse-settings"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I userland/libos64/include -I abi/include \
    tools/test_mouse_settings_lib_host.c userland/libos64/mouse_settings.c \
    userland/libos64/conf.c userland/libos64/str.c userland/libos64/fmt.c \
    -Wl,--gc-sections,--wrap=os64_conf_find_bytes,--wrap=os64_conf_write_checked -o "$work/mouse-settings-lib"
"$work/mouse-settings-lib"
