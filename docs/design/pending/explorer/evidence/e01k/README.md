# E1K validation — 2026-10-11

Branch `codex/explorer-e1k`, implementation based on `2bc9f2f4`.
[Binary hashes](binaries.sha256) and [source hashes](sources.sha256) identify the
original review snapshot. The [readiness regression fix](wake-regression/README.md)
records the updated diagnostic and ten passing 153-check QEMU runs. Chris
subsequently confirmed validation in both QEMU and on the P5. That acceptance
is user-reported; additional run counts, logs and installed-binary hashes were
not supplied. The first ordinary review produced three findings; the fixes
and their new validation are recorded below. Those fixes await re-review and
fresh P5 acceptance.

Retained logs have trailing whitespace stripped for repository checks.

## Results

- Kernel and strict userland cross builds passed. New diagnostic installs as
  `/tests/popuptest`; `--show` opens a visual native-window fixture.
- Production queue harness passed at all 64 ring wrap positions, covering
  dropped releases, focus eviction, gesture-history truncation and retained
  control ordering, with ASan/UBSan/LeakSanitizer enabled.
- Action/status tests: 495 fake-font and [542 real-font checks](actions-real.log),
  zero failures, ASan/UBSan/LeakSanitizer enabled. New checks reset a pressed
  button, pointer grab and shortcut-held state on INPUT_LOST.
- Appearance suite passed with ASan/UBSan/LeakSanitizer, including queue pressure,
  retained appearance/pointer state, doorbell fairness, font transactions and
  existing settings/control-center behavior. Two existing overflow fixtures
  were updated to expect INPUT_LOST and removal of queued mouse gestures.
- [Popup diagnostic](popuptest.txt): 146 checks, zero failures, exit 0 in a
  four-core QEMU guest. It exercises empty-set/invalid-output readiness, rotation
  without consuming events, delayed wakes, both waiter-conflict directions,
  caught-signal interruption, last-window teardown, owner/focus refusals,
  50x20 popup pitch/minimums, content geometry, nested focus/peer events,
  conditional focus return, BUSY parent destruction, foreign-task refusal,
  12 task-exit families and two full-window-table create/destroy passes.
- Existing [window focus](windowfocustest.txt): 11 checks, zero failures.
  Existing [doorbell diagnostic](doorbelltest.txt): PASS, 0 failures; its success
  exit marker is `0x600B0000`. These ran on the same final kernel before the
  popup fixture's signal-sender correction.
- [Boot serial log](serial-acceptance.log): built-in batches report 34/34/3,
  zero failures. Several networking and root-ext2 tests explicitly SKIP in
  this FAT-root/no-NIC configuration; the batch totals include those skips.
- `git diff --check` passed. The retirement scan's reported document names,
  abbreviated ABI names and historical `popup_attach` discussion are retained
  deliberately; no retired runtime identifier was found. No new superlative
  comment claims were reported. See [scan](stale-refs.log).

The first sandbox sanitizer attempts reached LeakSanitizer attachment EPERM;
existing queue/action binaries were rerun outside the sandbox with
`ASAN_OPTIONS=detect_leaks=1`. The appearance script was rebuilt/run there after
correcting its sibling overflow expectation. Leak detection was not disabled.

The first expanded guest fixture stalled because its sibling signal sender
could claim the task-wide handler before the parked main thread observed it.
Thread inspection showed the sender exited and the main thread remained parked.
The fixture now sends from another process, leaving the parked thread as the
recipient. The corrected run produced the 146-check result above; the kernel
wait implementation was unchanged by this diagnosis.

## Visual checks

1. [Native panels](native-panels.png): two independent panels extend beyond
   the owner's frame; the owner's titlebar stays active while the child is
   focused. Switching from the text VT to the GUI preserves the family.
2. [First Alt+Tab](alt-tab.png): the other ordinary window receives focus;
   neither popup becomes a switcher destination.
3. [Close after external focus](external-close.png): Escape closes the deepest
   panel, leaving the other window's titlebar active. [Focus log](popup-show.txt)
   records the transition from child handle 5 to other handle 2, with no later
   focus return caused by that closure.
4. [Actual outside click](external-click.png): on a fresh family, move the
   pointer from (512,384) by (288,-234) and click the other window. Its titlebar
   becomes active and the owner's becomes inactive. Escape/Q then close the
   fixture without a focus grab back to its owner; [log](popup-click.txt).

The fixture intentionally keeps panels visible on external focus so the kernel
focus behavior can be observed. Menu dismissal and click suppression belong to
E1B; these pictures do not claim that the menu controller exists.

## Reproduction

Run from the worktree:

```sh
make -C kernel -j8
make -C userland "$PWD/userland/bin/tests/popuptest" \
  "$PWD/userland/bin/tests/windowfocustest" "$PWD/userland/bin/tests/doorbelltest"
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I abi/include -I kernel/include \
  tools/test_gui_queue_host.c kernel/src/gui/event_queue.c -o /tmp/e1k-queue-host
ASAN_OPTIONS=detect_leaks=1 /tmp/e1k-queue-host
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --output /tmp/e1k-actions
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --real --output /tmp/e1k-actions-real
ASAN_OPTIONS=detect_leaks=1 bash tools/test_appearance_host.sh
```

QEMU uses scratch copies of both repository disks under `/tmp/e1k-vm`; no
repository disk is modified. Starting with the existing ISO, replay its boot
metadata and replace `/boot/os64_kernel` with the worktree kernel and
`/boot/limine/limine.conf` with [the tested config](limine-tested.conf):

```sh
xorriso -indev /home/yogi/src/os64/os64_kernel.iso \
  -outdev /tmp/e1k-vm/e1k-final.iso -boot_image any replay \
  -map /tmp/e1k-vm/limine.conf /boot/limine/limine.conf \
  -map kernel/bin/os64_kernel /boot/os64_kernel
mcopy -o -i /tmp/e1k-vm/os64.img@@1048576 userland/bin/tests/popuptest \
  userland/bin/tests/windowfocustest userland/bin/tests/doorbelltest ::/tests/
mcopy -o -i /tmp/e1k-vm/os64.img@@1048576 userland/bin/libos64.so \
  userland/bin/libfreetype.so ::/lib/
qemu-system-x86_64 -machine q35 -cpu qemu64,+rdrand,+rdseed -m 4g -smp 4 \
  -no-reboot -cdrom /tmp/e1k-vm/e1k-final.iso -boot d -display none \
  -serial file:/tmp/e1k-vm/serial-acceptance.log \
  -monitor telnet:127.0.0.1:55556,server,nowait \
  -qmp unix:/tmp/e1k-vm/qmp.sock,server=on,wait=off \
  -drive file=/tmp/e1k-vm/os64.img,format=raw,if=none,id=nvme1 \
  -device nvme,drive=nvme1,serial=nvme1-serial \
  -drive file=/tmp/e1k-vm/data.img,format=raw,if=none,id=data1 \
  -device nvme,drive=data1,serial=data1-serial \
  -device qemu-xhci -device usb-kbd,id=e1k_hidkbd
```

Use the E1A QMP helper with its socket path changed to `/tmp/e1k-vm/qmp.sock`.
Wait for `husk: started` in the serial log before typing, and let a VT switch
settle before entering a command. In VT1 run `/tests/popuptest`, redirecting to
`/home/popuptest-final.txt`; similarly run the focus/doorbell fixtures. Run
`sync` before reading reports using
`debugfs -R 'cat /popuptest-final.txt' '/tmp/e1k-vm/data.img?offset=1048576'`.
For visual checks run `/tests/popuptest --show`, then Ctrl+Alt+F8; Escape closes
the deepest panel and Q exits. Stop only this VM through its QMP socket.

## Limits

P5 acceptance is Chris's report above; retained automated logs are from QEMU.
Menu widget/controller acceptance and independent code review remain pending. The allocation-time parent-destruction/reuse race is guarded by
locked ID/focus revalidation but was not forced deterministically in this guest
run. Full-table repetition verifies reusable slots, not a measured allocator
leak budget. Existing task VA-reservation and sibling-TLB limitations remain
outside E1K. E1B must test post-destruction handle quarantine, input-loss
dismissal and expiry of suppression after focus without a content click.

## PR #257 review round 1 — 2026-10-11

All three findings were accepted. Popup capacity is now an explicit create-time
hint (bit 34), set by `os64_gui_popup_create` for owned and unowned menus.
Borderless/pinned applications retain ordinary capacity unless they opt in.
Repeated input loss yields to retained control records; direct consumers cancel
custom gestures, including gterm's selection, Workshop's overlapping UI trees,
Frame Studio's preview, Yonder's page presses/key decoders/question bar, and
standalone grootmenu dismissal.

Validation of the fixes:

- The new queue regression failed against the reviewed implementation at the
  expected retained-close assertion. After the fix, all 64 wrap positions pass
  drop/eviction, retained-order and sustained-overflow fairness checks, including
  competition with doorbells. The existing appearance queue test also checks
  that an emptied ring does not carry fairness state into a later loss episode.
- [Appearance and consumer suite](review-1/appearance.txt) passes, including a
  gterm release queued before overflow and reset of all Workshop trees while a
  confirmation owns input. [Yonder](review-1/yonder.txt): 2,654 checks, zero
  failures, including loss between question-bar press/release and key decoding.
  Host tests ran with ASan, UBSan and `ASAN_OPTIONS=detect_leaks=1` outside the
  sandbox. Frame Studio and grootmenu were audited and cross-built; their new
  loss branches were not injected interactively in this run.
- The kernel and affected applications cross-build. Four-core QEMU passes
  [run 1](review-1/native-1.txt) and [run 2](review-1/native-2.txt), 159 checks and
  zero failures apiece; both exit 0 in the [serial log](review-1/serial.txt).
  New assertions cover invalid capacity-hint flags and borderless/pinned
  growth with stable canvas pointer/pitch. Existing popup-capacity checks stay.
- gclock task 111 starts pinned and borderless with the inherited 16px font
  ([before](review-1/gclock-before.png)). While that same process remains alive,
  `/tests/fontsettingstest 9` successfully applies Interface 32px
  ([report](review-1/clock-font.txt)); the clock grows and adopts the larger text
  ([after](review-1/gclock-font32.png)). Ctrl+Alt+right-drag then enlarges the
  window while keeping the text at 32px and centered
  ([resized](review-1/gclock-resized.png)). Reproduce with `Titlebar=false`,
  `Pinned=true`, `FontSize=inherit`; command 6 resets the shared font defaults.
  The early serial history includes a test-controller retry: its missing
  ampersand mapping initially launched gclock in the foreground. The recorded
  before/after pair is the final same-process check after correcting that.

[Binary hashes](review-1/binaries.sha256) identify this validation build. The
latest gterm handler was exercised on the host and cross-built after the guest
was staged. The guest used scratch disks and was stopped after testing. These
review fixes have not been revalidated on P5; the earlier acceptance applies to
the preceding implementation. Re-review is pending.

### Follow-up: Interface Apply remains unresolved in Chris's setup

Chris confirmed that window resizing works again, but substantially increasing
Interface font size and clicking Apply did not change gclock's text. He reports
that this behavior predates E1K and asked to defer investigation to
[DEBTS.md](../../../../../../DEBTS.md). Exact font/configuration values and the
cause have not been established. The controlled QEMU 16-to-32px test above
remains valid evidence for that tested configuration; it is not confirmation
that live font adoption works through Workshop in Chris's setup. No runtime
change was made for this follow-up.

## PR #257 review round 2 — owned-popup close escalation

Accepted the finding that a second Alt+F4 on an undrained owned popup could
terminate its task, including the document. The compositor now excludes
windows with `popup_owner_id` from escalation and leaves their escalation
timestamp unarmed. Repeated presses request close again. Windows without a
popup owner retain their existing escalation behavior. The API and design
comments describe this exception; the task-exit sweep's misplaced description
was moved back to the sweep.

The native close probes catch SIGTERM so a failure can be reported rather than
killing the diagnostic. They pause event draining for eight seconds after a
serial READY marker while the QMP controller sends two Alt+F4 presses 400ms
apart. The full-queue probe fills retained focus records by creating and
closing 40 nested children, returning focus to the tested popup each time.
The controller changes to VT8 before injecting the chord.

| Probe | Before fix | After fix |
| --- | --- | --- |
| Stalled owned popup | [SIGTERM; one close; fails](review-2/close-before-owned.txt) | [No signal; two closes; passes](review-2/close-after-owned.txt) |
| Full owned-popup queue | [SIGTERM; closes dropped; fails](review-2/close-before-full.txt) | [No signal; loss reported; passes](review-2/close-after-full.txt) |
| Ordinary window control | [SIGTERM; passes](review-2/close-before-ordinary.txt) | [SIGTERM; passes](review-2/close-after-ordinary.txt) |

The fixed kernel also passes the default [159-check native suite](review-2/native.txt)
with exit 0. Kernel and fixture cross-builds pass. Both comparison boots used
four-core QEMU and scratch disks; the VM is stopped. This round has not been
retested on P5. It exercises kernel close delivery/escalation, not the E1B menu
controller's dismissal behavior.

[Before serial](review-2/before-serial.txt), [after serial](review-2/after-serial.txt),
[before controller results](review-2/before-runs.txt),
[after controller results](review-2/after-runs.txt), and
[fixed source/binary hashes](review-2/hashes.sha256) retain the comparison.
The pre-fix kernel is the review-1 kernel in [its hash manifest](review-1/binaries.sha256).
To repeat, use the scratch VM setup above, copy the retained
[controller](review-2/close-probes.py) and [QMP helper](review-2/qmp.py) into
`/tmp/e1k-vm`, name serial logs `serial-close-before.log` or
`serial-close-after.log`, and run `python3 /tmp/e1k-vm/close-probes.py before`
or `after`. Its modes are `popuptest --close-owned`, `--close-full`, and
`--close-ordinary`.

An initial full-queue fixture incorrectly attempted direct popup focus, which
the public API refuses. That attempt was discarded; the retained comparison
uses the corrected nested-child fixture on both kernels.
