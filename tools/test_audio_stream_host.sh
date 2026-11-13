#!/bin/bash
# Run the audio stream's arithmetic on the host, under ASan and UBSan, beside
# a plain model. See tools/test_audio_stream_host.c for what is asserted.
#
#   tools/test_audio_stream_host.sh [random rounds, default 300]

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include -I abi/include \
   kernel/src/audio/audio_stream.c tools/test_audio_stream_host.c -o "$work/test_audio_stream"

"$work/test_audio_stream" "${1:-300}"
