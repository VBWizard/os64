#!/bin/bash
# Run libav's C-library shims (userland/libav/port/av_libc.c) on the host,
# under ASan and UBSan, against glibc: tools/test_av_libc_host.c says what
# is compared. av_libc.c includes no os64 header, which is what lets it
# build here unchanged. _DEFAULT_SOURCE and not _GNU_SOURCE: glibc's GNU
# mode reads strtol by C23's rules ("0b1" in base 2), and FFmpeg's sources
# are C17, which is what av_libc.c implements.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c17 -g -O1 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
   -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I userland/libav/port \
   tools/test_av_libc_host.c userland/libav/port/av_libc.c -lm -o "$work/test_av_libc"

"$work/test_av_libc"
