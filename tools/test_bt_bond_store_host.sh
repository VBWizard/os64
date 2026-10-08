#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror -masm=intel \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/memory -I abi/include \
    tools/test_bt_bond_store_host.c -o "$work/bt-bond-store"
"$work/bt-bond-store"
