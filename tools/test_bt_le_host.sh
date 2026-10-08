#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I kernel/include \
    tools/test_bt_le_host.c -lcrypto -o "$work/bt-le"
"$work/bt-le"
