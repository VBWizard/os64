#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
js_runtime_work=$(mktemp -d)
trap 'rm -rf "$js_runtime_work"' EXIT
make -C userland -j8 js-runtime-test > "$js_runtime_work/build.log" 2>&1 || { cat "$js_runtime_work/build.log"; exit 1; }
flags=(-O2 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    -I userland/libmath/include -I userland/libjs/port -I userland/libjs/include
    -I userland/libos64/include -I abi/include -isystem userland/obj/js/upstream
    -fsanitize=address,undefined -fno-sanitize-recover=all)
for source in userland/libjs/runtime/runtime.c userland/libos64/str.c userland/libos64/fmt.c \
    tools/test_js_port_calendar.c tools/test_js_runtime_host.c; do
    cc "${flags[@]}" -c "$source" -o "$js_runtime_work/$(basename "$source" .c).o"
done
objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
    --localize-symbol=memcmp "$js_runtime_work/str.o" "$js_runtime_work/str-local.o"
# Execute the actual target engine, adapters, compiler helpers and M1 maths.
# Only heap and syscall behavior are host fixtures; this does not prove a guest.
mapfile -t maths < <(make -s -C userland math-host-flags | python3 -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack userland/obj/js/core.o \
    "${maths[@]}" "$js_runtime_work/runtime.o" "$js_runtime_work/str-local.o" \
    "$js_runtime_work/fmt.o" "$js_runtime_work/test_js_port_calendar.o" \
    "$js_runtime_work/test_js_runtime_host.o" -o "$js_runtime_work/probe"
# Also instrument the engine: ASan bypasses its small-object arenas, giving
# the constructor sweep independent coverage of individual allocations.
engine_flags=("${flags[@]}" -U__linux__ -I userland/libjs/port/compat
    '-DCONFIG_VERSION="2026-06-04"' -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers -Wno-implicit-fallthrough)
for name in quickjs dtoa libregexp libunicode cutils; do
    cc "${engine_flags[@]}" -fno-sanitize=undefined \
        -c "userland/obj/js/upstream/$name.c" -o "$js_runtime_work/$name.o"
done
for name in allocator platform format; do
    cc "${flags[@]}" -c "userland/libjs/port/$name.c" -o "$js_runtime_work/$name.o"
done
cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack \
    "$js_runtime_work/quickjs.o" "$js_runtime_work/dtoa.o" "$js_runtime_work/libregexp.o" \
    "$js_runtime_work/libunicode.o" "$js_runtime_work/cutils.o" \
    "$js_runtime_work/allocator.o" "$js_runtime_work/platform.o" "$js_runtime_work/format.o" \
    "${maths[@]}" "$js_runtime_work/runtime.o" "$js_runtime_work/str-local.o" \
    "$js_runtime_work/fmt.o" "$js_runtime_work/test_js_port_calendar.o" \
    "$js_runtime_work/test_js_runtime_host.o" -o "$js_runtime_work/sanitized-probe"
for probe in probe sanitized-probe; do
    "$js_runtime_work/$probe"
    # This arena-allocation index is a separate regression for the actual
    # target core; the sanitized sweep measures its own larger inventory.
    if [ "$probe" = probe ]; then "$js_runtime_work/$probe" --raw-construction; fi
    for mode in --leak --active-destroy; do
        set +e
        "$js_runtime_work/$probe" "$mode" > "$js_runtime_work/fatal.out" 2> "$js_runtime_work/fatal.err"
        result=$?
        set -e
        test "$result" -eq 99 || { cat "$js_runtime_work/fatal.err"; exit 1; }
        grep -q 'libjs: engine invariant failure:' "$js_runtime_work/fatal.err"
        grep -q 'target exit badge: 4a534641' "$js_runtime_work/fatal.err"
    done
done
printf 'Runtime invariant paths: PASS (host hook verified full os64 badge)\n'
