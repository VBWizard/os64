#!/usr/bin/env bash
# The JS speed ledger's arithmetic (userland/libjs/runtime/ledger.c) on the
# host, under ASan and UBSan: tools/test_js_ledger_host.c says what it holds.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
work=$(mktemp -d -t os64-js-ledger.XXXXXX)
trap 'rm -rf "$work"' EXIT
cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I userland/libjs/runtime userland/libjs/runtime/ledger.c tools/test_js_ledger_host.c -o "$work/ledger"
"$work/ledger"
