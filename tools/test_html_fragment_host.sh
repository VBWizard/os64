#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d /tmp/os64-html-fragment.XXXXXX)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iuserland/libhtml/include -Iuserland/libos64/include -Iabi/include \
    userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
    userland/libhtml/tree.c userland/libhtml/dom.c userland/libhtml/fragment.c \
    userland/libhtml/serialize.c tools/test_html_driver.c -o "$work/html_driver"
"$work/html_driver" --fragments
