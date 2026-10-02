#!/bin/bash
# The js runner on the host, in two suites.
#
#   1. The runner's own decisions (test_js_cli_host.c): its real main,
#      libos64's real parser, formatter and string code, and a stand-in for
#      the os64_js_* runtime that follows CONTRACT.md (test_js_cli_fake.c),
#      under ASan/UBSan. Every exit status, message and call is checked,
#      including ones the real library cannot be made to produce on demand.
#   2. The runner on the real library (test_js_cli_real.c): actual JavaScript
#      through libjs/runtime, the cross-built QuickJS core and libmath's
#      cross-built objects, with real files and the host's monotonic clock.
#      What a stand-in cannot prove, that the library behaves as the runner
#      expects, is proved here.
set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then rm -rf "$work"; else echo "js runner test artifacts: $work" >&2; fi
}
trap cleanup EXIT

# The runner must also pass the target's strict build.
make -s -C userland --no-print-directory js-runner >/dev/null

flags=(-std=gnu11 -g -O1 -Wall -Wextra -Werror -fno-builtin
       -fsanitize=address,undefined -fno-sanitize-recover=all
       -I userland/libjs/include -I userland/libos64/include -I abi/include -I tools)
cc "${flags[@]}" -Dmain=js_main -c userland/apps/js/js.c -o "$work/js.o"
for source in userland/libos64/args.c userland/libos64/fmt.c userland/libos64/str.c \
              tools/test_js_cli_fake.c tools/test_js_cli_host.c; do
    cc "${flags[@]}" -c "$source" -o "$work/$(basename "$source" .c).o"
done
cc -fsanitize=address,undefined "$work"/*.o -o "$work/js_cli"
"$work/js_cli"

# Suite 2: the real library. The QuickJS core and libmath are the very
# objects the guest's libjs.so and libmath.so are linked from; the runtime,
# the runner and libos64's helpers are rebuilt here under ASan/UBSan.
mkdir -p "$work/real"
real_flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -fno-stack-protector -DOS64_JS_TARGET
    -I userland/libmath/include -I userland/libjs/port -I userland/libjs/include
    -I userland/libos64/include -I abi/include -isystem userland/obj/js/upstream
    -fsanitize=address,undefined -fno-sanitize-recover=all)
for source in userland/libjs/runtime/runtime.c userland/apps/js/js.c \
    userland/libos64/args.c userland/libos64/fmt.c userland/libos64/str.c \
    tools/test_js_port_calendar.c tools/test_js_cli_real.c; do
    extra=()
    case "$source" in */js.c) extra=(-Dmain=js_main) ;; esac
    cc "${real_flags[@]}" "${extra[@]}" -c "$source" -o "$work/real/$(basename "$source" .c).o"
done
# The core carries its own private memcpy family; libos64's must not collide.
objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
    --localize-symbol=memcmp "$work/real/str.o" "$work/real/str-local.o"
rm "$work/real/str.o"
mapfile -t maths < <(make -s -C userland --no-print-directory math-host-flags | python3 -c '
import shlex, sys
for line in sys.stdin:
    if line.startswith("MATH_OBJS="):
        print("\n".join(shlex.split(line.split("=", 1)[1])[0].split()))
')
cc -fsanitize=address,undefined -Wl,-z,noexecstack userland/obj/js/core.o "${maths[@]}" \
    "$work"/real/*.o -o "$work/js_cli_real"
if readelf -dW "$work/js_cli_real" | grep -q 'NEEDED.*libm\.so'; then
    echo "FAIL: the real-library suite linked host libm" >&2
    exit 1
fi
"$work/js_cli_real"
if [ -n "${JS_CLI_KEEP:-}" ]; then cp "$work/js_cli_real" "$JS_CLI_KEEP"; fi
