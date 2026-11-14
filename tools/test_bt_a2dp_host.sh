#!/bin/bash
# Run the A2DP pump on the host, under ASan and UBSan, against the real
# AVDTP machine, the real SBC encoder, a model seam and a model pair of
# headphones. tools/test_bt_a2dp_host.c says what is asserted.
#
# The encoder is built as tools/test_sbc_host.sh builds it: upstream with
# the port's switches, -O2, warnings shown and not fatal, UBSan's
# shift-base check off for those files alone (kernel/ports/sbc/README.md
# § UBSan). Ours is built with every warning fatal. The kernel's headers are
# reached for quoted includes only, so its math.h does not stand in for the
# host's. The builds go in a temporary directory, not under kernel/obj:
# tools/test_sbc_audit.py reads every object there as the kernel's.
#
#   tools/test_bt_a2dp_host.sh [soup rounds, default 50000]

set -eu
cd "$(git rev-parse --show-toplevel)"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
P=kernel/ports/sbc
upstream=$(grep -o 'ports/sbc/upstream/[^ ]*\.c' $P/sources.mk | sed 's|^|kernel/|')
[ -n "$upstream" ] || { echo "FAIL test_bt_a2dp_host: no upstream sources named in $P/sources.mk" >&2; exit 1; }

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
UP_FLAGS="-std=gnu11 -g -O2 -fno-strict-aliasing -Wall -Wextra -include $P/port/sbc_switches.h -I $P/port/compat -I $P/upstream/include"
INC="-iquote kernel/include -iquote $P/port -iquote $P/upstream/include"
OUR_FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror $INC $SAN"

objs=""
for src in $upstream; do
	base=$(basename "$src" .c)
	cc $UP_FLAGS $SAN -fno-sanitize=shift-base -c "$src" -o "$out/$base.o"
	objs="$objs $out/$base.o"
done
for src in $P/port/sbc_port.c kernel/src/audio/sbc_frame.c kernel/src/driver/system/usb/bt_avdtp.c \
	kernel/src/driver/system/usb/bt_a2dp.c tools/test_bt_a2dp_host.c; do
	base=$(basename "$src" .c)
	cc $OUR_FLAGS -c "$src" -o "$out/$base.o"
	objs="$objs $out/$base.o"
done
cc $SAN $objs -lm -o "$out/test_bt_a2dp"

"$out/test_bt_a2dp" "${1:-50000}"
