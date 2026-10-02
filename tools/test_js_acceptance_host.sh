#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
js_acceptance_work=$(mktemp -d)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ]; then rm -rf "$js_acceptance_work";
    else echo "JavaScript acceptance artifacts: $js_acceptance_work" >&2; fi
}
trap cleanup EXIT
make -s -C userland -j8 js-acceptance-test
flags=(-O2 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    -DJS_ACCEPTANCE_HOST -I userland/libmath/include -I userland/libjs/include
    -I userland/libjs/port -I userland/libos64/include -I abi/include
    -I userland/obj/js/acceptance -isystem userland/obj/js/upstream
    -fsanitize=address,undefined -fno-sanitize-recover=all)
for source in tools/js_acceptance/consumer.c tools/js_acceptance/host.c \
    userland/libjs/runtime/runtime.c userland/libos64/str.c userland/libos64/fmt.c \
    tools/test_js_port_calendar.c; do
    cc "${flags[@]}" -c "$source" -o "$js_acceptance_work/$(basename "$source" .c).o"
done
objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
    --localize-symbol=memcmp "$js_acceptance_work/str.o" "$js_acceptance_work/str-local.o"
rm "$js_acceptance_work/str.o"
mapfile -t maths < <(make -s -C userland --no-print-directory math-host-flags | python3 -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
cc -fsanitize=address,undefined -Wl,-z,noexecstack userland/obj/js/core.o \
    "${maths[@]}" "$js_acceptance_work"/*.o -o "$js_acceptance_work/consumer"
# A second engine build instruments individual accesses and bypasses QuickJS's
# small-object arenas. It exercises the same consumer against different heap
# allocation paths; target execution remains the first executable's evidence.
mkdir "$js_acceptance_work/engine"
engine_flags=("${flags[@]}" -U__linux__ -I userland/libjs/port/compat
    '-DCONFIG_VERSION="2026-06-04"' -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers -Wno-implicit-fallthrough)
for name in quickjs dtoa libregexp libunicode cutils; do
    cc "${engine_flags[@]}" -fno-sanitize=undefined \
        -c "userland/obj/js/upstream/$name.c" -o "$js_acceptance_work/engine/$name.o"
done
for name in allocator platform format; do
    cc "${flags[@]}" -c "userland/libjs/port/$name.c" -o "$js_acceptance_work/engine/$name.o"
done
cc -fsanitize=address,undefined -Wl,-z,noexecstack \
    "${maths[@]}" "$js_acceptance_work"/*.o "$js_acceptance_work"/engine/*.o \
    -o "$js_acceptance_work/sanitized-consumer"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}"
for probe in consumer sanitized-consumer; do
    if readelf -dW "$js_acceptance_work/$probe" | grep -q 'NEEDED.*libm\.so'; then
        echo 'FAIL: acceptance linked host libm' >&2; exit 1
    fi
    echo "Acceptance executable: $probe"
    "$js_acceptance_work/$probe"
    "$js_acceptance_work/$probe" --diagnostic-oom
    set +e
    "$js_acceptance_work/$probe" --fatal-leak > "$js_acceptance_work/fatal.out" 2> "$js_acceptance_work/fatal.err"
    result=$?
    set -e
    test "$result" -eq 99
    grep -q 'libjs: engine invariant failure:' "$js_acceptance_work/fatal.err"
    grep -q 'acceptance target exit: 4a534641' "$js_acceptance_work/fatal.err"
    echo 'Separate fatal process: PASS (full os64 exit badge verified by host hook)'
done
