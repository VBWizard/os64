#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd kernel && cc -c src/driver/net/ax210_firmware.S -o "$work/firmware.o")
cc -std=c11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include tools/test_ax210_fw_host.c \
    kernel/src/driver/net/ax210_fw.c "$work/firmware.o" -o "$work/ax210-fw"
"$work/ax210-fw"
cc -std=gnu11 -D_POSIX_C_SOURCE=200809L -fno-builtin-strnlen -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I kernel/include -I kernel/include/driver/system -I kernel/include/memory \
    -I abi/include tools/test_ax210_boot_host.c \
    kernel/src/driver/net/ax210.c kernel/src/driver/net/ax210_boot.c \
    kernel/src/driver/net/ax210_fw.c "$work/firmware.o" -o "$work/ax210-boot"
for scenario in p5-pnvm cold d3 pending clock-timeout early-stop shutdown bad-token unsafe-stop \
    bad-pnvm command-reject early-init stale-reply coex-reject coex-timeout msix msix-fault; do
    "$work/ax210-boot" "$scenario"
done
