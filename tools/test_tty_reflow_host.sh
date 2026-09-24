#!/bin/bash
# Drive the terminal's reflow on the host, under ASan and UBSan.
#
# The reflow is what keeps a screen and its scrollback whole across a console
# font change (CONSOLE_FONTS.md § The contents survive). It is index
# arithmetic over a ring, which is exactly the kind of code that is right in
# the case you thought of and one row off in the one you did not — so the
# harness checks a property over thousands of random grids instead of a
# handful of pictures, and every result lands in a buffer of exactly the
# planned size.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined \
   -I kernel/include -I abi/include \
   kernel/src/tty_reflow.c tools/test_tty_reflow_host.c -o "$work/test_tty_reflow"

"$work/test_tty_reflow"
echo "test_tty_reflow_host: all checks passed"
