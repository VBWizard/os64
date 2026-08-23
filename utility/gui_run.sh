#!/bin/bash
# usage: utility/gui_run.sh PORT "<key script>"   — run from the repo root; builds nothing
# Key script, one command per line:
#   keys a b ctrl-alt-p   HMP sendkey names (combos, one press each)
#   qmp down:alt down:tab up:tab up:alt   raw QMP key edges (hold a chord across keys)
#   qmp btn:down btn:up wait:150 btn:down btn:up   raw left-button edges (a double-click)
#   raw mouse_move 250 -282   any HMP monitor line
#   sleep N  /  shot name     (PNG lands in $S/name.png)
# Boots the SECOND Limine entry (/QEMU GUI Boot) with a USB keyboard attached so the
# HID dialect is what gets tested; set GUI_RUN_SCRATCH to choose where logs/shots go.
S=${GUI_RUN_SCRATCH:-/tmp/os64-gui-run}; mkdir -p $S
P=$1; SCRIPT=$2
cd /home/yogi/src/os64; rm -f $S/qemu_com1.log
setsid timeout 170 qemu-system-x86_64 -machine q35 -cdrom os64_kernel.iso -boot d -m 8g -no-reboot -smp 8 -display none \
  -serial file:$S/qemu_com1.log -monitor telnet:127.0.0.1:$P,server,nowait \
  -drive file=disk/os64.img,format=raw,if=none,id=nvme1 -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=disk/os64_data.img,format=raw,if=none,id=data1 -device nvme,drive=data1,serial=data1-serial \
  -device qemu-xhci -device usb-kbd -qmp unix:$S/qmp.sock,server,nowait > $S/qemu.err 2>&1 &
QPID=$!
sleep 4
mon() { exec 3<>/dev/tcp/127.0.0.1/$P; while [ $# -gt 0 ]; do printf 'sendkey %s\n' "$1" >&3; sleep 0.1; shift; done; sleep 0.4; exec 3>&- 3<&-; }
shot() { exec 3<>/dev/tcp/127.0.0.1/$P; printf 'screendump %s/%s.ppm\n' "$S" "$1" >&3; sleep 1.5; exec 3>&-; python3 -c "from PIL import Image; Image.open('$S/$1.ppm').save('$S/$1.png')"; }
mon down ret
for i in $(seq 1 30); do sleep 2; grep -q "guicomp" $S/qemu_com1.log 2>/dev/null && break; done; sleep 12
while IFS= read -r line; do
  set -- $line; cmd=$1; shift
  case $cmd in keys) mon "$@";; qmp) python3 utility/qmpkey.py $S/qmp.sock "$@";; raw) exec 3<>/dev/tcp/127.0.0.1/$P; printf "%s\n" "$*" >&3; sleep 0.3; exec 3>&-;; sleep) sleep "$1";; shot) shot "$1";; esac
done <<< "$SCRIPT"
exec 3<>/dev/tcp/127.0.0.1/$P; printf 'quit\n' >&3; exec 3>&-; sleep 2; kill $QPID 2>/dev/null
