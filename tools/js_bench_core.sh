#!/usr/bin/env bash
# js_bench_core.sh — build libjs's engine core as core.mk does (the cross
# compiler, libjs's own flags and sources, one `ld -r` object), less the
# flags named, for the JS benchmark host twin's `--core` experiment
# (tools/js_bench_host.sh).
#
#   tools/js_bench_core.sh OUT FLAG...    e.g. OUT -fno-builtin
#   JSB_CORE_CC=cc tools/js_bench_core.sh OUT     the host's compiler instead
#
# Each FLAG is removed from libjs's flags (LIBJS_FLAGS, read from the
# makefile so it cannot drift from what os64 builds with); nothing is added.
# JSB_CORE_CC names another compiler (and its libgcc) for the same sources
# and flags, which is how a compiler's own version is measured.
# The prepared sources (patched upstream) must exist: build
# userland/bin/libjs.so in this tree first.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
out=$(realpath -m "${1:?usage: tools/js_bench_core.sh OUT FLAG...}")
shift
# Each setting is tagged: the makefile prints a banner of its own first.
settings=$(make -s -C userland --no-print-directory \
    --eval 'jsbcore: ; @echo "JSB_CC=$(CC)"; echo "JSB_FLAGS=$(LIBJS_FLAGS) $(LIBJS_UPSTREAM_WARNINGS)"; echo "JSB_GEN=$(LIBJS_GENERATED)"' jsbcore)
cc_cmd=${JSB_CORE_CC:-$(sed -n 's/^JSB_CC=//p' <<< "$settings")}
read -r -a flags <<< "$(sed -n 's/^JSB_FLAGS=//p' <<< "$settings")"
generated=$(sed -n 's/^JSB_GEN=//p' <<< "$settings")
[ -f "$generated/.prepared" ] || { echo "js_bench_core: $generated is not prepared (make userland/bin/libjs.so first)" >&2; exit 2; }
kept=()
for f in "${flags[@]}"; do
    drop=0
    for r in "$@" -MMD -MP; do [ "$f" = "$r" ] && drop=1; done
    [ "$drop" = 0 ] && kept+=("$f")
done
work=$(mktemp -d -t os64-js-core.XXXXXX)
trap 'rm -rf "$work"' EXIT
cd userland
for name in quickjs dtoa libregexp libunicode cutils; do
    $cc_cmd "${kept[@]}" -c "$generated/$name.c" -o "$work/$name.o"
done
for name in allocator platform format memory; do
    $cc_cmd "${kept[@]}" -c "libjs/port/$name.c" -o "$work/port-$name.o"
done
# core.mk's order (LIBJS_CORE_NAMES, then LIBJS_PORT_NAMES), so the code is
# laid out as in core.o and only the flags differ.
ld -r -o "$out" "$work"/{quickjs,dtoa,libregexp,libunicode,cutils}.o \
    "$work"/port-{allocator,platform,format,memory}.o "$($cc_cmd -print-libgcc-file-name)"
echo "js_bench_core: built $out without: $*"
