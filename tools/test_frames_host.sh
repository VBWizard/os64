#!/bin/bash
# The frame table (kernel/src/memory/frames.c) on the host, under ASan and
# UBSan, with hand cases and a differential run against a reference model.
# FRAMES.md § How it gets built. Extra arguments are seeds; each one gets a
# full run (the default seed always runs).

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=all -I kernel/include/memory \
   kernel/src/memory/frames.c tools/test_frames_host.c -o "$work/test_frames"

# The kernel builds this file freestanding, without SSE and without libc;
# compile it that way here too, so a host-only dependency fails on the host.
cc -std=c11 -ffreestanding -fno-builtin -nostdlib -mno-sse -mno-red-zone \
   -Wall -Wextra -Werror -I kernel/include/memory \
   -c kernel/src/memory/frames.c -o "$work/frames_freestanding.o"

"$work/test_frames"
for s in "$@"; do
	"$work/test_frames" "$s"
done
