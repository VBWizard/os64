#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -Iabi/include -Iuserland/libos64/include -Iuserland/libos64 -Itools/fonts \
    tools/test_decoration_host.c tools/fonts/fake_backend.c shared/decoration.c \
    userland/apps/framestudio/model.c userland/apps/framestudio/storage.c tools/frame_storage_host.c \
    userland/libos64/decoration_prepare.c userland/libos64/decoration_startup.c userland/libos64/arena.c \
    userland/libos64/font_provider.c userland/libos64/text.c userland/libos64/text_cache.c \
    userland/libos64/text_decode.c userland/libos64/text_bitmap.c userland/libos64/text_draw.c \
    userland/libos64/str.c userland/libos64/fmt.c -o "$work/test_decoration"
"$work/test_decoration"
