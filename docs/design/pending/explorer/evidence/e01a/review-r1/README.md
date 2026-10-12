# PR #254 — first review fixes

Reviewed base: `e0800cb8`. The commit introducing this packet addresses the
[Ctrl punctuation finding](https://github.com/VBWizard/os64/pull/254#discussion_r4242842291)
and the [partial-help finding](https://github.com/VBWizard/os64/pull/254#discussion_r4242842294).
Ordinary re-review remains pending; no approval or merge is claimed.

Ctrl character bytes 1–31 now unfold to their printable ASCII keys, including
`[`, backslash, `]`, `^` and `_`. Physical Escape/editing keys keep their named
identities, and navigation bursts are rejected before normalization. Tests use
both drivers' actual punctuation identities, Shift for caret/underscore, lock
modifiers, repeated downs and release edges; Escape and arrow bursts remain
unclaimed by punctuation actions.

Status paint measures the secondary retained run after reserving notice width
and gap. A complete message must fit the remaining space to appear. This rule
also applies to base information; suppression preserves the messages/tokens.
Pixel comparisons cover positive leftover space, half a message, one pixel short,
exact fit and one pixel extra, with bitmap and real proportional faces.

| Command from the E1A worktree | Result | Evidence |
| --- | --- | --- |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --output /tmp/e01a-r1-before` before production fixes | 313 checks, 54 failures reproducing both findings; exit 1 | [before](before.log) |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --output /tmp/e01a-r1-fake` | 387 checks, 0 failures | [fake](fake.log) |
| `ASAN_OPTIONS=detect_leaks=1 python3 tools/test_ui_action_host.py --real --output /tmp/e01a-r1-real` | 436 checks, 0 failures | [real](real.log) |
| `make -C userland "$PWD/userland/bin/actiongallery" "$PWD/userland/bin/tests/fontsettingstest"` | Strict cross build passed | [cross build](cross-build.log) |

Host runs retained ASan, UBSan and LeakSanitizer, using the established supported
outside-sandbox route. No leak check was disabled. Original UI/Scribe/font
regression evidence remains in the parent packet; this follow-up changes the
action matcher and status paint, without kernel or production consumer changes.
[Source/binary hashes](tested-files.json) identify the review revision separately
from the original implementation's evidence.

QEMU reused the parent's disposable `/tmp/e01a-vm` image preparation and command,
with updated `libos64.so`, `actiongallery` and `fontsettingstest`. The USB keyboard
was present at boot. Launch `actiongallery`, send Ctrl+H twice, switch to VT2,
run `/tests/fontsettingstest 9 > /home/e01a-r1-font32.txt`, then return to VT8.
At the 32px Interface face, the [notice appears alone](notice-priority.png) rather
than alongside a clipped help fragment. Ctrl+N clears the notice and the
[complete retained help returns](help-restored.png). See [font publication](font32.txt)
and [guest serial](qemu.log), which records gallery exit code zero. The VM was
stopped. Log whitespace is normalized for repository checks.
