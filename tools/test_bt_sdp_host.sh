#!/bin/bash
# Run the SDP client and responder on the host, under ASan and UBSan,
# against a model headphone's server and a model client. See
# tools/test_bt_sdp_host.c for what is asserted.
#
#   tools/test_bt_sdp_host.sh [fuzz rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_sdp.c tools/test_bt_sdp_host.c -o "$work/test_bt_sdp"

"$work/test_bt_sdp" "${1:-3000}"
