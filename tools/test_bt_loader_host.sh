#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Replace the hardware scheduling boundary, preserving the production rings,
# completion dispatcher, DMA lifetime handling, and complete loader sequence.
python3 - "$work" <<'PY'
from pathlib import Path
import sys
source = Path('kernel/src/driver/system/usb/xhci.c').read_text()
boundary = 'static uint32_t xhci_drain_events(void)\n{'
assert source.count(boundary) == 1
source = source.replace(boundary, boundary + '\n\ttest_controller_step();')
Path(sys.argv[1], 'xhci_bt_host.inc').write_text(source)
PY
(cd kernel && cc -c src/driver/system/usb/bt_firmware.S -o "$work/firmware.o")
cc -std=c11 -Dasm=__asm__ -O1 -g -fno-builtin -Wall -Wextra -Werror -masm=intel \
    -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$work" -I kernel/include -I kernel/include/memory -I kernel/include/driver/system -I abi/include \
    tools/test_bt_loader_host.c "$work/firmware.o" -Wl,--gc-sections,-z,noexecstack -o "$work/bt-loader"
"$work/bt-loader"
