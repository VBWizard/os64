#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
js_port_work=$(mktemp -d)
trap 'rm -rf "$js_port_work"' EXIT
make -C userland -j8 js-core > "$js_port_work/cross-build.log" 2>&1
python3 tools/js_prepare.py "$js_port_work/upstream"
base_flags=(-O2 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    '-DCONFIG_VERSION="2026-06-04"'
    -I userland/libmath/include -I userland/libjs/port -I userland/libjs/include -I userland/libos64/include
    -I abi/include -isystem "$js_port_work/upstream")
for name in quickjs dtoa libregexp libunicode cutils; do
    cc "${base_flags[@]}" -U__linux__ -I userland/libjs/port/compat -fsanitize=address \
        -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-implicit-fallthrough \
        -c "$js_port_work/upstream/$name.c" -o "$js_port_work/$name.o"
done
# Adapter and library helpers keep strict diagnostics and both sanitizers.
for source in userland/libjs/port/allocator.c userland/libjs/port/platform.c \
    userland/libjs/port/format.c userland/libos64/str.c userland/libos64/fmt.c \
    tools/test_js_port_calendar.c tools/test_js_port_host.c; do
    cc "${base_flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all \
        -c "$source" -o "$js_port_work/$(basename "$source" .c).o"
done
cc -fsanitize=address,undefined "$js_port_work/"*.o -lm -o "$js_port_work/probe"
"$js_port_work/probe"
# Exercise the actual cross-built engine, adapter and libgcc helpers on x86-64.
# Host libm and syscall/heap fixtures remain substitutes; this is not a guest.
# Separate DSOs can carry their own hidden memory aliases. Localize the
# fixture library's aliases when both copies share this host executable.
objcopy --localize-symbol=memcpy --localize-symbol=memmove \
    --localize-symbol=memset --localize-symbol=memcmp \
    "$js_port_work/str.o" "$js_port_work/str-local.o"
cc -fsanitize=address,undefined -Wl,-z,noexecstack userland/obj/js/core.o \
    "$js_port_work/test_js_port_host.o" "$js_port_work/test_js_port_calendar.o" \
    "$js_port_work/str-local.o" "$js_port_work/fmt.o" -lm -o "$js_port_work/cross-probe"
"$js_port_work/cross-probe"
printf 'Cross-built core executed on host (host maths/syscalls): PASS\n'
for probe in probe cross-probe; do
    for mode in --leak --clock-failure --zero-allocation; do
        set +e
        "$js_port_work/$probe" "$mode" > "$js_port_work/fatal.out" 2> "$js_port_work/fatal.err"
        status=$?
        set -e
        test "$status" -eq 99
        grep -q 'libjs: engine invariant failure:' "$js_port_work/fatal.err"
        grep -q 'target exit badge: 4a534641' "$js_port_work/fatal.err"
        if [ "$mode" = --zero-allocation ]; then
            grep -q 'engine invariant failure: zero-byte allocation' "$js_port_work/fatal.err"
        fi
    done
done
printf 'Target fatal paths: PASS (host hook verified full os64 badge)\n'
