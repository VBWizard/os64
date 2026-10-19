#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-pie -no-pie \
   -fsanitize=address,undefined -I userland/libos64/include -I abi/include \
   userland/apps/sort/sort.c userland/libos64/args.c tools/sort_host_support.c \
   -o "$work/sort"
python3 tools/test_sort_host.py "$work/sort"
