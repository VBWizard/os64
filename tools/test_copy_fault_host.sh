#!/bin/bash
# The user copy's fault rule on the host, under ASan and UBSan
# (kernel/src/copy_fault.c; tools/test_copy_fault_host.c).
#
#   tools/test_copy_fault_host.sh

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -include stdbool.h \
   -I kernel/include \
   tools/test_copy_fault_host.c kernel/src/copy_fault.c -o "$work/test_copy_fault"

"$work/test_copy_fault"
