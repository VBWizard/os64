#!/usr/bin/env bash
# libjs Intl's segmentation against Unicode's own break tests, under ASan and
# UBSan (tools/test_intl_segment_host.c says what is checked). segment.c is
# pure, so it compiles here alone with the host's cc.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-intl-segment.XXXXXX)
trap 'rm -rf "$work"' EXIT
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    userland/libjs/intl/segment.c tools/test_intl_segment_host.c -o "$work/segment"
"$work/segment" tools/unicode/segmentation
