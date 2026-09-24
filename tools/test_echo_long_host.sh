#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libos64/include -I abi/include tools/test_echo_long_host.c \
   userland/libos64/str.c -o "$work/echo-long"
"$work/echo-long"
echo 'echo long arguments, escaping, partial writes and failure PASS'
