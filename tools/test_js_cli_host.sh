#!/bin/bash
# The js runner on the host, under the sanitizers: its real main, libos64's
# real argument parser, formatter and string code, and a stand-in for the
# os64_js_* runtime that follows CONTRACT.md (tools/test_js_cli_fake.c).
# What the stand-in cannot prove is that the real library behaves the way the
# contract says; the same cases run against R2's library when it exists.
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
