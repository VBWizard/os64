#!/bin/bash
# Cross-check os64's pure ustar codec with the host tar in both directions.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror \
   -I userland/apps/tar \
   userland/apps/tar/tar_format.c tools/test_tar_format_host.c \
   -o "$work/test_tar_format"

"$work/test_tar_format"

"$work/test_tar_format" --write "$work/os64.tar"
tar -tf "$work/os64.tar" > "$work/os64.list"
grep -qx 'tree' "$work/os64.list"
grep -qx 'tree/hello.txt' "$work/os64.list"
test "$(tar -xOf "$work/os64.tar" tree/hello.txt)" = 'hello os64'
echo "os64 headers and payload accepted by host tar             PASS"

mkdir -p "$work/source/tree/sub"
printf 'from the host\n' > "$work/source/tree/sub/message.txt"
tar --format=ustar -cf "$work/host.tar" -C "$work/source" tree
"$work/test_tar_format" --read "$work/host.tar" > "$work/host.list"
grep -Eq '^5 0 tree/?$' "$work/host.list"
grep -Eq '^5 0 tree/sub/?$' "$work/host.list"
grep -q '^0 14 tree/sub/message.txt$' "$work/host.list"
echo "host ustar headers accepted by os64 decoder               PASS"
