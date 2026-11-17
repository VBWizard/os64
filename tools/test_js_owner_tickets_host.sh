#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-owner-tickets.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${OWNER_TICKETS_KEEP:-0}" = 0 ]; then rm -rf "$work"; else echo "Owner ticket artifacts: $work" >&2; fi
}
trap cleanup EXIT
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -Iuserland/libjs/include)
cc "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all -Wl,-z,noexecstack \
    userland/libjs/runtime/owner_tickets.c tools/test_js_owner_tickets_host.c -o "$work/sanitized"
"$work/sanitized"
make -C userland -j8 js-owner-tickets-test > "$work/target-build.log" 2>&1
object=userland/obj/js/runtime/owner_tickets.o
if [ -n "$(x86_64-elf-nm -u "$object")" ]; then
    echo "owner metadata acquired an external dependency" >&2
    x86_64-elf-nm -u "$object" >&2
    exit 1
fi
cc "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=all -Wl,-z,noexecstack \
    tools/test_js_owner_tickets_host.c "$object" -o "$work/target-object"
"$work/target-object"
echo "Private metadata: target object has no imports; caller/ID behavior checked, production incarnation issuer remains outside this unit."
