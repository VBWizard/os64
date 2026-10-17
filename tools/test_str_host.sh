#!/usr/bin/env bash
# libos64's memset, memcpy and memmove are where the compiler's own calls
# land, so an optimizer must not turn their loops back into those calls. Built
# HOSTED at -O2 and -O3 with no sanitizer — no -ffreestanding to stop it, no
# sanitizer memset to hide it — they must run, not recurse until the stack
# runs out.
set -eu
cd "$(git rev-parse --show-toplevel)"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/probe.c" <<'PROBE'
#include <stddef.h>
void *os64_memset(void *, int, size_t);
void *os64_memcpy(void *, const void *, size_t);
void *os64_memmove(void *, const void *, size_t);
int main(void)
{
    static unsigned char a[4096], b[4096];
    os64_memset(a, 0x5a, sizeof a);
    os64_memcpy(b, a, sizeof b);
    os64_memmove(a + 1, a, sizeof a - 1);
    return !(a[4095] == 0x5a && b[17] == 0x5a);
}
PROBE
for level in -O2 -O3; do
    cc "$level" -Wall -Wextra -Werror -I userland/libos64/include -I abi/include \
        -o "$dir/probe" "$dir/probe.c" userland/libos64/str.c
    timeout 10 "$dir/probe"
done
echo "str: memset, memcpy and memmove do not call themselves at -O2 and -O3, hosted"
# Their bytes, under the sanitizers and at the optimization userland is built
# with. str.c defines the C library's own names for the compiler's calls, so
# those are made local to its object: the test's memcpy and memcmp, and the
# sanitizers', stay the C library's.
for flags in "-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all" "-O2"; do
    # shellcheck disable=SC2086
    cc $flags -std=gnu11 -Wall -Wextra -Werror -msse2 -I userland/libos64/include -I abi/include \
        -c userland/libos64/str.c -o "$dir/str.o"
    objcopy --localize-symbol=memcpy --localize-symbol=memmove --localize-symbol=memset \
        --localize-symbol=memcmp "$dir/str.o" "$dir/str-local.o"
    # shellcheck disable=SC2086
    cc $flags -std=gnu11 -Wall -Wextra -Werror -o "$dir/test_str" tools/test_str_host.c "$dir/str-local.o"
    "$dir/test_str"
done
