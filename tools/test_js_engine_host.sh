#!/bin/bash
set -eu
cd "$(git rev-parse --show-toplevel)"
tools/test_js_contract_headers.sh
js_engine_work=$(mktemp -d)
trap 'rm -rf "$js_engine_work"' EXIT
cp userland/libjs/upstream/* "$js_engine_work/"
patch -s -d "$js_engine_work" -p1 < userland/libjs/patches/0001-disable-atomics-preserve-stack-check.patch
if rg '^#define CONFIG_ATOMICS' "$js_engine_work/quickjs.c"; then exit 1; fi
rg -q '^#define CONFIG_STACK_CHECK' "$js_engine_work/quickjs.c"
cc -O2 -fPIC -ffreestanding -fno-builtin -fno-stack-protector \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_GNU_SOURCE \
    '-DCONFIG_VERSION="2026-06-04"' -I "$js_engine_work" \
    "$js_engine_work/quickjs.c" "$js_engine_work/dtoa.c" \
    "$js_engine_work/libregexp.c" "$js_engine_work/libunicode.c" \
    "$js_engine_work/cutils.c" tools/test_js_engine_host.c \
    -lm -o "$js_engine_work/probe"
"$js_engine_work/probe"
