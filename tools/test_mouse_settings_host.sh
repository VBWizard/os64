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
mkdir -p "$work/include/os64"
cat > "$work/include/os64/syscall.h" <<'HEADER'
#include <stdint.h>
#include "os64/syscall_numbers.h"
uint64_t os64_syscall1(uint64_t, uint64_t);
uint64_t os64_syscall2(uint64_t, uint64_t, uint64_t);
uint64_t os64_syscall3(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t os64_syscall6(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
HEADER
cc -std=c11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$work/include" -I userland/libos64/include -I abi/include \
    tools/test_mouse_settings_lib_host.c userland/libos64/mouse_settings.c \
    userland/libos64/conf.c userland/libos64/slurp.c userland/libos64/str.c userland/libos64/fmt.c \
    -Wl,--gc-sections -o "$work/mouse-settings-lib"
"$work/mouse-settings-lib" "$work"
