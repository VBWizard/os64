#!/bin/bash
# Run the controller's ACL buffer accounting on the host, under ASan and
# UBSan, beside a plain model. See tools/test_bt_acl_host.c for what is
# asserted.
#
#   tools/test_bt_acl_host.sh [random rounds, default 2000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I kernel/include \
   kernel/src/driver/system/usb/bt_acl.c tools/test_bt_acl_host.c -o "$work/test_bt_acl"

"$work/test_bt_acl" "${1:-2000}"
