#!/usr/bin/env bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-cut-host.XXXXXX)
trap 'rm -rf "$work"' EXIT
flags=(-std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-builtin
       -fsanitize=address,undefined -fno-sanitize-recover=all -fno-pie
       -I userland/libos64/include -I abi/include)
cc "${flags[@]}" -Dmain=cut_main -c userland/apps/cut/cut.c -o "$work/cut.o"
for source in userland/libos64/args.c userland/libos64/fmt.c userland/libos64/str.c \
              tools/test_cut_host.c; do
    cc "${flags[@]}" -c "$source" -o "$work/$(basename "$source" .c).o"
done
cc -no-pie -fsanitize=address,undefined "$work"/*.o -o "$work/cut"
max_args=$(printf '#include "os64/syscall_numbers.h"\nOS64_SPAWN_MAX_ARGS\n' |
    cc -E -P -I abi/include -x c - | tail -n 1)
python3 tools/test_cut_host.py "$work/cut" "$max_args"
