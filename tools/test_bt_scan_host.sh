#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -I kernel/include \
    tools/test_bt_scan_host.c kernel/src/driver/system/usb/bt_scan.c \
    kernel/src/driver/system/usb/bt_intel.c -o "$work/bt-scan"
"$work/bt-scan"
