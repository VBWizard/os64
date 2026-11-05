# test262/build.sh — sourced: libjs_runner <dir> builds <dir>/runner
# (tools/test262/runner.c): the target engine, the very core.o libjs.so is
# linked from, with libjs's runtime and Intl rebuilt under ASan/UBSan, on
# controlled OS services. Run from the top of the tree.
libjs_runner() {
    local work=$1
    make -s -C userland js-core > "$work/build.log" 2>&1 || { cat "$work/build.log"; return 2; }
    local flags=(-O1 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
        -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
        -I userland/libmath/include -I userland/libjs/include -I userland/libjs/port
        -I userland/libos64/include -I abi/include -isystem userland/obj/js/upstream
        '-DLIBJS_INTL_DIR="userland/libjs/intl/"'
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    local source extra
    for source in tools/test262/runner.c userland/libjs/runtime/runtime.c userland/libjs/intl/intl.c \
        userland/libos64/str.c userland/libos64/fmt.c tools/test_js_port_calendar.c; do
        extra=()
        case "$source" in tools/test262/runner.c) extra=(-U_FORTIFY_SOURCE -fbuiltin) ;; esac
        cc "${flags[@]}" "${extra[@]}" -c "$source" -o "$work/$(basename "$source" .c).o" || return 2
    done
    # The core carries its own private memcpy family; libos64's must not collide.
    objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
        --localize-symbol=memcmp "$work/str.o" "$work/str-local.o"
    rm "$work/str.o"
    local maths
    mapfile -t maths < <(make -s -C userland --no-print-directory math-host-flags | python3 -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
    cc -fsanitize=address,undefined -Wl,-z,noexecstack userland/obj/js/core.o "${maths[@]}" \
        "$work"/*.o -o "$work/runner"
}
