# D5b finished-document fixture

Serve this directory on the development machine:

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory tools/yonder_script_fixture
```

In a QEMU guest using user networking, open
`http://10.0.2.2:8000/index.html` in Yonder. On hardware, use the development
machine's reachable address and bind the test server accordingly.

Settings has **Run page scripts**, off by default. Apply changes this window
and requests a reload when the mode changes. Save as default also persists
`scripts = on/off` in `yonder.conf`. POST reload keeps its resend question.
The status line shows **SCRIPTS ON** while the setting is enabled.

With scripts off, the page says **JavaScript is off**, shows its fallback,
and leaves the field empty. With scripts on, its first inline script captures
a field wrapper and changes the heading. Twenty short fixture tasks provide
about six seconds for human input, with a countdown on the page. Click the
field, or Tab into it, and type **Q**. After the final check, expect
**Two JavaScript donuts!**, **Reference kept; field = Q**, and Q still in the
field. The fallback is absent. Reload to repeat. Each input-window task pauses
for 300 ms, within the existing one-second script deadline. This is a bounded
manual test, not a performance benchmark; scripts run on the UI thread.

For queued-task cancellation, open `cancel.html`, then navigate to another
page while its schedule remains queued. Its first task pauses briefly and
its following small tasks provide time to enter another address. `quiet.html`
is the destination. Console output should contain `J3:cancel-start` and
`J3:cancel-first-ready`, and omit `J3:queued-script-ran` when navigation wins.
The test deliberately does not claim to interrupt a currently running task.

Developer diagnostics can be enabled when launching the browser:

```text
yonder --script-audit http://10.0.2.2:8000/cancel.html > /home/j3.out
```

Page retirement logs `yonder: scripted page retired; heap problems=0` on a
clean teardown. This checks heap integrity; the sanitizer/ledger host suite
and runtime destroy invariant supply the separate leak checks. Close the
browser, start another window, and verify Save as default retained the chosen
mode. `/tests/domtest`, `/tests/pagetest` and `/tests/htmltest` should still pass;
`/tests/htmltest pinned` intentionally exits with the HTML badge.

`tools/test_yonder_scripts_host.sh` tests the actual queue, rebuild and widget
code under ASan/UBSan/LSan with real native libraries and libui editing. OS
services and fonts are hosted; it does not boot a compositor. Set
`YONDER_SCRIPTS_KEEP=1` to keep successful host artifacts.
The suite also covers password deletion reaching scripts and submission,
Reset discarding unflushed edits in its owning form, and author CSS failing to
reveal scripting-mode `noscript` fallback.

This is the D5b/J3 finished-document inline-classic-script fixture. External
scripts, modules, browser execution order, events, timers, document.write,
geometry and navigation APIs are separate work. See
[DOM.md](../../../docs/design/pending/DOM.md#d5b-as-built) for the ownership,
budget and remaining reclamation/event-loop gates.
