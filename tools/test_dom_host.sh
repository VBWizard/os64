#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d /tmp/os64-dom-host.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${DOM_HOST_KEEP:-0}" = 0 ]; then rm -rf "$work"; else echo "DOM host artifacts: $work" >&2; fi
}
trap cleanup EXIT
for name in core node collection content event timer window; do
    test -f "userland/libdom/$name.c" || { echo "libdom implementation pending: $name.c" >&2; exit 1; }
done
make -C userland -j8 js-runtime-test > "$work/build.log" 2>&1 || { cat "$work/build.log"; exit 1; }
flags=(-O2 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    -Iuserland/libmath/include -Iuserland/libjs/port -Iuserland/libjs/include
    -Iuserland/libdom/include -Iuserland/libhtml/include -Iuserland/libpage/include
    -Iuserland/libpage/upstream/ryu -Iuserland/libos64/include -Iabi/include
    -isystem userland/obj/js/upstream -fsanitize=address,undefined -fno-sanitize-recover=all)
objects=()
for source in userland/libjs/runtime/runtime.c userland/libos64/str.c userland/libos64/fmt.c \
    userland/libos64/bidi.c userland/libos64/url.c tools/test_js_port_calendar.c tools/test_dom_host.c \
    userland/libhtml/{core,encoding,tokenizer,tree,dom,fragment,serialize}.c \
    userland/libpage/{core,state,resolve,value,submit,encode,refresh,activate,number,range}.c \
    userland/libpage/upstream/ryu/ryu/d2s.c; do
    object="$work/${source//\//_}.o"
    cc "${flags[@]}" -c "$source" -o "$object"
    if [ "$source" = userland/libos64/str.c ]; then
        objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
            --localize-symbol=memcmp "$object" "$object-local"
        object="$object-local"
    fi
    objects+=("$object")
done
# Source-local interception counts binding allocations without attributing
# engine or tree allocations to the binding's own failure sweep. The private
# allocator name is distinct because these DSOs share one host executable.
for name in core node collection content event timer window; do
    object="$work/dom-$name.o"
    cc "${flags[@]}" -Dd_alloc=dom_host_private_alloc -Dos64_malloc=dom_host_malloc -Dos64_calloc=dom_host_calloc \
        -Dos64_realloc=dom_host_realloc -Dos64_free=dom_host_free \
        -Dos64_malloc_size=dom_host_malloc_size -c "userland/libdom/$name.c" -o "$object"
    objects+=("$object")
done
mapfile -t maths < <(make -s -C userland math-host-flags | python3 -c '
import shlex,sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=",1)[1])[0].split()))
')
cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack userland/obj/js/core.o \
    "${maths[@]}" "${objects[@]}" -o "$work/target-core-probe"
engine_flags=("${flags[@]}" -U__linux__ -Iuserland/libjs/port/compat
    '-DCONFIG_VERSION="2026-06-04"' -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers -Wno-implicit-fallthrough)
engine=()
for name in quickjs dtoa libregexp libunicode cutils; do
    object="$work/engine-$name.o"
    cc "${engine_flags[@]}" -fno-sanitize=undefined -c "userland/obj/js/upstream/$name.c" -o "$object"
    engine+=("$object")
done
for name in allocator platform format; do
    object="$work/port-$name.o"
    cc "${flags[@]}" -c "userland/libjs/port/$name.c" -o "$object"
    engine+=("$object")
done
cc -fsanitize=address,undefined -pthread -Wl,-z,noexecstack "${engine[@]}" \
    "${maths[@]}" "${objects[@]}" -o "$work/sanitized-core-probe"
for probe in target-core-probe sanitized-core-probe; do
    echo "DOM profile: $probe"
    "$work/$probe" "$@"
done
