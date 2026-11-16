#!/bin/bash
# Drive the xHCI event-ring service logic on the host, under ASan and UBSan,
# against a model of controllers that stamps its events and records every
# ERDP write. See tools/test_xhci_service_host.c for what is asserted.
#
#   tools/test_xhci_service_host.sh [random rounds, default 2000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/driver/system/usb/xhci_service.c tools/test_xhci_service_host.c -o "$work/test_xhci_service"

"$work/test_xhci_service" "${1:-2000}"
