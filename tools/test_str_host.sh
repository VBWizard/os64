#!/usr/bin/env bash
# libos64's memset, memcpy and memmove are loops the compiler's own calls
# land in, so an optimizer must not turn them back into those calls. Built
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
echo "str: memset, memcpy and memmove stay loops at -O2 and -O3, hosted"
