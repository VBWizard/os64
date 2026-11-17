#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-owner-incarnation.XXXXXX)
cleanup() {
    result=$?
    if [ "$result" -eq 0 ] && [ "${OWNER_INCARNATION_KEEP:-0}" = 0 ]; then rm -rf "$work"; else echo "Incarnation artifacts: $work" >&2; fi
}
trap cleanup EXIT
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin
    -fno-tree-loop-distribute-patterns -Iuserland/libjs/include -pthread
    -fsanitize=address,undefined -fno-sanitize-recover=all -Wl,-z,noexecstack)
cc "${flags[@]}" userland/libjs/runtime/owner_incarnation.c tools/test_js_owner_incarnation_host.c -o "$work/sanitized"
"$work/sanitized"
make -C userland -j8 js-owner-incarnation-test > "$work/target-build.log" 2>&1
object=userland/obj/js/runtime/owner_incarnation.o
if [ -n "$(x86_64-elf-nm -u "$object")" ]; then
    echo "incarnation issuer acquired an external dependency" >&2
    exit 1
fi
python3 - <<'PY'
import re,subprocess
obj='userland/obj/js/runtime/owner_incarnation.o'
symbols=subprocess.check_output(['x86_64-elf-readelf','-sW',obj],text=True)
functions=re.findall(r'FUNC\s+GLOBAL\s+(\w+)\s+\d+\s+(js_owner_incarnation_\w+)',symbols)
assert len(functions)==2 and all(v=='HIDDEN' for v,n in functions),functions
objects=re.findall(r'\d+:\s+[0-9a-f]+\s+(\d+)\s+OBJECT\s+LOCAL\s+DEFAULT\s+\d+\s+(\S+)',symbols)
assert objects==[('8','process_issuer')],objects
assembly=subprocess.check_output(['x86_64-elf-objdump','-d',obj],text=True)
assert 'lock cmpxchg' in assembly
print('Incarnation object: two hidden operations, one 8-byte issuer, lock-free target CAS, no imports')
PY
cc "${flags[@]}" tools/test_js_owner_incarnation_host.c "$object" -o "$work/target-object"
"$work/target-object"
echo "Issuer namespace tested; runtime binding and admission remain separate."
