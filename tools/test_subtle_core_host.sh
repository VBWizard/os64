#!/usr/bin/env bash
# libdom's Web Cryptography core against the standards' vectors, under ASan
# and UBSan (tools/test_subtle_core_host.c says what is checked). The core
# is pure, so it compiles here with the host's cc, beside the BearSSL
# sources it calls, configured as libtls configures them (port/config.h).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. tools/subtle_bearssl.sh
# The two copies of the source list agree: library.mk's LIBDOM_BEARSSL (what
# libdom.so links) and subtle_bearssl.sh's (what the host harnesses compile).
linked=$(sed -n '/^LIBDOM_BEARSSL :=/,/[^\\]$/p' userland/libdom/library.mk | tr ' \\,)' '\n\n\n\n' |
    grep -E '^(hash|codec|mac|kdf|symcipher|aead|ec|rsa|int|x509|rand)/' | sort)
listed=$(printf '%s\n' "${SUBTLE_BEARSSL_SOURCES[@]}" | sed 's|^userland/libtls/upstream/src/||; s|\.c$||' | sort)
if [ "$linked" != "$listed" ]; then
    echo "FAIL: library.mk's LIBDOM_BEARSSL and tools/subtle_bearssl.sh list different sources" >&2
    diff <(echo "$linked") <(echo "$listed") >&2 || true
    exit 1
fi
work=$(mktemp -d -t os64-subtle-core.XXXXXX)
trap 'rm -rf "$work"' EXIT
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Iuserland/libtls/upstream/inc -c userland/libdom/subtle_core.c -o "$work/subtle_core.o"
cc -O1 -g -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -c tools/test_subtle_core_host.c -o "$work/test.o"
objects=("$work/subtle_core.o" "$work/test.o")
for source in "${SUBTLE_BEARSSL_SOURCES[@]}"; do
    object="$work/$(basename "$source" .c).o"
    cc -O1 -g -std=gnu11 -fsanitize=address,undefined -fno-sanitize-recover=all "${SUBTLE_BEARSSL_FLAGS[@]}" \
        -c "$source" -o "$object"
    objects+=("$object")
done
cc -fsanitize=address,undefined "${objects[@]}" -o "$work/subtle-core"
"$work/subtle-core"
