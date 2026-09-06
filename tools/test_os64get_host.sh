#!/bin/bash
# Compile the production app with host I/O adapters and exercise batch failures.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=c11 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
   -fsanitize=address,undefined -fno-pie -no-pie \
   -I userland/libos64/include -I abi/include -I userland/libgzip/include \
   tools/test_os64get_host.c userland/apps/os64get/install.c userland/apps/os64get/http.c \
   userland/libos64/{str,fmt,args,date,crc32,url}.c userland/libgzip/{gzip,inflate,deflate}.c \
   -Wl,--gc-sections,--wrap=os64_time -o "$work/os64get-test"
for scenario in success absent unchanged force-identical no-archive single url url-https url-archive-blocked url-short url-cancel short crc \
                backup-read backup-write backup-corrupt sync close publish aliases appeared unsafe-name archive-overlap \
                cancel-list cancel-download cancel-backup cancel-verify cancel-commit cancel-cleanup; do
    mkdir "$work/$scenario"
    ASAN_OPTIONS=detect_leaks=0 "$work/os64get-test" "$scenario" "$work/$scenario"
done
