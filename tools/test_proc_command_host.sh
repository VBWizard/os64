#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I userland/libos64/include -I abi/include \
    tools/test_proc_command_host.c userland/libos64/fmt.c userland/libos64/str.c \
    -o "$work/test_proc_command"
"$work/test_proc_command"
