#!/bin/bash
# Drive the floating-point register image's rules on the host, under ASan
# and UBSan: the 832-byte layout, the sanitizer every restore goes through,
# the reset image a thread is born with, and the boot's mode decision
# (kernel/include/fpu_image.h; AVX.md § The rules).
#
#   tools/test_fpu_image_host.sh

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include \
   kernel/src/fpu_image.c tools/test_fpu_image_host.c -o "$work/test_fpu_image"

"$work/test_fpu_image"
