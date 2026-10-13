#!/bin/bash
# Exercise the installed PNGs through the real theme, config parser, catalog,
# and PNG decoder while replacing only the kernel's config-path resolution.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

assets=${ICON_ASSET_ROOT:-assets/icons/default}
for name in folder file drive; do
    test -f "$assets/$name-24.png"
done
mkdir -p "$work/default" "$work/selected"
cp "$assets/folder-24.png" "$assets/file-24.png" "$work/default/"
cp "$assets/drive-24.png" "$work/selected/folder-24.png"
printf 'format = 1\nname = Shipped fixture\nauthor = os64\n' > "$work/default/theme.conf"
printf 'format = 1\nname = Selected fixture\nauthor = Test\n' > "$work/selected/theme.conf"
printf 'set = %s/selected\n' "$work" > "$work/icons.conf"

common=(-std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -I userland/libimage/include -I userland/libos64/include
    -I userland/libpng/include -I userland/libgzip/include -I abi/include)
cc "${common[@]}" -Dos64_conf_get=os64_conf_get_unused \
    -c userland/libos64/conf.c -o "$work/conf.o"
cc "${common[@]}" "-DOS64_ICON_DEFAULT_ROOT=\"$work/default\"" \
    userland/libimage/icon_theme.c userland/libimage/icons.c \
    userland/libos64/ui_icon.c \
    userland/libpng/png.c userland/libgzip/inflate.c \
    userland/libos64/crc32.c tools/test_icons_theme_e2e_host.c "$work/conf.o" \
    -Wl,--gc-sections -o "$work/test_icons_theme"
ASAN_OPTIONS=detect_leaks=1 "$work/test_icons_theme" "$work"
