#!/usr/bin/env bash
# test262_host.sh — test262 through the real libjs, held to its expectation
# files (INTL.md § 8; tools/test262/run.py says what is run and what the
# gate is).
#
#   tools/test262_host.sh                        every directory with expectations
#   tools/test262_host.sh intl402/DateTimeFormat one directory
#   tools/test262_host.sh --update [dir ...]     rewrite [excluded] from this run
#   tools/test262_host.sh --only <substring> dir only the tests whose path holds it
#
# The suite comes from tools/test262_fetch.sh, at its pin, in .vendor/test262
# (TEST262_DIR elsewhere). The runner is the target engine (the very core.o
# libjs.so is linked from) with libjs's runtime and Intl rebuilt here under
# ASan/UBSan, on controlled OS services.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-test262-build.XXXXXX)
trap 'rm -rf "$work"' EXIT
make -s -C userland js-core > "$work/build.log" 2>&1 || { cat "$work/build.log"; exit 2; }
flags=(-O1 -g -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    -I userland/libmath/include -I userland/libjs/include -I userland/libjs/port
    -I userland/libos64/include -I abi/include -isystem userland/obj/js/upstream
    '-DLIBJS_INTL_DIR="userland/libjs/intl/"'
    -fsanitize=address,undefined -fno-sanitize-recover=all)
for source in tools/test262/runner.c userland/libjs/runtime/runtime.c userland/libjs/intl/intl.c \
    userland/libos64/str.c userland/libos64/fmt.c tools/test_js_port_calendar.c; do
    extra=()
    case "$source" in tools/test262/runner.c) extra=(-U_FORTIFY_SOURCE -fbuiltin) ;; esac
    cc "${flags[@]}" "${extra[@]}" -c "$source" -o "$work/$(basename "$source" .c).o"
done
# The core carries its own private memcpy family; libos64's must not collide.
objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
    --localize-symbol=memcmp "$work/str.o" "$work/str-local.o"
rm "$work/str.o"
mapfile -t maths < <(make -s -C userland --no-print-directory math-host-flags | python3 -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
cc -fsanitize=address,undefined -Wl,-z,noexecstack userland/obj/js/core.o "${maths[@]}" \
    "$work"/*.o -o "$work/runner"
python3 tools/test262/run.py --runner "$work/runner" "$@"
