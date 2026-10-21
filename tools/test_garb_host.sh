#!/bin/bash
# libgarb's parser on the host, under the sanitizers (GARB.md § Slices, G1):
# css-parsing-tests through the driver, then every allocation failed in turn
# over a real 2026 sheet and through every entry point, then a million
# nested ( on a small stack, numbers past int64_t's reach, and what a parse
# costs (GARB.md § The cost): a 4 MiB sheet whole, one at the cap kept, and
# @keyframes at scale held to a sort's cost; then the Modern Web arc's mark,
# on the names GARB_MODERN_PROBE marks for the purpose.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "libgarb test artifacts: $work" >&2; fi
}
trap cleanup EXIT

# libos64's str.c defines memset itself, as the freestanding build needs:
# without -fno-builtin and -fno-tree-loop-distribute-patterns an optimizing
# host compiler may turn os64_memset's loop into a call to memset — that
# memset — and recurse until the stack or the machine runs out.
# float-cast-overflow is not in GCC's `undefined`: a double cast to an
# integer it does not fit is the undefined behaviour a number parser invites.
. tools/host_build.sh
host_flags=(-std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined,float-cast-overflow
   -fno-sanitize-recover=all -fno-builtin -fno-tree-loop-distribute-patterns
   -I userland/libgarb/include -I userland/libgarb -I userland/libhtml/include
   -I userland/libos64/include -I userland -I abi/include
   '-DGARB_MODERN_PROBE="animation-name","grid-gap","-webkit-transform"')
host_sources=(tools/test_garb_host.c
   userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c
   userland/libgarb/dump.c userland/libgarb/select.c
   userland/libgarb/values.c userland/libgarb/props.c
   userland/libgarb/media.c userland/libgarb/cascade.c
   userland/libhtml/core.c userland/libhtml/encoding.c
   userland/libhtml/tokenizer.c userland/libhtml/tree.c userland/libhtml/dom.c
   userland/libhtml/fragment.c userland/libhtml/serialize.c
   userland/libos64/str.c userland/libos64/fmt.c userland/libos64/arena.c)
host_objects "$work"
cc -fsanitize=address,undefined,float-cast-overflow "${host_objs[@]}" -o "$work/garb_driver"

python3 tools/test_garb_suite.py "$work/garb_driver"
"$work/garb_driver" --sweep tools/garb_corpus/sweep.css
"$work/garb_driver" --deep
"$work/garb_driver" --numbers
"$work/garb_driver" --cost 4194304
"$work/garb_driver" --full
"$work/garb_driver" --keyframes-scale
"$work/garb_driver" --faces-cap
"$work/garb_driver" --modern
