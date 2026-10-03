#!/bin/bash
# The verbs that change a finished document (DOM.md, D1), on the host under
# the sanitizers: cases by hand, every allocation failed in turn, pins and
# retirement, and a random walk against a second tree.
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
  -Iuserland/libhtml/include -Iuserland/libos64/include -Iabi/include \
  userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c \
  userland/libhtml/tree.c userland/libhtml/dom.c userland/libhtml/fragment.c userland/libhtml/serialize.c \
  tools/test_html_dom_host.c -o "$work/dom"
"$work/dom"
