#!/bin/bash
# The Bluetooth host with its LE peers, its Classic link and slot 2's audio,
# under ASan and UBSan. Slot 2's audio runs the REAL SBC encoder, built as
# tools/test_bt_a2dp_host.sh builds it: upstream with the port's switches,
# -O2, warnings shown and not fatal, UBSan's shift-base check off for those
# files alone (kernel/ports/sbc/README.md § UBSan); the boundary with every
# warning fatal. The builds go in a temporary directory, not under
# kernel/obj: tools/test_sbc_audit.py reads every object there as the
# kernel's.
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
P=kernel/ports/sbc
upstream=$(grep -o 'ports/sbc/upstream/[^ ]*\.c' $P/sources.mk | sed 's|^|kernel/|')
[ -n "$upstream" ] || { echo "FAIL test_bt_multi_host: no upstream sources named in $P/sources.mk" >&2; exit 1; }
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
UP_FLAGS="-std=gnu11 -g -O2 -fno-strict-aliasing -Wall -Wextra -include $P/port/sbc_switches.h -I $P/port/compat -I $P/upstream/include"
objs=""
for src in $upstream; do
	base=$(basename "$src" .c)
	cc $UP_FLAGS $SAN -fno-sanitize=shift-base -c "$src" -o "$work/$base.o"
	objs="$objs $work/$base.o"
done
cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -iquote kernel/include -iquote $P/port -iquote $P/upstream/include \
    $SAN -c $P/port/sbc_port.c -o "$work/sbc_port.o"
cc -std=c11 -O1 -g -Wall -Wextra -Werror $SAN \
    -I kernel/include -I kernel/include/memory -iquote $P/port \
    tools/test_bt_multi_host.c kernel/src/driver/system/hid_keyboard_map.c \
    kernel/src/driver/system/hid_mouse.c $objs "$work/sbc_port.o" -lcrypto -o "$work/bt-host"
"$work/bt-host"
