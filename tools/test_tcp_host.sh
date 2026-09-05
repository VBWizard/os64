#!/bin/bash
# Deterministic sender transitions, with ASan and UBSan.
set -eu
cd "$(git rev-parse --show-toplevel)"
tcp_test_dir=$(mktemp -d)
trap 'rm -rf "$tcp_test_dir"' EXIT
python3 tools/test_tcp_host.py "$tcp_test_dir"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-variable \
   -fsanitize=address,undefined "$tcp_test_dir/harness.c" -o "$tcp_test_dir/test"
"$tcp_test_dir/test"
