#!/bin/bash
# Run the Classic Bluetooth link on the host, under ASan and UBSan, against
# a model headphone. See tools/test_bt_classic_host.c for what is asserted.
#
#   tools/test_bt_classic_host.sh [random rounds, default 2000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_classic.c tools/test_bt_classic_host.c -o "$work/test_bt_classic"

"$work/test_bt_classic" "${1:-2000}"
