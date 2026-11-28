#!/bin/bash
# libmedia's fragment parser (userland/libmedia/media_fragment.c) on the host
# under ASan and UBSan, beside the model in tools/test_media_fragment_host.c.
# Its mutants: tools/test_media_fragment_mutants.py. Its fuzzer:
# tools/fuzz_media_fragment_host.sh. Real streams decoded:
# tools/test_media_fragment_decode_host.sh.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libmedia/include \
   tools/test_media_fragment_host.c userland/libmedia/media_fragment.c -o "$work/test_media_fragment"

"$work/test_media_fragment"
