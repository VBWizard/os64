# E1A implementation evidence — 2026-10-11

Implementation branch: `codex/explorer-e01a`, worktree `.worktrees/explorer-e01a`.
Runtime base: `6d988565`. Design documents were imported from `d33e651e` without
its older runtime tree. Chris requested commit and push for review after his
successful OS checkout. Implementation commit `bd86ce4f` is pushed; [PR #254](https://github.com/VBWizard/os64/pull/254) targets `userland`.
Ordinary review and merge remain pending.
[tested-files.json](tested-files.json) identifies source and guest binary SHA-256
values from validation before publication. These source files are unchanged
since validation. Implementation commit `bd86ce4f` introduces this evidence and the
action/status source files.

## Host and cross build

Saved log line endings and trailing whitespace are normalized for repository
checks. All host runs below used ASan/UBSan and leak detection. The sandbox diagnosed
thread-attachment `EPERM` with `TracerPid: 0`; [the diagnostic](lsan-sandbox.log)
is not a leak report. Successful checks ran outside the sandbox with
`ASAN_OPTIONS=detect_leaks=1`. Leak detection was not disabled.

| Command (from the implementation worktree) | Result | Log |
| --- | --- | --- |
| `python3 tools/test_ui_action_host.py --output /tmp/e01a-action-fake` | 231 checks, 0 failures | [fake actions/status](e01a-action-fake.log) |
| `python3 tools/test_ui_action_host.py --real --output /tmp/e01a-action-real` | 278 checks, 0 failures | [real actions/status](e01a-action-real.log) |
| `python3 tools/test_ui_text_host.py --output /tmp/e01a-ui-fake` | 148 checks, 0 failures | [fake UI](e01a-ui-fake.log) |
| `python3 tools/test_ui_text_host.py --real --output /tmp/e01a-ui-real` | 1626 checks, 0 failures | [real UI](e01a-ui-real.log) |
| `python3 tools/test_scribe_host.py --output /tmp/e01a-scribe` | 2470 checks, 0 failures | [Scribe](e01a-scribe.log) |
| `python3 tools/test_font_contracts.py` | Backend lifecycle/allocation checks, 17 layout vectors, strict target header/backend compilation passed | [font contracts](e01a-font-contracts.log) |
| `make -C userland "$PWD/userland/bin/actiongallery" "$PWD/userland/bin/tests/fontsettingstest"` | Strict cross build passed; builds libos64 and FreeType dependencies | [cross build](cross-build.log) |

The focused harness drives the production action registry, views, bindings,
router, status class and gallery. Coverage includes copied descriptors, duplicate
IDs/shortcuts, reserved chords, registration/view/binding/text allocations,
context retain refusal and cleanup, the 32-view bound, independent snapshots,
invalid radio state, stale contexts, refresh after refused/error invocation,
disabled-after-press, detach during an outstanding press, callback lifetime,
PS/2/HID identities, Ctrl control bytes, lock modifier masking, keypad ASCII,
escape-burst rejection, held gestures, unclaimed clipboard input, stale help and
notice tokens, exhaustion, clipping/notice priority, repeated visibility changes,
hidden font preparation, failure through the final status slot, and allocation-free
font commit with real faces. Gallery checks retain messages/actions through reflow
and transfer focus before hiding its target panel.

Scribe's existing fixture failed to compile because its included UI settings
helper and Scribe both defined `place`. A local macro around the fixture include
isolates that helper; production Scribe is unchanged.

## QEMU preparation and execution

Only disposable files under `/tmp/e01a-vm` were modified. The existing ISO and
kernel from the base checkout were reused; its first Limine entry was changed
in the scratch ISO to boot the existing FAT-root GUI environment:

```text
ROOT=2f4fd02e-68b4-4c82-98bc-72467529b3fc BACKSTOP=10 KWORKER HUSK GUI
```

[limine-tested.conf](limine-tested.conf) retains that input. Preparation commands:

```sh
mkdir -p /tmp/e01a-vm
cp --sparse=always /home/yogi/src/os64/disk/os64.img /tmp/e01a-vm/os64.img
cp --sparse=always /home/yogi/src/os64/disk/os64_data.img /tmp/e01a-vm/data.img
xorriso -osirrox on -indev /home/yogi/src/os64/os64_kernel.iso \
  -extract /boot/limine/limine.conf /tmp/e01a-vm/limine.conf
# Make the extracted file writable and replace its first cmdline as above.
xorriso -indev /home/yogi/src/os64/os64_kernel.iso \
  -outdev /tmp/e01a-vm/e01a.iso -boot_image any replay \
  -map /tmp/e01a-vm/limine.conf /boot/limine/limine.conf
mcopy -o -i /tmp/e01a-vm/os64.img@@1048576 userland/bin/actiongallery ::/bin/actiongallery
mcopy -o -i /tmp/e01a-vm/os64.img@@1048576 userland/bin/libos64.so ::/lib/libos64.so
mcopy -o -i /tmp/e01a-vm/os64.img@@1048576 userland/bin/libfreetype.so ::/lib/libfreetype.so
mcopy -o -i /tmp/e01a-vm/os64.img@@1048576 userland/bin/tests/fontsettingstest ::/tests/fontsettingstest
```

Boot command (USB run; omit `-device usb-kbd,id=e01a_hidkbd` for PS/2):

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64,+rdrand,+rdseed -m 4g -smp 4 \
  -no-reboot -cdrom /tmp/e01a-vm/e01a.iso -boot d -display none \
  -serial file:/tmp/e01a-vm/serial.log \
  -monitor telnet:127.0.0.1:55556,server,nowait \
  -qmp unix:/tmp/e01a-vm/qmp.sock,server=on,wait=off \
  -drive file=/tmp/e01a-vm/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=/tmp/e01a-vm/data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial \
  -device qemu-xhci -device usb-kbd,id=e01a_hidkbd
```

[qmp_control.py](qmp_control.py) contains the actual input/screenshot helper.
Copy it to `/tmp/e01a-vm/control.py`; it talks to the scratch QMP socket.
Use `key ctrl-alt-f1`, `type actiongallery`, then `key ctrl-alt-f8` to launch
from the text shell and see the window. `hold d` sends Ctrl+D down, twelve
repeated D downs, then releases both keys. `cycle 20` sends twenty rounds of
panel hide/show, status hide/show, and a new help message. The mouse commands
use relative displacements from the current pointer location.

| Observation | Evidence |
| --- | --- |
| A held PS/2 Ctrl+D invokes once; a mouse click of the bound button invokes the same action once more (A=2, B=0) | [PS/2 button and shortcut](ps2-final.png) |
| Hiding the focused target panel moves focus to the surviving content control and reflows controls | [hidden panel](hidden-final.png), host focus assertion |
| Clearing an older help token retains “Action help 2”; persistent notice remains visible | [token priority](tokens-final.png) |
| USB keyboard present at boot: held Ctrl+D records A once; choosing initially disabled B consumes its chord without recording | [disabled B](hid-disabled-b.png) |
| Enabling B and invoking records B once independently (A=1, B=1) | [two targets](hid-two-targets.png) |
| Invalidating target A disables its bound button while application controls remain usable | [stale target](stale-final.png), host stale invocation checks |
| `/tests/fontsettingstest 9` publishes a 32px DejaVu Interface face while preserving other roles; the existing gallery reflows with notice/help intact | [font result](font32.txt), [live reflow](interface-32.png) |
| Shortcuts and hide/show remain operational after adoption; deferred close exits normally | [USB serial](qemu-hid.log), [exit code](exit.txt) |

The gallery's planner refuses a font candidate that cannot fit its window,
preserving the installed face/layout. The default window leaves room for the
32px acceptance face including native frame height. Narrow status layout is
also checked by pixel comparison on the host; persistent notice owns its clip.

The USB keyboard must be attached at boot for this evidence. A preliminary
hotplug attempt did not produce usable keyboard input in this existing guest;
that kernel behavior was not changed by E1A. Both final input paths were tested
with the appropriate keyboard present at boot. The test VMs were stopped.

## Resources and scope

After twenty warm-up rounds, another twenty rounds leave the gallery at the
same 1,048,576 mapped heap bytes, 103,808 live bytes and 433 live blocks, with
`audit ok`. See [warm](heap-warm.txt) and [after](heap-after.txt). The comparison begins after warm-up.
Reports were obtained with `cat /proc/58/heap > /home/...`, followed by `sync`
and read-only `debugfs dump` from the scratch data image at offset 1,048,576.
Host teardown releases context references, copied registry/messages and font
runs and leaves the UI binding NULL; LeakSanitizer passes.

No native menus/popups, icon files, kernel input policy, Scribe editing or
clipboard behavior, persistent visibility storage, or P5 installation was
implemented. E1B/E1C and E1K follow-up remain separate. E1A awaits ordinary code
review; these results do not accept the complete E1 slice.

## Chris's checkout and review publication

Chris confirmed that the gallery worked perfectly in his OS checkout, then
explicitly requested commit and push for review. Hardware and installed revision
were not reported; this confirmation is separate from the automated QEMU runs.
His publication request supersedes the QEMU guide's earlier no-commit instruction.
The branch is pushed and [PR #254](https://github.com/VBWizard/os64/pull/254) is open for ordinary review; no review approval
or merge is claimed.
