#!/bin/bash
# Drive the SYN option parser and the window-scale arithmetic on the host,
# under ASan and UBSan. The module is pure (tcp_options.h says why), so this
# is plain cc, seconds per cycle, and the only proof of the parser that
# does not need a scaling peer on the wire.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I kernel/include \
   kernel/src/driver/net/tcp_options.c tools/test_tcp_options_host.c -o "$work/test_tcp_options"

"$work/test_tcp_options"
