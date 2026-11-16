#!/bin/bash
# Run the media-key rule on the host, under ASan and UBSan, beside a plain
# model. See tools/test_media_keys_host.c for what is asserted.
#
#   tools/test_media_keys_host.sh [random rounds, default 300]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include -I abi/include \
   kernel/src/audio/media_keys.c tools/test_media_keys_host.c -o "$work/test_media_keys"

"$work/test_media_keys" "${1:-300}"
