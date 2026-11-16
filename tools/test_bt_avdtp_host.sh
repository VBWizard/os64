#!/bin/bash
# Run AVDTP's signalling on the host, under ASan and UBSan, against a model
# sink that builds every message from the specification. See
# tools/test_bt_avdtp_host.c for what is asserted.
#
#   tools/test_bt_avdtp_host.sh [fuzz rounds, default 3000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_avdtp.c tools/test_bt_avdtp_host.c -o "$work/test_bt_avdtp"

"$work/test_bt_avdtp" "${1:-3000}"
