#!/bin/bash
# The thread name rule on the host, under ASan and UBSan
# (abi/include/os64/thread_name.h; tools/test_thread_name_host.c).
#
#   tools/test_thread_name_host.sh

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I abi/include \
   tools/test_thread_name_host.c -o "$work/test_thread_name"

"$work/test_thread_name"
