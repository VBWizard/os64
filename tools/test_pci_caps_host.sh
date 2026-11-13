#!/bin/bash
# Drive the PCI capability-list walker on the host, under ASan and UBSan.
#
# The list is read from hardware, so the walk is fed lists that point into
# the header, loop, and fill configuration space, then random ones. See
# tools/test_pci_caps_host.c.
#
#   tools/test_pci_caps_host.sh [random configuration spaces, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include/driver/system \
   kernel/src/driver/system/pci_caps.c tools/test_pci_caps_host.c -o "$work/test_pci_caps"

"$work/test_pci_caps" "${1:-20000}"
