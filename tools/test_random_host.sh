#!/bin/bash
# The entropy pool's primitives against their references, with ASan and UBSan.
set -eu
cd "$(dirname "$0")/.."
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
python3 tools/test_random_host.py "$dir/vectors.h"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I kernel/include -DVECTORS_HEADER="\"$dir/vectors.h\"" \
   kernel/src/crypto/blake2s.c kernel/src/crypto/chacha20.c tools/test_random_host.c \
   -o "$dir/test"
ASAN_OPTIONS=detect_leaks=0 "$dir/test"
# The pool itself, hardware replaced by hooks (tools/test_random_pool_host.c).
python3 tools/test_random_host.py --pool "$dir/pool.c"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
   -I kernel/include kernel/src/crypto/blake2s.c kernel/src/crypto/chacha20.c "$dir/pool.c" \
   -o "$dir/pool"
ASAN_OPTIONS=detect_leaks=0 "$dir/pool"
