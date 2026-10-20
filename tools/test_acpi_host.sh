#!/bin/bash
# Drive the ACPI table walker on the host, under ASan and UBSan.
#
# Every firmware read goes through a `map` that hands back a heap block of
# exactly the bytes asked for, so "read a table it never mapped" — the bug
# that killed the first boot on a 3900X — is a sanitizer report here. See
# tools/test_acpi_host.c for the machines it builds and breaks.
#
#   tools/test_acpi_host.sh [damaged-machine rounds, default 20000]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include/driver/system \
   kernel/src/driver/system/acpi_tables.c tools/test_acpi_host.c -o "$work/test_acpi"

"$work/test_acpi" "${1:-20000}"
