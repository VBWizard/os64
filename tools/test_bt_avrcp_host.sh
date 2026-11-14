#!/bin/bash
# Run AVCTP and AVRCP on the host, under ASan and UBSan, against model
# headphones that build every message from the specification. See
# tools/test_bt_avrcp_host.c for what is asserted.
#
#   tools/test_bt_avrcp_host.sh [fuzz rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_avrcp.c tools/test_bt_avrcp_host.c -o "$work/test_bt_avrcp"

"$work/test_bt_avrcp" "${1:-3000}"
