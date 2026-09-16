#!/usr/bin/env bash
set -eu
cd "$(git rev-parse --show-toplevel)"
saved_test_dir=$(mktemp -d)
trap 'rm -rf "$saved_test_dir"' EXIT
mkdir -p "$saved_test_dir/include/os64" "$saved_test_dir/conf"
cat > "$saved_test_dir/include/os64/syscall.h" <<'HEADER'
#include <stdint.h>
#include "os64/syscall_numbers.h"
uint64_t os64_syscall2(uint64_t, uint64_t, uint64_t);
uint64_t os64_syscall3(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t os64_syscall6(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
HEADER
cc -pthread -std=c11 -g -O1 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -I "$saved_test_dir/include" -I userland/libos64/include -I abi/include \
    tools/test_appearance_saved_host.c userland/libos64/ui_saved.c userland/libos64/ui_theme.c \
    userland/libos64/ui_palette.c userland/libos64/conf.c userland/libos64/str.c userland/libos64/fmt.c \
    -Wl,--gc-sections -o "$saved_test_dir/test"
"$saved_test_dir/test" "$saved_test_dir/conf"
