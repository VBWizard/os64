#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
work=${XML_TREE_TEST_WORK:-$(mktemp -d)}
mkdir -p "$work"
trap 'status=$?; if [ "$status" -eq 0 ] && [ -z "${XML_TREE_TEST_WORK:-}" ]; then rm -rf "$work"; else echo "XML tree artifacts: $work" >&2; fi' EXIT
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}"
export CCACHE_DISABLE=1
. tools/host_build.sh
host_flags=(-std=c11 -g -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
    -fno-sanitize-recover=all -fno-builtin -fno-tree-loop-distribute-patterns
    -Iuserland/libxml/include -Iuserland/libhtml/include -Iuserland/libgarb/include
    -Iuserland/libos64/include -Iabi/include)
host_sources=(tools/test_xml_tree_host.c userland/libxml/xml.c userland/libhtml/xml.c
    userland/libhtml/core.c userland/libhtml/encoding.c userland/libhtml/tokenizer.c
    userland/libhtml/tree.c userland/libhtml/dom.c userland/libhtml/fragment.c userland/libhtml/serialize.c
    userland/libgarb/tokenize.c userland/libgarb/parse.c userland/libgarb/decode.c
    userland/libgarb/dump.c userland/libgarb/select.c userland/libgarb/values.c
    userland/libgarb/props.c userland/libgarb/media.c userland/libgarb/cascade.c
    userland/libos64/str.c userland/libos64/fmt.c userland/libos64/arena.c)
host_objects "$work"
cc -fsanitize=address,undefined "${host_objs[@]}" -o "$work/xml_tree"
timeout 90s "$work/xml_tree"
