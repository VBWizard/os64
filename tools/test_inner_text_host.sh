#!/bin/bash
# innerText's walk (userland/libdom/inner_text.c) on the host, under the
# sanitizers, against libhtml's own parse and a provider of the test's own
# (WHEEL_FILELIST_INNERTEXT.md § 3.5). An argument sets the deep case's
# depth (default 50,000 levels).
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
  -Iuserland/libdom -Iuserland/libdom/include -Iuserland/libhtml/include -Iuserland/libos64/include -Iabi/include \
  userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c userland/libhtml/tree.c \
  userland/libhtml/dom.c userland/libhtml/fragment.c userland/libhtml/serialize.c userland/libhtml/shadow.c \
  userland/libhtml/range.c userland/libdom/inner_text.c tools/test_inner_text_host.c -o "$work/inner_text"
"$work/inner_text" "$@"
