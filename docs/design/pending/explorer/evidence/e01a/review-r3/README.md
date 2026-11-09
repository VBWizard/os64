# PR #254 — action binding teardown contract

Reviewed base: `8d622f77`. This packet addresses the
[button storage lifetime finding](https://github.com/VBWizard/os64/pull/254#discussion_r4243733256).

The public header incorrectly instructed callers to dispose the widget tree
before detaching action bindings. `os64_ui_action_unbind()` accesses the button,
its binding and UI, so dynamically allocated button storage must remain alive.
The contract now requires waiting for dispatch to return, unbinding buttons,
destroying views and registry, releasing widget/font resources, and then freeing
tree storage. The unbind API comment repeats the live-storage requirement.

The handoff's related teardown paragraph now specifies the same order. It also
corrects the distinction between the binding's view reference (held until
unbind) and invocation's additional reference (held across the callback and
semantic refresh). This matches `gallery_dispose()` and the implementation;
no runtime behavior or test code changed.

The existing lifetime checks cover destruction returning BUSY during callbacks,
unbinding pressed controls, release of view/context/registry references, and
callback-requested close returning through production dispatch before gallery
cleanup. Reused binaries from the R2 packet passed again outside the sandbox:

| Command from the E1A worktree | Result | Evidence |
| --- | --- | --- |
| `ASAN_OPTIONS=detect_leaks=1 /tmp/e01a-r2-fake/test_ui_action userland/libfreetype/fixtures` | 428 checks, 0 failures | [fake](fake.log) |
| `ASAN_OPTIONS=detect_leaks=1 /tmp/e01a-r2-real/test_ui_action userland/libfreetype/fixtures` | 475 checks, 0 failures | [real](real.log) |

ASan, UBSan and LeakSanitizer remained enabled. Runtime source and guest-binary
hashes were verified against the R2 packet; [this record](tested-files.json)
identifies the reused host binaries and corrected header. Sibling lifetime
references and touched comments were checked. `tools/stale_refs.sh` reported
no retired names or new superlative claims for the contract correction.
Ordinary PR re-review remains pending.
