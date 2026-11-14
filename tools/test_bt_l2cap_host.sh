#!/bin/bash
# Run Classic L2CAP on the host, under ASan and UBSan, against a model peer
# that checks every PDU it is sent byte for byte. See
# tools/test_bt_l2cap_host.c for what is asserted.
#
#   tools/test_bt_l2cap_host.sh [fuzz rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_l2cap.c tools/test_bt_l2cap_host.c -o "$work/test_bt_l2cap"

"$work/test_bt_l2cap" "${1:-3000}"
