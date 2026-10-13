#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

"${CC:-cc}" -std=c11 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/abi/include" \
    -I"$repo/userland/libimage/include" \
    -I"$repo/userland/libos64/include" \
    -I"$repo/userland/libpng/include" \
    "$repo/userland/libimage/icon_theme.c" \
    "$repo/userland/libimage/tests/icon_theme_test.c" \
    -o "$build/icon_theme_test"

ASAN_OPTIONS=detect_leaks=1 "$build/icon_theme_test"
